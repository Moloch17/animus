/*
 * This file is part of the Animus project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AnimusMod.h"
#include "AnimusPlayerSettings.h"
#include "BotAccounts.h"
#include "BotFactory.h"
#include "Chat.h"
#include "ClassAssets.h"
#include "ClassKit.h"
#include "ClassProfile.h"
#include "FollowPolicy.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MapBlock.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "SceneRegistry.h"
#include "StageDefinition.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <chrono>

namespace
{
    /// The levels move4_follow trained its characters at; a caller outside them is warned, not refused.
    constexpr uint8 TRAINED_LEVEL_MIN = 13;
    constexpr uint8 TRAINED_LEVEL_MAX = 25;

    /// The follower accounts: the bot range's top, clear of the forge's seats (BotAccounts::BASE onwards).
    constexpr uint32 FOLLOWER_ACCOUNT_FIRST = Animus::BotAccounts::BASE + 800000;
    constexpr uint32 FOLLOWER_ACCOUNTS = 100000;

    void SayNotInThisBuild()
    {
        LOG_INFO("module.animus", "mod-animus: bots are followers played by the <class>_follow models (`.animus follow "
            "<n>`, the addon, dungeon party fill). The v1 class companion and Life are not in this build (attic/).");
    }

    /// How long after a map change a leader's group is filled (the group and the map settled by then).
    constexpr int32 FILL_DELAY_MS = 2000;
}

Animus::AnimusMod* Animus::AnimusMod::Instance()
{
    static AnimusMod instance;
    return &instance;
}

void Animus::AnimusMod::LoadConfig()
{
    _config.Load();
    _partyFillOverride.reset();
    SayNotInThisBuild();

    // The camera and the mental map are the process's, set once before any layout is built (the vision block's size
    // comes from the camera's): a reload keeps the ones the server started with.
    if (!_visionConfigured)
    {
        Vision::Configure(_config.Camera);
        Vision::ConfigureMap(_config.MapRun);
        Curriculum::MapBlock::ConfigureCoverage(_config.ClusterMinCells, _config.PocketMinCells);
        _visionConfigured = true;
        Vision::Resolution const render = _config.Render;
        LOG_INFO("module.animus", "Animus camera: {}x{} (cast at {}x{}), fov {}x{}, range {}, zoom {} (min {}), pitch "
            "{}; mental map {} tiles, {} coarse", _config.Camera.Width, _config.Camera.Height, render.Width,
            render.Height, _config.Camera.FovH, _config.Camera.FovV, _config.Camera.Range, _config.Camera.Zoom,
            _config.Camera.ZoomMin, _config.Camera.Pitch, _config.MapRun.Caps.MaxTiles,
            _config.MapRun.Caps.CoarseTiles);
    }
    else
        LOG_INFO("module.animus", "Animus: the camera and mental map settings (Animus.Vision.*, Animus.Map.*) take "
            "effect at the next restart");

    // Models load when a follower or `.animus models` first needs them, from the (new) model directory.
    _models.Reset(_config.Enable ? _config.ModelDir : "");

    if (!_config.Enable)
        LOG_INFO("module.animus", "Animus is disabled (Animus.Enable = 0)");
}

void Animus::AnimusMod::OnStartup()
{
    SayNotInThisBuild();
    if (!_config.Enable)
        return;

    // The camera's static world: each configured map's scene loaded, or baked from the server's data first when it is
    // missing or was built from other data (SceneRegistry::Ensure), on the world thread before any map thread reads.
    std::string const dataDir = sWorld->GetDataPath();
    std::string const sceneDir = _config.SceneDir();
    for (uint32 mapId : _config.VisionMaps)
    {
        auto const start = std::chrono::steady_clock::now();
        Vision::SceneEnsure report;
        std::string error;
        if (!Vision::SceneRegistry::Instance().Ensure(dataDir, sceneDir, mapId, report, error))
        {
            LOG_ERROR("module.animus", "Animus camera scene for map {}: {} (no follower can be summoned there)", mapId,
                error);
            continue;
        }
        double const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        LOG_INFO("module.animus", "Animus camera scene for map {}: {} in {:.1f} s ({} triangles, {} terrain tiles, {})",
            mapId, report.Baked ? "baked (" + report.Reason + ")" : "loaded", seconds, report.Triangles,
            report.TerrainTiles, report.Path);
    }
}

void Animus::AnimusMod::OnUpdate(uint32 diff)
{
    if (!_config.Enable)
    {
        RemoveAll();
        _pendingFill.clear();
        return;
    }

    // The party fills that fell due.
    for (auto itr = _pendingFill.begin(); itr != _pendingFill.end();)
    {
        itr->second -= int32(diff);
        if (itr->second > 0)
        {
            ++itr;
            continue;
        }
        ObjectGuid const leader = itr->first;
        itr = _pendingFill.erase(itr);
        if (Player* player = ObjectAccessor::FindConnectedPlayer(leader))
            FillParty(player);
    }

    if (_parties.empty())
        return;

    std::vector<ObjectGuid> gone;
    for (auto const& [leader, party] : _parties)
        if (party->Update(diff, _config.FollowTuning) == Follow::FollowParty::Status::Dismiss)
            gone.push_back(leader);
    for (ObjectGuid const& leader : gone)
        RemoveParty(leader);

    // A follower a party lost on its own no longer counts.
    std::erase_if(_partyByBot, [](auto const& entry) { return !entry.second->HasBot(entry.first); });
}

void Animus::AnimusMod::OnShutdown()
{
    RemoveAll();
}

bool Animus::AnimusMod::Summon(Player* caller, uint32 count, std::string& message, std::vector<std::string>& warnings)
{
    if (!_config.Enable)
    {
        message = "Animus is disabled.";
        return false;
    }
    if (count < 1 || count > Follow::FollowParty::MAX_FOLLOWERS)
    {
        message = Acore::StringFormat("Summon 1 to {} followers.", Follow::FollowParty::MAX_FOLLOWERS);
        return false;
    }
    if (_parties.contains(caller->GetGUID()))
    {
        message = "You already have followers; `.animus dismiss` them first.";
        return false;
    }
    if (BotFactory::IsAway(caller))
    {
        message = "Followers cannot be summoned on a flight path or a vehicle.";
        return false;
    }
    if (!BotFactory::CanJoin(caller))
    {
        message = "Followers cannot join you in a battleground or arena, or while you are changing maps.";
        return false;
    }

    // The caller's party, which it leads: never a raid (training's party frames are a five-player party's).
    uint32 freeSlots = MAXGROUPSIZE - 1;
    if (Group* group = caller->GetGroup())
    {
        if (group->isRaidGroup() || group->isBGGroup())
        {
            message = "Followers do not join a raid or battleground group.";
            return false;
        }
        if (!group->IsLeader(caller->GetGUID()))
        {
            message = "Only your group's leader can summon followers into it.";
            return false;
        }
        freeSlots = group->GetMembersCount() < MAXGROUPSIZE ? MAXGROUPSIZE - group->GetMembersCount() : 0;
    }
    if (count > freeSlots)
    {
        message = Acore::StringFormat("Your group has room for {} more.", freeSlots);
        return false;
    }

    // The camera needs this map's baked scene (Animus.Vision.Maps).
    if (!Vision::SceneRegistry::Instance().Get(caller->GetMapId()))
    {
        std::string maps;
        for (uint32 mapId : _config.VisionMaps)
            maps += (maps.empty() ? "" : ", ") + std::to_string(mapId);
        message = Acore::StringFormat("There is no camera scene for this map ({}); followers can be summoned on the "
            "maps of Animus.Vision.Maps ({}) whose scenes loaded at startup.", caller->GetMapId(), maps);
        return false;
    }

    uint8 const level = uint8(caller->GetLevel());
    if (level < TRAINED_LEVEL_MIN || level > TRAINED_LEVEL_MAX)
        warnings.push_back(Acore::StringFormat("Warning: the follow models trained at levels {} to {}; yours is {}.",
            TRAINED_LEVEL_MIN, TRAINED_LEVEL_MAX, level));

    // The classes a follower may be: one with a loaded <class>_follow model, a race of the caller's faction, and a
    // starting level at or below the caller's (a death knight is never below 55).
    struct Choice
    {
        Curriculum::Layout const* L;
        std::vector<uint8> Races;
    };
    std::vector<Choice> choices;
    std::string missing;
    for (Curriculum::ClassProfile const& profile : Curriculum::ClassProfiles())
    {
        Curriculum::ClassAssets const& assets = Curriculum::ClassAssets::For(profile);
        if (assets.Kit && assets.Kit->MinLevel() > level)
            continue;
        Choice choice{ &LayoutFor(profile), {} };
        for (uint8 race : assets.Races)
            if (Player::TeamIdForRace(race) == caller->GetTeamId())
                choice.Races.push_back(race);
        std::string error;
        if (choice.Races.empty())
            continue;
        if (!_models.Find(*choice.L, error))
        {
            missing += (missing.empty() ? "" : ", ") + choice.L->ModelName();
            continue;
        }
        choices.push_back(std::move(choice));
    }
    if (choices.empty())
    {
        message = Acore::StringFormat("No class has a {} model this server can play{}; see `.animus models`.",
            "follow", missing.empty() ? "" : " (not loading: " + missing + ")");
        return false;
    }

    // Every follower's class and its model first, so a model that will not run refuses the summon before any bot is
    // made.
    struct Planned
    {
        Choice const* C;
        std::unique_ptr<Follow::FollowPolicy> Policy;
    };
    std::vector<Planned> planned;
    for (uint32 i = 0; i < count; ++i)
    {
        Choice const& choice = choices[urand(0, uint32(choices.size()) - 1)];
        std::string loadError;
        std::unique_ptr<Follow::FollowPolicy> policy = Follow::MakeFollowPolicy(choice.L->ModelName(),
            _models.Find(*choice.L, loadError));
        if (!policy)
        {
            message = Acore::StringFormat("The {} model cannot be run by this build (see the server log).",
                choice.L->ModelName());
            return false;
        }
        planned.push_back({ &choice, std::move(policy) });
    }

    auto party = std::make_unique<Follow::FollowParty>(caller->GetGUID());
    std::vector<std::string> joined;
    for (Planned& plan : planned)
    {
        std::string const name = Follow::FollowParty::NewName();
        if (name.empty())
        {
            LOG_ERROR("module.animus", "No free name for a follower of {}", caller->GetName());
            break;
        }
        uint32 const account = FOLLOWER_ACCOUNT_FIRST + (_nextAccount++ % FOLLOWER_ACCOUNTS);
        uint8 const race = plan.C->Races[urand(0, uint32(plan.C->Races.size()) - 1)];
        std::string said;
        if (!party->Add(caller, *plan.C->L, std::move(plan.Policy), race, name, account, said))
        {
            LOG_ERROR("module.animus", "Summoning {}'s followers: {}", caller->GetName(), said);
            warnings.push_back(said);
            break;
        }
        joined.push_back(said);
    }
    if (!party->Size())
    {
        message = "No follower could be summoned; see the server log.";
        return false;
    }

    for (ObjectGuid const& bot : party->GetBotGUIDs())
        _partyByBot[bot] = party.get();
    _parties[caller->GetGUID()] = std::move(party);

    message.clear();
    for (std::string const& line : joined)
        message += (message.empty() ? "" : " ") + line;
    return true;
}

bool Animus::AnimusMod::Dismiss(Player* caller, std::string& message)
{
    if (!_parties.contains(caller->GetGUID()))
    {
        message = "You have no followers.";
        return false;
    }
    RemoveParty(caller->GetGUID());
    message = "Your followers are gone.";
    return true;
}

std::vector<std::string> Animus::AnimusMod::List(Player* caller) const
{
    auto const party = _parties.find(caller->GetGUID());
    return party == _parties.end() ? std::vector<std::string>() : party->second->Describe();
}

std::vector<std::string> Animus::AnimusMod::ModelStatus()
{
    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat("Stage {}, models in {}:", _config.CurriculumStage, _config.ModelDir));
    uint32 loaded = 0;
    for (Curriculum::ClassProfile const& profile : Curriculum::ClassProfiles())
    {
        Curriculum::Layout const& layout = LayoutFor(profile);
        std::string error;
        bool const ok = _models.Find(layout, error) != nullptr;
        loaded += ok ? 1 : 0;
        lines.push_back(Acore::StringFormat("  {}: {}", layout.ModelName(), ok ? "loads" : error));
    }
    std::string scenes;
    for (auto const& [mapId, checksum] : Vision::SceneRegistry::Instance().Checksums())
        scenes += (scenes.empty() ? "" : ", ") + std::to_string(mapId);
    lines.push_back(Acore::StringFormat("{} of {} load. Camera scenes loaded for maps: {}.", loaded,
        Curriculum::ClassProfiles().size(), scenes.empty() ? "none" : scenes));
    return lines;
}

void Animus::AnimusMod::OnPlayerLogout(Player* player)
{
    _filledInstance.erase(player->GetGUID());
    _pendingFill.erase(player->GetGUID());
    if (_parties.contains(player->GetGUID()))
        RemoveParty(player->GetGUID());
    PlayerSettings::Forget(player->GetGUID());
}

void Animus::AnimusMod::OnPlayerLogin(Player* player)
{
    if (player && !IsBot(player->GetGUID()))
        PlayerSettings::Load(player->GetGUID());
}

bool Animus::AnimusMod::MaySummon(Player const* player) const
{
    return player && player->GetSession() && uint32(player->GetSession()->GetSecurity()) >= _config.FollowSecurity;
}

bool Animus::AnimusMod::PartyFillFor(Player const* player) const
{
    if (!_config.Enable || !_partyFillOverride.value_or(_config.PartyFill) || !player)
        return false;
    return PlayerSettings::PartyFill(player->GetGUID());
}

bool Animus::AnimusMod::SetPartyFill(Player* player, bool on, std::string& message)
{
    if (!player)
    {
        message = "No player.";
        return false;
    }
    PlayerSettings::SetPartyFill(player->GetGUID(), on);
    if (!on)
        _pendingFill.erase(player->GetGUID());
    PartyFillStatus const status = PartyFillState(player);
    message = on ? "Your dungeon party will be filled with followers." : "Your dungeon party will not be filled with "
        "followers; those already with you stay until you leave their dungeon.";
    if (on && !status.Realm)
        message += " (The realm has party fill off right now.)";
    return true;
}

bool Animus::AnimusMod::PartyFillRealm(bool on, std::string& message)
{
    _partyFillOverride = on;
    if (!on)
        _pendingFill.clear();
    message = Acore::StringFormat("Dungeon party fill is {} for every player until the next config reload (the "
        "config says {}).", on ? "on" : "off", _config.PartyFill ? "on" : "off");
    return true;
}

Animus::AnimusMod::PartyFillStatus Animus::AnimusMod::PartyFillState(Player const* player) const
{
    PartyFillStatus status;
    status.Config = _config.PartyFill;
    status.Overridden = _partyFillOverride.has_value();
    status.Realm = _config.Enable && _partyFillOverride.value_or(_config.PartyFill);
    status.Mine = player ? PlayerSettings::PartyFill(player->GetGUID()) : true;
    return status;
}

void Animus::AnimusMod::OnPlayerMapChanged(Player* player)
{
    if (!player || IsBot(player->GetGUID()))
        return;

    ObjectGuid const guid = player->GetGUID();
    Map* map = player->GetMap();
    bool const dungeon = map && map->IsDungeon() && !map->IsRaid();

    // Leaving the dungeon the group was filled for, the followers go. Not as a ghost: releasing in a dungeon puts
    // the player outside at a graveyard, and it runs back in to its corpse.
    if (auto const filled = _filledInstance.find(guid); filled != _filledInstance.end()
        && (!dungeon || map->GetInstanceId() != filled->second) && player->IsAlive())
    {
        _filledInstance.erase(filled);
        if (_parties.contains(guid))
        {
            RemoveParty(guid);
            ChatHandler(player->GetSession()).SendSysMessage("Your followers stay behind in the dungeon.");
        }
    }

    if (PartyFillFor(player) && dungeon && !_filledInstance.contains(guid) && !_parties.contains(guid))
        _pendingFill[guid] = FILL_DELAY_MS;
}

void Animus::AnimusMod::FillParty(Player* leader)
{
    Map* map = leader->GetMap();
    if (!map || !map->IsDungeon() || map->IsRaid() || !PartyFillFor(leader) || _parties.contains(leader->GetGUID()))
        return;
    // Alone, or the group's leader; the group never a raid.
    uint32 members = 1;
    if (Group* group = leader->GetGroup())
    {
        if (!group->IsLeader(leader->GetGUID()) || group->isRaidGroup() || group->isBGGroup())
            return;
        members = group->GetMembersCount();
    }
    if (members >= MAXGROUPSIZE)
        return;
    uint32 const count = std::min<uint32>(MAXGROUPSIZE - members, Follow::FollowParty::MAX_FOLLOWERS);
    std::string message;
    std::vector<std::string> warnings;
    ChatHandler chat(leader->GetSession());
    if (!Summon(leader, count, message, warnings))
    {
        LOG_INFO("module.animus", "Party fill for {} in map {}: {}", leader->GetName(), map->GetId(), message);
        chat.SendSysMessage(Acore::StringFormat("Your dungeon party was not filled: {}", message));
        return;
    }
    _filledInstance[leader->GetGUID()] = map->GetInstanceId();
    for (std::string const& warning : warnings)
        chat.SendSysMessage(warning);
    chat.SendSysMessage(Acore::StringFormat("Your dungeon party is filled: {}", message));
}

void Animus::AnimusMod::RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type)
{
    // What a follower took, as the forge's EnvPool::RecordDamage counts a seat's DamageTaken: aimed damage, not its
    // own (drowning, falls). What it dealt is not counted: a follow env has no targets (FollowParty::TrackStep).
    if (_partyByBot.empty() || !attacker || !victim || !damage
        || (type != DIRECT_DAMAGE && type != SPELL_DIRECT_DAMAGE && type != DOT))
        return;
    if (auto const taker = _partyByBot.find(victim->GetGUID()); taker != _partyByBot.end())
        taker->second->RecordDamageTaken(victim->GetGUID(), damage);
}

void Animus::AnimusMod::RemoveParty(ObjectGuid leader)
{
    auto const itr = _parties.find(leader);
    if (itr == _parties.end())
        return;

    // Unlink before destroying: logging the bots out re-enters the module through OnPlayerLogout.
    std::unique_ptr<Follow::FollowParty> const party = std::move(itr->second);
    _parties.erase(itr);
    std::erase_if(_partyByBot, [&](auto const& entry) { return entry.second == party.get(); });
    party->DestroyAll();
}

void Animus::AnimusMod::RemoveAll()
{
    while (!_parties.empty())
        RemoveParty(_parties.begin()->first);
}

Animus::Curriculum::Layout const& Animus::AnimusMod::LayoutFor(Curriculum::ClassProfile const& profile)
{
    // AnimusConfig::Load only keeps a stage that exists; the first stage stands in should that ever not hold.
    Curriculum::StageDefinition const* found = Curriculum::FindStage(_config.CurriculumStage);
    if (!found)
        found = &Curriculum::CurriculumStages().front();
    Curriculum::StageDefinition const& stage = *found;
    std::string const name = profile.Name + stage.Suffix;
    auto itr = _layouts.find(name);
    if (itr == _layouts.end())
    {
        // Builds the class's assets on first use (trainer data and item pools: a few seconds).
        itr = _layouts.emplace(name, Curriculum::Layout::Build(profile, stage)).first;
        LOG_INFO("module.animus", "Animus built the {} layout (obs {}, actions {})", name, itr->second.ObsDim,
            itr->second.NumActions);
    }
    return itr->second;
}

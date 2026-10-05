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
#include "LifeService.h"
#include "AccountMgr.h"
#include "AnimusAddon.h"
#include "BotAccounts.h"
#include "BotFactory.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "CompanionLoader.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "ClassAssets.h"
#include "ClassProfile.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StageDefinition.h"
#include "StringFormat.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <vector>

namespace
{
    struct NamedId
    {
        std::string_view Name;
        uint8 Id;
    };

    /// What a companion may be, first name of each id shown in messages. Matched case-insensitively, ignoring
    /// spaces, underscores, hyphens and apostrophes (night_elf, Night-Elf and nightelf are the same).
    constexpr std::array<NamedId, 12> RACE_NAMES =
    { {
        { "human", RACE_HUMAN }, { "dwarf", RACE_DWARF }, { "nightelf", RACE_NIGHTELF }, { "gnome", RACE_GNOME },
        { "draenei", RACE_DRAENEI }, { "orc", RACE_ORC }, { "undead", RACE_UNDEAD_PLAYER },
        { "forsaken", RACE_UNDEAD_PLAYER }, { "tauren", RACE_TAUREN }, { "troll", RACE_TROLL },
        { "bloodelf", RACE_BLOODELF }, { "scourge", RACE_UNDEAD_PLAYER },
    } };

    constexpr std::array<NamedId, 11> CLASS_NAMES =
    { {
        { "warrior", CLASS_WARRIOR }, { "paladin", CLASS_PALADIN }, { "hunter", CLASS_HUNTER }, { "rogue", CLASS_ROGUE },
        { "priest", CLASS_PRIEST }, { "deathknight", CLASS_DEATH_KNIGHT }, { "shaman", CLASS_SHAMAN },
        { "mage", CLASS_MAGE }, { "warlock", CLASS_WARLOCK }, { "druid", CLASS_DRUID }, { "dk", CLASS_DEATH_KNIGHT },
    } };

    std::string Normalize(std::string_view text)
    {
        std::string normal;
        for (char c : text)
            if (c != ' ' && c != '_' && c != '-' && c != '\'')
                normal.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        return normal;
    }

    template <std::size_t N>
    std::optional<uint8> FindNamed(std::array<NamedId, N> const& names, std::string_view text)
    {
        std::string const wanted = Normalize(text);
        for (NamedId const& entry : names)
            if (entry.Name == wanted)
                return entry.Id;
        return std::nullopt;
    }

    template <std::size_t N>
    std::string NameOf(std::array<NamedId, N> const& names, uint8 id)
    {
        for (NamedId const& entry : names)
            if (entry.Id == id)
                return std::string(entry.Name);
        return "?";
    }

    /// Every id once, under its first name, in the order of the table.
    template <std::size_t N>
    std::vector<NamedId> Unique(std::array<NamedId, N> const& names)
    {
        std::vector<NamedId> unique;
        for (NamedId const& entry : names)
            if (std::none_of(unique.begin(), unique.end(), [&](NamedId const& seen) { return seen.Id == entry.Id; }))
                unique.push_back(entry);
        return unique;
    }

    /// Every id's first name, for messages.
    template <std::size_t N>
    std::string Names(std::array<NamedId, N> const& names)
    {
        std::string list;
        for (NamedId const& entry : Unique(names))
            list += (list.empty() ? "" : ", ") + std::string(entry.Name);
        return list;
    }

    /// The companion character's cache entry. A character that left the cache but not the database (a companion
    /// put away by a build that still forgot it) is read back from its row and cached again; null only when the
    /// character is really gone.
    CharacterCacheEntry const* CharacterOf(ObjectGuid bot)
    {
        if (CharacterCacheEntry const* character = sCharacterCache->GetCharacterCacheByGuid(bot))
            return character;

        QueryResult const result = CharacterDatabase.Query(
            "SELECT name, account, race, gender, class, level FROM characters WHERE guid = {}", bot.GetCounter());
        if (!result)
            return nullptr;

        Field const* fields = result->Fetch();
        sCharacterCache->AddCharacterCacheEntry(bot, fields[1].Get<uint32>(), fields[0].Get<std::string>(),
            fields[3].Get<uint8>(), fields[2].Get<uint8>(), fields[4].Get<uint8>(), fields[5].Get<uint8>());
        LOG_INFO("module.animus", "Companion {} ({}) was missing from the character cache; read back from the "
            "database", fields[0].Get<std::string>(), bot.ToString());
        return sCharacterCache->GetCharacterCacheByGuid(bot);
    }
}

Animus::AnimusMod* Animus::AnimusMod::Instance()
{
    static AnimusMod instance;
    return &instance;
}

void Animus::AnimusMod::LoadConfig()
{
    _config.Load();
    sLife->Configure(_config.Life);

    // Models load when a companion or a stage seat first needs them, from the (new) model directory.
    _models.Reset(_config.Enable ? _config.ModelDir : "");

    if (!_config.Enable)
        LOG_INFO("module.animus", "Animus is disabled (Animus.Enable = 0)");
}

void Animus::AnimusMod::OnStartup()
{
    // After the character cache (World::SetInitialWorldSettings loads it long after the config): a record whose
    // character is gone is recognised as such, not every record.
    _registry.Load();
}

void Animus::AnimusMod::OnUpdate(uint32 diff)
{
    CompanionLoader::Update();

    if (!_pendingFill.empty())
    {
        std::vector<ObjectGuid> due;
        for (auto& [owner, ms] : _pendingFill)
            if ((ms -= int32(diff)) <= 0)
                due.push_back(owner);
        for (ObjectGuid const& owner : due)
        {
            _pendingFill.erase(owner);
            Player* player = ObjectAccessor::FindPlayer(owner);
            if (player && player->IsInWorld() && !player->IsBeingTeleported() && _config.Enable && _config.PartyFill)
                FillParty(player);
        }
    }

    if (_parties.empty())
        return;

    if (!_config.Enable)
    {
        RemoveAll();
        return;
    }

    CompanionParty::Settings const settings{ _config.CurriculumDecisionMs, _config.CurriculumActions,
        _config.CurriculumOptions, _config.CurriculumDirector, DirectorLayout() };
    std::vector<ObjectGuid> gone;
    for (auto const& [owner, party] : _parties)
    {
        // No model, no companion: one whose model is gone (a reload that refuses it) is saved and despawned.
        for (CompanionParty::Modelless const& lost : party->WithoutModel(_models))
        {
            LOG_ERROR("module.animus", "Companion {} despawned: its model is not available ({})", lost.Name,
                lost.Error);
            if (!lost.Temporary)
                if (CompanionRegistry::Record* record = _registry.Find(owner))
                {
                    party->Save(*record);
                    _registry.Update(*record);
                }
            std::string message;
            party->Remove(lost.Name, message);
            if (Player* player = ObjectAccessor::FindPlayer(owner))
            {
                std::string const told = Acore::StringFormat("{} left: there is no model this server can play for it "
                    "({}).", lost.Name, lost.Error);
                ChatHandler(player->GetSession()).SendSysMessage(told);
                Addon::Push(player, told);
            }
        }
        if (!party->Size() || party->Update(diff, settings, _models) == CompanionParty::Status::Dismiss)
            gone.push_back(owner);
    }

    for (ObjectGuid const& owner : gone)
        RemoveParty(owner);

    // A companion a party lost on its own (not dismissed) no longer counts damage.
    std::erase_if(_partyByBot, [](auto const& entry) { return !entry.second->HasBot(entry.first); });
}

void Animus::AnimusMod::OnShutdown()
{
    RemoveAll();
}

Animus::CompanionParty* Animus::AnimusMod::PartyOf(Player* owner, std::string& message)
{
    auto const party = _parties.find(owner->GetGUID());
    if (party == _parties.end() || !party->second->Companions())
    {
        message = _registry.Find(owner->GetGUID()) ? "Your companion is not summoned." : "You have no companion.";
        return nullptr;
    }
    return party->second.get();
}

bool Animus::AnimusMod::ResolveRaceClass(Player const* owner, std::string_view race, std::string_view playerClass,
    uint8& raceId, uint8& classId, std::string& message) const
{
    std::optional<uint8> const raceFound = FindNamed(RACE_NAMES, race);
    if (!raceFound)
    {
        message = Acore::StringFormat("Unknown race {}. Races: {}.", race, Names(RACE_NAMES));
        return false;
    }

    std::optional<uint8> const classFound = FindNamed(CLASS_NAMES, playerClass);
    if (!classFound)
    {
        message = Acore::StringFormat("Unknown class {}. Classes: {}.", playerClass, Names(CLASS_NAMES));
        return false;
    }

    if (!Curriculum::ClassAssets::FindProfile(*classFound))
    {
        message = Acore::StringFormat("There is no class profile for {}.", NameOf(CLASS_NAMES, *classFound));
        return false;
    }

    if (!sObjectMgr->GetPlayerInfo(*raceFound, *classFound))
    {
        message = Acore::StringFormat("A {} cannot be a {}.", NameOf(RACE_NAMES, *raceFound),
            NameOf(CLASS_NAMES, *classFound));
        return false;
    }

    if (Player::TeamIdForRace(*raceFound) != owner->GetTeamId())
    {
        message = Acore::StringFormat("A {} is not of your faction.", NameOf(RACE_NAMES, *raceFound));
        return false;
    }

    raceId = *raceFound;
    classId = *classFound;
    return true;
}

bool Animus::AnimusMod::Create(Player* owner, std::string name, std::string_view race, std::string_view playerClass,
    std::string& message)
{
    if (!_config.Enable)
    {
        message = "Animus is disabled.";
        return false;
    }

    if (_registry.Find(owner->GetGUID()))
    {
        message = "You already have a companion. Change its race and class, or its name, instead.";
        return false;
    }

    uint8 raceId = 0;
    uint8 classId = 0;
    if (!CompanionRegistry::CheckName(name, message) || !ResolveRaceClass(owner, race, playerClass, raceId, classId,
        message))
        return false;

    // No companion can ride a flight path or a vehicle with you, and no bot can be placed while you are between
    // maps; a battleground takes only queued players.
    if (BotFactory::IsAway(owner))
    {
        message = "A companion cannot be created on a flight path or a vehicle; wait until you are off it.";
        return false;
    }

    if (!BotFactory::CanJoin(owner))
    {
        message = "A companion cannot join you in a battleground or arena, or while you are changing maps.";
        return false;
    }

    Curriculum::ClassProfile const& profile = *Curriculum::ClassAssets::FindProfile(classId);
    Curriculum::Layout const& layout = LayoutFor(profile);
    if (!HasModel(layout, message))
        return false;

    uint32 const account = _registry.AccountFor(owner);
    if (!account)
    {
        message = "The companion's account could not be created; see the server log.";
        return false;
    }

    std::unique_ptr<CompanionParty>& party = _parties[owner->GetGUID()];
    if (!party)
        party = std::make_unique<CompanionParty>(owner->GetGUID());

    if (!party->Add(owner, layout, raceId, name, account, message))
    {
        if (!party->Size())
            _parties.erase(owner->GetGUID());
        return false;
    }

    // Written as a login's character is. The save's statements exist on the asynchronous connection only, so the
    // transaction goes through the worker; a load that follows queues behind it there.
    // The member just added: the last, after any temporary ones the owner's dungeon already has.
    Player* bot = ObjectAccessor::FindConnectedPlayer(party->GetBotGUIDs().back());
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    bot->SaveToDB(trans, true, false);
    CharacterDatabase.CommitTransaction(trans);

    CompanionRegistry::Record& record = _registry.Insert(owner->GetGUID(), account, bot->GetGUID(), 0);
    party->Save(record);
    _registry.Update(record);
    for (ObjectGuid const& guid : party->GetBotGUIDs())
        _partyByBot[guid] = party.get();

    return true;
}

bool Animus::AnimusMod::Adopt(Player* owner, Player* bot, CompanionRegistry::Record& record, std::string& message)
{
    Curriculum::ClassProfile const* profile = Curriculum::ClassAssets::FindProfile(bot->getClass());
    if (!profile)
    {
        message = "The companion's class has no profile any more.";
        return false;
    }

    Curriculum::Layout const& layout = LayoutFor(*profile);
    if (!HasModel(layout, message))
        return false;

    std::unique_ptr<CompanionParty>& party = _parties[owner->GetGUID()];
    if (!party)
        party = std::make_unique<CompanionParty>(owner->GetGUID());

    if (!party->Attach(owner, bot, layout, record, message))
    {
        if (!party->Size())
            _parties.erase(owner->GetGUID());
        return false;
    }

    for (ObjectGuid const& guid : party->GetBotGUIDs())
        _partyByBot[guid] = party.get();
    return true;
}

bool Animus::AnimusMod::HasModel(Curriculum::Layout const& layout, std::string& message)
{
    // No model, no companion: there is no fallback movement and no scripted play, so a companion whose class model
    // is missing, or was trained on another layout (an older move block among them), is not made, summoned or kept.
    std::string error;
    if (_models.Find(layout, error))
        return true;
    message = Acore::StringFormat("There is no {} model this server can play ({}), so no companion of that class can "
        "join you.", layout.ModelName(), error);
    return false;
}

bool Animus::AnimusMod::Summon(Player* owner, std::string& message)
{
    if (!_config.Enable)
    {
        message = "Animus is disabled.";
        return false;
    }

    CompanionRegistry::Record* record = _registry.Find(owner->GetGUID());
    if (!record)
    {
        message = "You have no companion. Create one first.";
        return false;
    }

    if (record->Loading)
    {
        message = "Your companion is on its way.";
        return false;
    }

    if (auto const party = _parties.find(owner->GetGUID()); party != _parties.end() && party->second->Companions())
    {
        message = "Your companion is already with you.";
        return false;
    }

    if (BotFactory::IsAway(owner))
    {
        message = "A companion cannot be summoned on a flight path or a vehicle; wait until you are off it.";
        return false;
    }

    if (!BotFactory::CanJoin(owner))
    {
        message = "A companion cannot join you in a battleground or arena, or while you are changing maps.";
        return false;
    }

    CharacterCacheEntry const* character = CharacterOf(record->Bot);
    if (!character)
    {
        message = "Your companion's character is gone; create a new one.";
        _registry.Erase(owner->GetGUID(), false);
        return false;
    }

    // No model, no companion: refused before its character is loaded (Adopt asks again once it is).
    if (Curriculum::ClassProfile const* profile = Curriculum::ClassAssets::FindProfile(character->Class))
        if (!HasModel(LayoutFor(*profile), message))
            return false;

    // Neither reaches a companion: whatever arrived while it was away goes before it loads.
    CompanionRegistry::Purge(record->Bot);

    record->Loading = true;
    ObjectGuid const ownerGuid = owner->GetGUID();
    CompanionLoader::Begin(record->Bot, record->Account, character->Name, [this, ownerGuid](Player* bot)
    {
        CompanionRegistry::Record* record = _registry.Find(ownerGuid);
        if (record)
            record->Loading = false;

        Player* owner = ObjectAccessor::FindPlayer(ownerGuid);
        std::string message;
        if (!bot)
            message = "Your companion could not be loaded; see the server log.";
        else if (!record || !owner || !owner->IsInWorld())
        {
            CompanionLoader::Discard(bot);
            return;
        }
        else if (!Adopt(owner, bot, *record, message))
            CompanionLoader::Discard(bot);

        if (owner)
        {
            ChatHandler(owner->GetSession()).SendSysMessage(message);
            Addon::Push(owner, message);
        }
    });

    message = Acore::StringFormat("{} is on the way.", character->Name);
    return true;
}

void Animus::AnimusMod::SaveParty(ObjectGuid owner)
{
    auto const party = _parties.find(owner);
    if (party == _parties.end())
        return;

    if (CompanionRegistry::Record* record = _registry.Find(owner))
    {
        party->second->Save(*record);
        _registry.Update(*record);
    }
    RemoveParty(owner);
}

bool Animus::AnimusMod::Dismiss(Player* owner, std::string& message)
{
    if (!PartyOf(owner, message))
        return false;

    PutAway(owner->GetGUID(), true);
    message = "Companion dismissed.";
    return true;
}

bool Animus::AnimusMod::Rename(Player* owner, std::string name, std::string& message)
{
    CompanionRegistry::Record* record = _registry.Find(owner->GetGUID());
    if (!record)
    {
        message = "You have no companion.";
        return false;
    }

    if (record->Loading)
    {
        message = "Wait for your companion to arrive first.";
        return false;
    }

    if (!CompanionRegistry::CheckName(name, message))
        return false;

    // The character is renamed in the database, which needs it out of the world: a summoned one goes and comes
    // back under its new name, which is also how the owner's client learns it.
    std::string ignored;
    bool const wasOut = PartyOf(owner, ignored) != nullptr;
    if (wasOut)
        PutAway(owner->GetGUID(), true);

    CompanionRegistry::Rename(*record, name);
    message = Acore::StringFormat("Your companion is now called {}.", name);
    LOG_INFO("module.animus", "{} renamed companion {} to {}", owner->GetName(), record->Bot.ToString(), name);

    if (wasOut)
    {
        std::string again;
        Summon(owner, again);
    }
    return true;
}

bool Animus::AnimusMod::Reroll(Player* owner, std::string_view race, std::string_view playerClass,
    std::string& message)
{
    CompanionRegistry::Record* record = _registry.Find(owner->GetGUID());
    if (!record)
    {
        message = "You have no companion. Create one first.";
        return false;
    }

    if (record->Loading)
    {
        message = "Wait for your companion to arrive first.";
        return false;
    }

    uint8 raceId = 0;
    uint8 classId = 0;
    if (!ResolveRaceClass(owner, race, playerClass, raceId, classId, message))
        return false;

    CharacterCacheEntry const* character = CharacterOf(record->Bot);
    if (!character)
    {
        message = "Your companion's character is gone; create a new one.";
        _registry.Erase(owner->GetGUID(), false);
        return false;
    }

    if (BotFactory::IsAway(owner) || !BotFactory::CanJoin(owner))
    {
        message = "The new companion could not be placed beside you here; try again on the ground, out of a "
            "battleground.";
        return false;
    }

    // The old character goes, unsaved (nothing of it is kept), and a new one of the same name takes its place.
    std::string const name = character->Name;
    PutAway(owner->GetGUID(), false);
    CompanionRegistry::DeleteCharacter(*record);
    _registry.Erase(owner->GetGUID(), false);
    LOG_INFO("module.animus", "{} rerolled companion {} as a {} {}", owner->GetName(), name, race, playerClass);

    if (!Create(owner, name, race, playerClass, message))
    {
        message = "Your old companion is gone and the new one could not be created: " + message;
        return false;
    }
    return true;
}

void Animus::AnimusMod::OnOwnerDeleted(ObjectGuid owner)
{
    CompanionRegistry::Record const* record = _registry.Find(owner);
    if (!record)
        return;

    RemoveParty(owner);
    LOG_INFO("module.animus", "Owner {} was deleted: deleting companion {} and its account {}", owner.ToString(),
        record->Bot.ToString(), record->Account);
    CompanionRegistry::DeleteCharacter(*record);
    _registry.Erase(owner, true);
}

std::vector<std::string> Animus::AnimusMod::PurgeAll()
{
    // Out of the world first, unsaved: AccountMgr::DeleteAccount kicks a character's session, which a socketless
    // one never answers, and deleting the rows under a character still in the world is not something to try.
    std::size_t const out = _parties.size();
    while (!_parties.empty())
        RemoveParty(_parties.begin()->first);
    _registry.Clear();

    std::vector<std::string> lines;
    if (out)
        lines.push_back(Acore::StringFormat("{} companion{} sent away unsaved.", out, out == 1 ? "" : "s"));
    lines.push_back("Table animus_companion dropped.");

    QueryResult accounts = LoginDatabase.Query("SELECT id, username FROM account WHERE username LIKE 'ANIMUS%'");
    if (!accounts)
    {
        lines.push_back("No Animus accounts.");
        return lines;
    }

    do
    {
        Field* fields = accounts->Fetch();
        uint32 const account = fields[0].Get<uint32>();
        std::string const username = fields[1].Get<std::string>();

        // The account's characters go with it (Player::DeleteFromDB, finally), then the account.
        uint32 characters = 0;
        if (QueryResult names = CharacterDatabase.Query("SELECT name FROM characters WHERE account = {}", account))
            characters = uint32(names->GetRowCount());

        AccountOpResult const result = AccountMgr::DeleteAccount(account);
        lines.push_back(Acore::StringFormat("Account {} ({}) with {} character{}: {}.", username, account, characters,
            characters == 1 ? "" : "s", result == AOR_OK ? "deleted" : "could not be deleted"));
        LOG_INFO("module.animus", "Purge: account {} ({}) with {} characters {}", username, account, characters,
            result == AOR_OK ? "deleted" : "not deleted");
    } while (accounts->NextRow());

    return lines;
}

bool Animus::AnimusMod::Talent(Player* owner, std::string_view name, uint32 talentId, bool learn,
    std::string& message)
{
    CompanionParty* party = PartyOf(owner, message);
    return party && party->Talent(name, talentId, learn, message);
}

bool Animus::AnimusMod::PetTalent(Player* owner, std::string_view name, uint32 talentId, bool learn,
    std::string& message)
{
    CompanionParty* party = PartyOf(owner, message);
    return party && party->PetTalent(name, talentId, learn, message);
}

bool Animus::AnimusMod::Equip(Player* owner, std::string_view name, uint8 bag, uint8 slot, uint8 equipSlot,
    std::string& message)
{
    CompanionParty* party = PartyOf(owner, message);
    return party && party->Equip(owner, name, bag, slot, equipSlot, message);
}

bool Animus::AnimusMod::Pet(Player* owner, std::string_view name, CompanionParty::PetView& view,
    std::string& message)
{
    CompanionParty* party = PartyOf(owner, message);
    return party && party->Pet(name, view, message);
}

Animus::AnimusMod::Companion Animus::AnimusMod::Describe(Player* owner)
{
    Companion companion;
    CompanionRegistry::Record const* record = _registry.Find(owner->GetGUID());
    CharacterCacheEntry const* character = record ? CharacterOf(record->Bot) : nullptr;
    if (!character)
        return companion;

    companion.Exists = true;
    companion.Name = character->Name;
    companion.Race = NameOf(RACE_NAMES, character->Race);
    companion.Class = NameOf(CLASS_NAMES, character->Class);
    companion.Level = character->Level;
    companion.Loading = record->Loading;
    companion.Out = record->Loading;
    if (Curriculum::ClassProfile const* profile = Curriculum::ClassAssets::FindProfile(character->Class))
        if (record->Spec < profile->Specs.size())
            companion.Spec = profile->Specs[record->Spec].Name;

    auto const party = _parties.find(owner->GetGUID());
    if (party != _parties.end())
        for (CompanionParty::Summary const& summary : party->second->Summarize(_models))
        {
            companion.Out = true;
            companion.Level = summary.Level;
            companion.Spec = summary.Spec;
            companion.Parked = summary.Parked;
            companion.ModelName = summary.ModelName;
            companion.Model = summary.Model;
        }
    return companion;
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
    lines.push_back(Acore::StringFormat("{} of {} load. Ground probe: {}{}.", loaded, Curriculum::ClassProfiles().size(),
        _config.ProbeSource, _config.ProbeSource == "live" ? "" : " from " + _config.ProbeDir));
    return lines;
}

std::vector<std::string> Animus::AnimusMod::List(Player* owner)
{
    Companion const companion = Describe(owner);
    if (!companion.Exists)
        return {};

    std::vector<std::string> lines;
    lines.push_back(Acore::StringFormat("{}: level {} {} {} ({}), {}", companion.Name, companion.Level,
        companion.Race, companion.Class, companion.Spec,
        companion.Loading ? "on the way" : companion.Out ? "with you" : "waiting to be summoned"));
    if (companion.Out && !companion.Loading)
        lines.push_back(Acore::StringFormat("model {}: {}{}", companion.ModelName, companion.Model,
            companion.Parked ? " (waiting for you to land)" : ""));
    return lines;
}

std::vector<Animus::AnimusMod::RaceChoice> Animus::AnimusMod::RaceChoices(Player const* owner)
{
    std::vector<RaceChoice> choices;
    for (NamedId const& race : Unique(RACE_NAMES))
    {
        if (Player::TeamIdForRace(race.Id) != owner->GetTeamId())
            continue;

        RaceChoice& choice = choices.emplace_back();
        choice.Race = race.Name;
        for (NamedId const& playerClass : Unique(CLASS_NAMES))
            if (sObjectMgr->GetPlayerInfo(race.Id, playerClass.Id))
                choice.Classes.emplace_back(playerClass.Name);
    }
    return choices;
}

std::vector<std::string> Animus::AnimusMod::StageList() const
{
    std::vector<std::string> lines;
    for (Curriculum::StageDefinition const& stage : Curriculum::CurriculumStages())
    {
        std::string arenas;
        for (Curriculum::ArenaDefinition const& arena : stage.Arenas)
            arenas += (arenas.empty() ? "" : ", ") + arena.Name;

        lines.push_back(Acore::StringFormat("{}: {} (arenas: {})", stage.Name, stage.Summary, arenas));
    }
    return lines;
}

void Animus::AnimusMod::OnPlayerLogout(Player* player)
{
    _filledInstance.erase(player->GetGUID());
    _pendingFill.erase(player->GetGUID());
    SaveParty(player->GetGUID());
}

void Animus::AnimusMod::PutAway(ObjectGuid owner, bool save)
{
    auto const party = _parties.find(owner);
    if (party == _parties.end())
        return;
    if (!party->second->HasFillers())
    {
        if (save)
            SaveParty(owner);
        else
            RemoveParty(owner);
        return;
    }

    // The companion goes; the temporary members stay with the owner until it leaves their dungeon.
    if (CompanionRegistry::Record* record = _registry.Find(owner); record && save)
    {
        party->second->Save(*record);
        _registry.Update(*record);
    }
    party->second->RemoveMembers(false);
    std::erase_if(_partyByBot, [](auto const& entry) { return !entry.second->HasBot(entry.first); });
}

void Animus::AnimusMod::OnPlayerMapChanged(Player* player)
{
    if (!player || _partyByBot.contains(player->GetGUID()) || IsCompanion(player->GetGUID()))
        return;

    ObjectGuid const guid = player->GetGUID();
    Map* map = player->GetMap();
    bool const dungeon = map && map->IsDungeon() && !map->IsRaid();

    // Leaving the dungeon the group was filled for, its temporary members go. Not as a ghost: releasing in a dungeon
    // puts the owner outside at a graveyard, and it runs back in to its corpse.
    if (auto const filled = _filledInstance.find(guid); filled != _filledInstance.end()
        && (!dungeon || map->GetInstanceId() != filled->second) && player->IsAlive())
    {
        _filledInstance.erase(filled);
        if (auto const party = _parties.find(guid); party != _parties.end() && party->second->HasFillers())
        {
            party->second->RemoveMembers(true);
            std::erase_if(_partyByBot, [](auto const& entry) { return !entry.second->HasBot(entry.first); });
            if (!party->second->Size())
                RemoveParty(guid);
            ChatHandler(player->GetSession()).SendSysMessage("Your temporary party members stay behind in the dungeon.");
        }
    }

    if (_config.Enable && _config.PartyFill && dungeon && !_filledInstance.contains(guid))
        _pendingFill[guid] = 2000;
}

std::string Animus::AnimusMod::FillerName()
{
    // Two or three syllables of a fantasy name, capitalized, that no character has and the core accepts.
    static constexpr std::array<char const*, 24> FIRST = { "Ael", "Bra", "Cor", "Dra", "Eld", "Fen", "Gal", "Hal",
        "Isa", "Jor", "Kel", "Lor", "Mar", "Nor", "Ora", "Pel", "Quin", "Ros", "Syl", "Tor", "Ul", "Val", "Wyn", "Zar" };
    static constexpr std::array<char const*, 20> MIDDLE = { "a", "e", "i", "o", "an", "en", "ar", "or", "is", "el",
        "ith", "on", "ra", "ri", "ul", "ae", "ia", "ed", "in", "ae" };
    static constexpr std::array<char const*, 20> LAST = { "dor", "wyn", "ric", "las", "mir", "nor", "thas", "vin",
        "ren", "dan", "sa", "ra", "lin", "gar", "mon", "thil", "ven", "rah", "dis", "mus" };
    for (uint32 attempt = 0; attempt < 100; ++attempt)
    {
        std::string name = std::string(FIRST[urand(0, FIRST.size() - 1)]) + (urand(0, 1) ? MIDDLE[urand(0,
            MIDDLE.size() - 1)] : "") + LAST[urand(0, LAST.size() - 1)];
        if (name.size() > MAX_PLAYER_NAME)
            continue;
        if (ObjectMgr::CheckPlayerName(name) == CHAR_NAME_SUCCESS && !sObjectMgr->IsReservedName(name)
            && !sCharacterCache->GetCharacterGuidByName(name))
            return name;
    }
    return {};
}

void Animus::AnimusMod::FillParty(Player* owner)
{
    namespace Curriculum = Animus::Curriculum;
    Map* map = owner->GetMap();
    if (!map || !map->IsDungeon() || map->IsRaid())
        return;

    // The group's leader fills it (a player alone is its own), and only a five-player group.
    Group* group = owner->GetGroup();
    if (group && (!group->IsLeader(owner->GetGUID()) || group->isRaidGroup() || group->isBGGroup()))
        return;
    uint32 const members = group ? group->GetMembersCount() : 1;
    if (members >= MAXGROUPSIZE)
        return;

    // What the group has: someone to hold the pull, someone to keep the others up. Every member is read off its
    // talents -- companions and temporary members through their party, real players directly.
    Curriculum::AptitudeDemand const tanks = Curriculum::AptitudeDemand::HoldsThePull();
    Curriculum::AptitudeDemand const heals = Curriculum::AptitudeDemand::KeepsThemUp();
    bool haveTank = false;
    bool haveHealer = false;
    auto const read = [&](Curriculum::Aptitude const& aptitude)
    {
        if (tanks.MetBy(aptitude))
            haveTank = true;
        else if (heals.MetBy(aptitude))
            haveHealer = true;
    };
    std::vector<Player*> present;
    if (group)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                present.push_back(member);
    }
    else
        present.push_back(owner);
    for (Player* member : present)
    {
        if (auto const party = _partyByBot.find(member->GetGUID()); party != _partyByBot.end())
        {
            if (Curriculum::Aptitude const* aptitude = party->second->AptitudeOf(member->GetGUID()))
                read(*aptitude);
        }
        else if (Curriculum::ClassProfile const* profile = Curriculum::ClassAssets::FindProfile(member->getClass()))
        {
            Curriculum::ClassAssets const& assets = Curriculum::ClassAssets::For(*profile);
            read(Curriculum::Aptitude::Of(assets, CompanionParty::ReadBuild(assets, member), member));
        }
    }

    std::vector<Curriculum::AptitudeDemand> roles;
    if (!haveTank)
        roles.push_back(tanks);
    if (!haveHealer)
        roles.push_back(heals);
    while (roles.size() < MAXGROUPSIZE - members)
        roles.push_back(Curriculum::AptitudeDemand::Anything());
    roles.resize(std::min<std::size_t>(roles.size(), MAXGROUPSIZE - members));

    std::unique_ptr<CompanionParty>& party = _parties[owner->GetGUID()];
    if (!party)
        party = std::make_unique<CompanionParty>(owner->GetGUID());

    uint8 const level = uint8(owner->GetLevel());
    std::vector<std::string> joined;
    for (Curriculum::AptitudeDemand const& role : roles)
    {
        bool const damage = !role.Any();
        // A class of the owner's faction that can take the role at this level, and a spec of it that does: for a
        // damage seat, one that neither holds the pull nor heals.
        struct Choice { uint8 Class; std::vector<uint8> Specs; std::vector<uint8> Races; };
        std::vector<Choice> choices;
        for (uint8 playerClass : Curriculum::ClassAssets::ClassesFor(level, role))
        {
            Curriculum::ClassProfile const* profile = Curriculum::ClassAssets::FindProfile(playerClass);
            if (!profile)
                continue;
            Curriculum::ClassAssets const& assets = Curriculum::ClassAssets::For(*profile);
            Choice choice{ playerClass, {}, {} };
            for (uint8 race : assets.Races)
                if (Player::TeamIdForRace(race) == owner->GetTeamId())
                    choice.Races.push_back(race);
            if (choice.Races.empty())
                continue;
            if (damage)
            {
                for (uint8 spec = 0; spec < assets.SpecAptitudes.size(); ++spec)
                    if (!tanks.MetBy(assets.SpecAptitudes[spec]) && !heals.MetBy(assets.SpecAptitudes[spec]))
                        choice.Specs.push_back(spec);
            }
            else
                choice.Specs = assets.SpecsMeeting(role);
            if (!choice.Specs.empty())
                choices.push_back(std::move(choice));
        }
        if (choices.empty())
            continue;

        // A class with no model this server can play fills no seat (no model, no companion).
        std::erase_if(choices, [this](Choice const& choice)
        {
            std::string error;
            return !_models.Find(LayoutFor(*Curriculum::ClassAssets::FindProfile(choice.Class)), error);
        });
        if (choices.empty())
            continue;

        Choice const& choice = choices[urand(0, choices.size() - 1)];
        Curriculum::ClassProfile const& profile = *Curriculum::ClassAssets::FindProfile(choice.Class);
        Curriculum::Layout const& layout = LayoutFor(profile);
        uint8 const spec = choice.Specs[urand(0, choice.Specs.size() - 1)];
        std::string const name = FillerName();
        if (name.empty())
            break;
        uint32 const account = BotAccounts::BASE + 800000 + (_nextFillerAccount++ % 100000);
        std::string message;
        if (!party->AddFiller(owner, layout, choice.Races[urand(0, choice.Races.size() - 1)], name, account, spec,
            message))
        {
            LOG_ERROR("module.animus", "Filling {}'s party: {}", owner->GetName(), message);
            break;
        }
        _partyByBot[party->GetBotGUIDs().back()] = party.get();
        joined.push_back(Acore::StringFormat("{} ({} {}, {})", name, profile.Specs[spec].Name, profile.Name,
            role.Any() ? (role.Feature == tanks.Feature ? "tank" : "healer") : "damage"));
    }

    if (!party->Size())
    {
        _parties.erase(owner->GetGUID());
        return;
    }
    _filledInstance[owner->GetGUID()] = map->GetInstanceId();
    if (!joined.empty())
    {
        std::string list;
        for (std::string const& entry : joined)
            list += (list.empty() ? "" : ", ") + entry;
        ChatHandler(owner->GetSession()).SendSysMessage(Acore::StringFormat("Your party is filled for the dungeon: {}.",
            list));
    }
}

void Animus::AnimusMod::RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type)
{
    if (_partyByBot.empty() || !attacker || !victim || !damage
        || (type != DIRECT_DAMAGE && type != SPELL_DIRECT_DAMAGE && type != DOT))
        return;

    // A companion's damage dealt and taken, counted as animus-lib's EnvPool::RecordDamage counts a forge seat's:
    // taken on the companion itself, dealt by it or by its pets, guardians and totems.
    if (auto const taker = _partyByBot.find(victim->GetGUID()); taker != _partyByBot.end())
        taker->second->RecordDamageTaken(victim->GetGUID(), damage);

    ObjectGuid const dealer = attacker->GetCharmerOrOwnerOrOwnGUID();
    if (auto const party = _partyByBot.find(dealer); party != _partyByBot.end())
        party->second->RecordDamageDealt(dealer, victim->GetGUID(), damage);
}

void Animus::AnimusMod::RemoveParty(ObjectGuid owner)
{
    auto const itr = _parties.find(owner);
    if (itr == _parties.end())
        return;

    // Unlink before destroying: logging the bots out re-enters the module through OnPlayerLogout.
    std::unique_ptr<CompanionParty> const party = std::move(itr->second);
    _parties.erase(itr);
    std::erase_if(_partyByBot, [&](auto const& entry) { return entry.second == party.get(); });

    party->DestroyAll();
}

void Animus::AnimusMod::RemoveAll()
{
    while (!_parties.empty())
        SaveParty(_parties.begin()->first);
}

Animus::Curriculum::Layout const* Animus::AnimusMod::DirectorLayout()
{
    // The stage's director, when its arenas are directed (a party, a dungeon, a raid): one layout for every party.
    Curriculum::StageDefinition const* stage = Curriculum::FindStage(_config.CurriculumStage);
    if (!stage || !stage->AnyArena([](Curriculum::ArenaDefinition const& arena) { return arena.DirectorLearned; }))
        return nullptr;
    if (!_directorLayout || _directorLayout->Stage != stage)
    {
        _directorLayout = Curriculum::Layout::BuildDirector(*stage);
        LOG_INFO("module.animus", "Animus built the {} layout (obs {}, actions {})", _directorLayout->ModelName(),
            _directorLayout->ObsDim, _directorLayout->NumActions);
    }
    return &*_directorLayout;
}

Animus::Curriculum::Layout const& Animus::AnimusMod::LayoutFor(Curriculum::ClassProfile const& profile)
{
    // AnimusConfig::Load only keeps a stage that exists; the first stage stands in should that ever not hold, rather
    // than a null dereference on the first create (which is what a renamed stage once caused).
    Curriculum::StageDefinition const* found = Curriculum::FindStage(_config.CurriculumStage);
    if (!found)
    {
        LOG_ERROR("module.animus", "Stage \"{}\" is not a curriculum stage; companions use \"{}\"",
            _config.CurriculumStage, Curriculum::CurriculumStages().front().Name);
        found = &Curriculum::CurriculumStages().front();
    }
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

void Animus::AnimusMod::OnOwnerQuestAccept(Player* owner, Quest const* quest)
{
    if (auto const party = _parties.find(owner->GetGUID()); party != _parties.end())
        sLife->OnOwnerQuestAccept(owner, quest, party->second->PresentBots());
}

void Animus::AnimusMod::OnOwnerQuestAbandon(Player* owner, uint32 questId)
{
    if (auto const party = _parties.find(owner->GetGUID()); party != _parties.end())
        sLife->OnOwnerQuestAbandon(owner, questId, party->second->PresentBots());
}

std::vector<std::string> Animus::AnimusMod::LifeStatus() const
{
    return sLife->Status();
}

bool Animus::AnimusMod::LifeToggle(std::string_view feature, std::string_view state, std::string& message)
{
    bool const on = state == "on";
    if (!on && state != "off")
    {
        message = "The state is on or off.";
        return false;
    }
    if (!sLife->Toggle(feature, on))
    {
        message = "The feature is one of all, quests, auction, mail, taxi, corpse, crafting.";
        return false;
    }
    message = Acore::StringFormat("Life: {} {} (until the next reload of the config).", feature, state);
    return true;
}

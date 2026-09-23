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
#include "BotFactory.h"
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

    /// What `.animus summon` accepts, first name of each id shown in messages. Matched case-insensitively, ignoring
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

    constexpr std::array<NamedId, 6> ROLE_NAMES =
    { {
        { "dps", uint8(Animus::Curriculum::Role::Dps) }, { "tank", uint8(Animus::Curriculum::Role::Tank) },
        { "heal", uint8(Animus::Curriculum::Role::Heal) }, { "damage", uint8(Animus::Curriculum::Role::Dps) },
        { "healer", uint8(Animus::Curriculum::Role::Heal) }, { "dd", uint8(Animus::Curriculum::Role::Dps) },
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

    /// Every id's first name, for messages.
    template <std::size_t N>
    std::string Names(std::array<NamedId, N> const& names)
    {
        std::string list;
        std::vector<uint8> seen;
        for (NamedId const& entry : names)
        {
            if (std::find(seen.begin(), seen.end(), entry.Id) != seen.end())
                continue;
            seen.push_back(entry.Id);
            list += (list.empty() ? "" : ", ") + std::string(entry.Name);
        }
        return list;
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

    // Models load when a companion or a stage seat first needs them, from the (new) model directory.
    _models.Reset(_config.Enable ? _config.ModelDir : "");

    if (!_config.Enable)
        LOG_INFO("module.animus", "Animus is disabled (Animus.Enable = 0)");
}

void Animus::AnimusMod::OnUpdate(uint32 diff)
{
    if (_parties.empty())
        return;

    if (!_config.Enable)
    {
        RemoveAll();
        return;
    }

    CompanionParty::Settings const settings{ _config.CurriculumDecisionMs, _config.CurriculumActions,
        _config.CurriculumOptions };
    std::vector<ObjectGuid> gone;
    for (auto const& [owner, party] : _parties)
        if (party->Update(diff, settings, _models) == CompanionParty::Status::Dismiss)
            gone.push_back(owner);

    for (ObjectGuid const& owner : gone)
        RemoveParty(owner);

    // A companion a party lost on its own (not dismissed) no longer counts damage.
    std::erase_if(_partyByBot, [](auto const& entry) { return !entry.second->HasBot(entry.first); });
}

void Animus::AnimusMod::OnShutdown()
{
    RemoveAll();
}

bool Animus::AnimusMod::Dismiss(Player* owner, std::string& message)
{
    auto const party = _parties.find(owner->GetGUID());
    if (party == _parties.end() || !party->second->Size())
    {
        message = "You have no companions.";
        return false;
    }

    std::size_t const count = party->second->Size();
    RemoveParty(owner->GetGUID());
    message = count == 1 ? "Companion dismissed." : Acore::StringFormat("{} companions dismissed.", count);
    return true;
}

bool Animus::AnimusMod::Summon(Player* owner, std::string_view race, std::string_view playerClass,
    std::string_view role, std::string& message)
{
    if (!_config.Enable)
    {
        message = "Animus is disabled.";
        return false;
    }

    std::optional<uint8> const raceId = FindNamed(RACE_NAMES, race);
    if (!raceId)
    {
        message = Acore::StringFormat("Unknown race {}. Races: {}.", race, Names(RACE_NAMES));
        return false;
    }

    std::optional<uint8> const classId = FindNamed(CLASS_NAMES, playerClass);
    if (!classId)
    {
        message = Acore::StringFormat("Unknown class {}. Classes: {}.", playerClass, Names(CLASS_NAMES));
        return false;
    }

    std::optional<uint8> const roleId = FindNamed(ROLE_NAMES, role);
    if (!roleId)
    {
        message = Acore::StringFormat("Unknown role {}. Roles: {}.", role, Names(ROLE_NAMES));
        return false;
    }

    std::string const raceName = NameOf(RACE_NAMES, *raceId);
    std::string const className = NameOf(CLASS_NAMES, *classId);
    Curriculum::Role const playRole = Curriculum::Role(*roleId);

    Curriculum::ClassProfile const* profile = Curriculum::ClassAssets::FindProfile(*classId);
    if (!profile || !profile->Plays(playRole))
    {
        std::string roles;
        if (profile)
            for (uint32 role = 0; role < Curriculum::ROLE_COUNT; ++role)
                if (profile->Plays(Curriculum::Role(role)))
                    roles += (roles.empty() ? "" : ", ") + std::string(Curriculum::RoleName(Curriculum::Role(role)));

        message = Acore::StringFormat("A {} cannot be a {}. A {} can be: {}.", className,
            Curriculum::RoleName(playRole), className, roles);
        return false;
    }

    if (!sObjectMgr->GetPlayerInfo(*raceId, *classId))
    {
        message = Acore::StringFormat("A {} cannot be a {}.", raceName, className);
        return false;
    }

    if (Player::TeamIdForRace(*raceId) != owner->GetTeamId())
    {
        message = Acore::StringFormat("A {} is not of your faction.", raceName);
        return false;
    }

    // No companion can ride a flight path or a vehicle with you; companions you already have wait for you to land.
    if (BotFactory::IsAway(owner))
    {
        message = "Companions cannot be summoned on a flight path or a vehicle; summon them once you are off it.";
        return false;
    }

    // Not a choice of the summon: no bot can be placed while you are between maps, and a battleground takes only
    // queued players.
    if (!BotFactory::CanJoin(owner))
    {
        message = "Companions cannot join you in a battleground or arena, or while you are changing maps.";
        return false;
    }

    Curriculum::Layout const& layout = LayoutFor(*profile);

    std::unique_ptr<CompanionParty>& party = _parties[owner->GetGUID()];
    if (!party)
        party = std::make_unique<CompanionParty>(owner->GetGUID());

    if (!party->Add(owner, layout, playRole, *raceId, message))
    {
        if (!party->Size())
            _parties.erase(owner->GetGUID());
        return false;
    }

    for (ObjectGuid const& bot : party->GetBotGUIDs())
        _partyByBot[bot] = party.get();

    std::string error;
    if (!_models.Find(layout, error))
        message += Acore::StringFormat(" Its model is not available ({}), so it only follows you.", error);

    return true;
}

std::vector<std::string> Animus::AnimusMod::List(Player* owner)
{
    auto const itr = _parties.find(owner->GetGUID());
    if (itr == _parties.end())
        return {};

    return itr->second->Describe(_models);
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
    RemoveParty(player->GetGUID());
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
        RemoveParty(_parties.begin()->first);
}

Animus::Curriculum::Layout const& Animus::AnimusMod::LayoutFor(Curriculum::ClassProfile const& profile)
{
    Curriculum::StageDefinition const& stage = *Curriculum::FindStage(_config.CurriculumStage);
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

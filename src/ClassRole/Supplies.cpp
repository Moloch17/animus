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

#include "Supplies.h"
#include "ClassRoleLayout.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <map>
#include <unordered_set>

namespace
{
    enum SupplySpells : uint32
    {
        SPELL_TAME_BEAST            = 1515,
    };

    /// Hunters get Tame Beast and Call Pet at level 10.
    constexpr uint8 HUNTER_PET_LEVEL = 10;
}

Animus::ClassRole::ConsumablePool const& Animus::ClassRole::ConsumablePool::Instance()
{
    static ConsumablePool const pool;
    return pool;
}

Animus::ClassRole::ConsumablePool::ConsumablePool()
{
    std::unordered_set<uint32> sold;
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT CAST(item AS SIGNED) FROM npc_vendor"))
    {
        do
        {
            if (int64 const item = result->Fetch()[0].Get<int64>(); item > 0)
                sold.insert(uint32(item));
        } while (result->NextRow());
    }

    for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (proto.Class != ITEM_CLASS_CONSUMABLE || proto.SubClass != ITEM_SUBCLASS_FOOD || !sold.contains(itemId)
            || proto.RequiredSkill || proto.RequiredReputationFaction)
            continue;

        _Spell const& use = proto.Spells[0];
        SpellInfo const* info = use.SpellId > 0 && use.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE
            ? sSpellMgr->GetSpellInfo(use.SpellId) : nullptr;
        if (!info)
            continue;

        uint8 const level = uint8(std::min<uint32>(proto.RequiredLevel, DEFAULT_MAX_LEVEL));
        if (info->HasAura(SPELL_AURA_MOD_REGEN))
            _food.emplace_back(level, itemId);
        else if (info->HasAura(SPELL_AURA_MOD_POWER_REGEN))
            _drink.emplace_back(level, itemId);
    }

    std::sort(_food.begin(), _food.end());
    std::sort(_drink.begin(), _drink.end());

    LOG_INFO("module.animus", "Consumables: {} foods, {} drinks sold by vendors", _food.size(), _drink.size());
}

uint32 Animus::ClassRole::ConsumablePool::Best(std::vector<std::pair<uint8, uint32>> const& items, uint8 level)
{
    uint32 best = 0;
    for (auto const& [reqLevel, itemId] : items)
        if (reqLevel <= level)
            best = itemId;

    return best;
}

Animus::ClassRole::StablePool const& Animus::ClassRole::StablePool::Instance()
{
    static StablePool const pool;
    return pool;
}

Animus::ClassRole::StablePool::StablePool()
{
    std::unordered_set<uint32> spawned;
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT id FROM creature"))
    {
        do
        {
            spawned.insert(result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());
    }

    // Creatures that walk waypoint paths are scripted set pieces (rares on patrol, escorts): not fair opponents or pets.
    std::unordered_set<uint32> walkers;
    if (QueryResult result = WorldDatabase.Query("SELECT DISTINCT c.id FROM creature c "
        "LEFT JOIN creature_addon a ON a.guid = c.guid WHERE c.MovementType = 2 OR IFNULL(a.path_id, 0) <> 0 "
        "UNION SELECT entry FROM creature_template_addon WHERE path_id <> 0"))
    {
        do
        {
            walkers.insert(result->Fetch()[0].Get<uint32>());
        } while (result->NextRow());
    }

    std::map<uint32, std::vector<uint32>> beastsByFamily;
    for (auto const& [entry, info] : *sObjectMgr->GetCreatureTemplates())
        if (spawned.contains(entry) && !walkers.contains(entry) && info.IsTameable(false))
            beastsByFamily[info.family].push_back(entry);

    for (auto& [family, entries] : beastsByFamily)
        _beastsByFamily.push_back(std::move(entries));

    LOG_INFO("module.animus", "Stable: {} tameable beast families", _beastsByFamily.size());
}

std::vector<uint32> Animus::ClassRole::StablePool::Random(uint32 count) const
{
    std::vector<uint32> families(_beastsByFamily.size());
    for (uint32 i = 0; i < families.size(); ++i)
        families[i] = i;

    std::vector<uint32> stable;
    while (stable.size() < count && !families.empty())
    {
        uint32 const pick = urand(0, uint32(families.size()) - 1);
        std::vector<uint32> const& entries = _beastsByFamily[families[pick]];
        stable.push_back(entries[urand(0, uint32(entries.size()) - 1)]);
        families.erase(families.begin() + pick);
    }

    return stable;
}

void Animus::ClassRole::StockConsumables(Player* bot, uint32 food, uint32 drink)
{
    for (uint32 item : { food, drink })
    {
        if (!item)
            continue;

        for (uint32 count = bot->GetItemCount(item); count < LayoutConstants::CONSUMABLE_COUNT; ++count)
            if (!bot->StoreNewItemInBestSlots(item, 1))
                break;
    }
}

bool Animus::ClassRole::CallHunterBeast(Player* bot, uint32 entry)
{
    if (bot->getClass() != CLASS_HUNTER || bot->GetPetGUID() || bot->GetLevel() < HUNTER_PET_LEVEL || !entry)
        return false;

    Pet* pet = bot->CreateTamedPetFrom(entry, SPELL_TAME_BEAST);
    if (!pet)
        return false;

    // Spell::EffectTameCreature without the database save: the bot and its pet are never saved.
    pet->SetUInt32Value(UNIT_FIELD_LEVEL, bot->GetLevel());
    pet->GetMap()->AddToMap(pet->ToCreature(), true);
    bot->SetMinion(pet, true);
    pet->InitTalentForLevel();
    bot->PetSpellInitialize();
    return true;
}

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

#include "GearBuilder.h"
#include "ClassKit.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace
{
    using Animus::Curriculum::StatProfile;

    constexpr std::array<uint32, 4> LEVEL_WINDOWS = { 4, 9, 19, 99 };
    constexpr uint32 EQUIP_ATTEMPTS = 6;
    constexpr uint32 AMMO_COUNT = 1000;

    /// Stat preference of a profile: 1 wanted, -1 never on this spec's gear, 0 indifferent.
    int32 StatPreference(StatProfile profile, uint32 stat)
    {
        bool const physical = profile == StatProfile::StrengthMelee || profile == StatProfile::AgilityMelee;
        bool const spell = profile == StatProfile::Caster || profile == StatProfile::Healer;

        switch (stat)
        {
            case ITEM_MOD_STRENGTH:
                return physical || profile == StatProfile::Tank ? 1 : (profile == StatProfile::Ranged ? 0 : -1);
            case ITEM_MOD_AGILITY:
                return spell ? -1 : 1;
            case ITEM_MOD_STAMINA:
                return profile == StatProfile::Tank ? 1 : 0;
            case ITEM_MOD_INTELLECT:
                return spell ? 1 : (profile == StatProfile::Ranged ? 0 : -1);
            case ITEM_MOD_SPIRIT:
                return profile == StatProfile::Healer ? 1 : (profile == StatProfile::Caster ? 0 : -1);
            case ITEM_MOD_ATTACK_POWER:
            case ITEM_MOD_ARMOR_PENETRATION_RATING:
                return spell ? -1 : (profile == StatProfile::Tank ? 0 : 1);
            case ITEM_MOD_RANGED_ATTACK_POWER:
            case ITEM_MOD_HIT_RANGED_RATING:
            case ITEM_MOD_CRIT_RANGED_RATING:
            case ITEM_MOD_HASTE_RANGED_RATING:
                return profile == StatProfile::Ranged ? 1 : (spell ? -1 : 0);
            case ITEM_MOD_HIT_MELEE_RATING:
            case ITEM_MOD_CRIT_MELEE_RATING:
            case ITEM_MOD_HASTE_MELEE_RATING:
                return spell ? -1 : (profile == StatProfile::Tank && stat != ITEM_MOD_HIT_MELEE_RATING ? 0 : 1);
            case ITEM_MOD_EXPERTISE_RATING:
                return spell || profile == StatProfile::Ranged ? -1 : 1;
            case ITEM_MOD_HIT_RATING:
                return profile == StatProfile::Healer ? -1 : 1;
            case ITEM_MOD_CRIT_RATING:
            case ITEM_MOD_HASTE_RATING:
                return profile == StatProfile::Tank ? 0 : 1;
            case ITEM_MOD_HIT_SPELL_RATING:
                return profile == StatProfile::Caster ? 1 : -1;
            case ITEM_MOD_CRIT_SPELL_RATING:
            case ITEM_MOD_HASTE_SPELL_RATING:
            case ITEM_MOD_SPELL_POWER:
            case ITEM_MOD_SPELL_DAMAGE_DONE:
            case ITEM_MOD_SPELL_HEALING_DONE:
            case ITEM_MOD_SPELL_PENETRATION:
                return spell ? 1 : -1;
            case ITEM_MOD_MANA_REGENERATION:
                return profile == StatProfile::Healer ? 1 : (profile == StatProfile::Caster ? 0 : -1);
            case ITEM_MOD_DEFENSE_SKILL_RATING:
            case ITEM_MOD_DODGE_RATING:
            case ITEM_MOD_PARRY_RATING:
            case ITEM_MOD_BLOCK_RATING:
            case ITEM_MOD_BLOCK_VALUE:
                return profile == StatProfile::Tank ? 1 : -1;
            default:
                return 0;
        }
    }

    struct StatVerdict
    {
        bool Wanted = false;
        bool Forbidden = false;

        void Add(StatProfile profile, uint32 stat)
        {
            int32 const preference = StatPreference(profile, stat);
            Wanted |= preference > 0;
            Forbidden |= preference < 0;
        }

        [[nodiscard]] bool Suits() const { return Wanted && !Forbidden; }
    };

    StatVerdict EnchantmentVerdict(StatProfile profile,
        std::array<uint32, MAX_ITEM_ENCHANTMENT_EFFECTS> const& enchantments)
    {
        StatVerdict verdict;
        for (uint32 enchantmentId : enchantments)
        {
            SpellItemEnchantmentEntry const* enchantment = sSpellItemEnchantmentStore.LookupEntry(enchantmentId);
            if (!enchantment)
                continue;

            for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
                if (enchantment->type[i] == ITEM_ENCHANTMENT_TYPE_STAT)
                    verdict.Add(profile, enchantment->spellid[i]);
        }

        return verdict;
    }

    /// Items a player can actually get: loot, vendors, quest rewards and crafting. Loaded once.
    std::unordered_set<uint32> const& ObtainableItems()
    {
        static std::unordered_set<uint32> const items = []()
        {
            std::unordered_set<uint32> result;

            // Column types differ between tables (npc_vendor.item is signed: negative = vendor reference),
            // so every id is read as a signed 64-bit value.
            for (char const* sql :
            {
                "SELECT CAST(Item AS SIGNED) FROM creature_loot_template WHERE Reference = 0",
                "SELECT CAST(Item AS SIGNED) FROM reference_loot_template WHERE Reference = 0",
                "SELECT CAST(Item AS SIGNED) FROM gameobject_loot_template WHERE Reference = 0",
                "SELECT CAST(Item AS SIGNED) FROM item_loot_template WHERE Reference = 0",
                "SELECT CAST(item AS SIGNED) FROM npc_vendor",
                "SELECT CAST(RewardItem1 AS SIGNED) FROM quest_template UNION SELECT CAST(RewardItem2 AS SIGNED) FROM "
                    "quest_template UNION SELECT CAST(RewardItem3 AS SIGNED) FROM quest_template UNION SELECT "
                    "CAST(RewardItem4 AS SIGNED) FROM quest_template",
                "SELECT CAST(RewardChoiceItemID1 AS SIGNED) FROM quest_template "
                    "UNION SELECT CAST(RewardChoiceItemID2 AS SIGNED) FROM quest_template "
                    "UNION SELECT CAST(RewardChoiceItemID3 AS SIGNED) FROM quest_template "
                    "UNION SELECT CAST(RewardChoiceItemID4 AS SIGNED) FROM quest_template "
                    "UNION SELECT CAST(RewardChoiceItemID5 AS SIGNED) FROM quest_template "
                    "UNION SELECT CAST(RewardChoiceItemID6 AS SIGNED) FROM quest_template",
            })
            {
                if (QueryResult query = WorldDatabase.Query(sql))
                {
                    do
                    {
                        int64 const itemId = query->Fetch()[0].Get<int64>();
                        if (itemId > 0)
                            result.insert(uint32(itemId));
                    } while (query->NextRow());
                }
            }

            for (uint32 spellId = 0; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                    for (SpellEffectInfo const& effect : info->GetEffects())
                        if ((effect.Effect == SPELL_EFFECT_CREATE_ITEM || effect.Effect == SPELL_EFFECT_CREATE_ITEM_2)
                            && effect.ItemType)
                            result.insert(effect.ItemType);

            LOG_INFO("module.animus", "Gear: {} obtainable items", result.size());
            return result;
        }();

        return items;
    }

    /// item_enchantment_template: random property / suffix group -> ids. Loaded once.
    std::unordered_map<uint32, std::vector<uint32>> const& RandomEnchantGroups()
    {
        static std::unordered_map<uint32, std::vector<uint32>> const groups = []()
        {
            std::unordered_map<uint32, std::vector<uint32>> result;
            if (QueryResult query = WorldDatabase.Query("SELECT entry, ench FROM item_enchantment_template"))
            {
                do
                {
                    Field* fields = query->Fetch();
                    result[fields[0].Get<uint32>()].push_back(fields[1].Get<uint32>());
                } while (query->NextRow());
            }

            return result;
        }();

        return groups;
    }

    std::unordered_set<uint32> UsableSkills(uint8 playerClass)
    {
        std::unordered_set<uint32> skills;
        for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(i);
            if (!line || (line->categoryId != SKILL_CATEGORY_WEAPON && line->categoryId != SKILL_CATEGORY_ARMOR))
                continue;

            for (uint8 race = RACE_HUMAN; race <= RACE_DRAENEI; ++race)
            {
                if (sObjectMgr->GetPlayerInfo(race, playerClass) && GetSkillRaceClassInfo(line->id, race, playerClass))
                {
                    skills.insert(line->id);
                    break;
                }
            }
        }

        return skills;
    }

    uint8 RequiredLevelOf(ItemTemplate const* proto)
    {
        if (proto->RequiredLevel)
            return uint8(std::min<uint32>(proto->RequiredLevel, DEFAULT_MAX_LEVEL));

        return uint8(std::clamp<uint32>(proto->ItemLevel > 5 ? proto->ItemLevel - 5 : 1, 1, DEFAULT_MAX_LEVEL));
    }
}

Animus::Curriculum::GearBuilder::GearBuilder(ClassRoleProfile const& profile, ClassKit const& kit)
    : _kit(kit), _class(profile.Class)
{
    for (SpecProfile const& spec : profile.Specs)
        if (!_pools.contains(spec.Stats))
            BuildPools(spec.Stats);

    if (_class == CLASS_HUNTER)
    {
        std::unordered_set<uint32> const& obtainable = ObtainableItems();
        for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            if (proto.Class != ITEM_CLASS_PROJECTILE || !obtainable.contains(itemId)
                || proto.Quality > ITEM_QUALITY_EPIC)
                continue;

            if (proto.SubClass == ITEM_SUBCLASS_ARROW)
                _arrows.emplace_back(RequiredLevelOf(&proto), itemId);
            else if (proto.SubClass == ITEM_SUBCLASS_BULLET)
                _bullets.emplace_back(RequiredLevelOf(&proto), itemId);
        }

        std::sort(_arrows.begin(), _arrows.end());
        std::sort(_bullets.begin(), _bullets.end());
    }
}

void Animus::Curriculum::GearBuilder::BuildPools(StatProfile stats)
{
    Pools& pools = _pools[stats];

    std::unordered_set<uint32> const& obtainable = ObtainableItems();
    std::unordered_map<uint32, std::vector<uint32>> const& enchantGroups = RandomEnchantGroups();
    std::unordered_set<uint32> const skills = UsableSkills(_class);
    uint32 const classMask = 1 << (_class - 1);

    for (auto const& [itemId, proto] : *sObjectMgr->GetItemTemplateStore())
    {
        if (!obtainable.contains(itemId) || proto.Quality < ITEM_QUALITY_NORMAL || proto.Quality > ITEM_QUALITY_EPIC)
            continue;

        if (!(proto.AllowableClass & classMask) || proto.RequiredSkill || proto.RequiredSpell || proto.RequiredHonorRank
            || proto.RequiredCityRank || proto.RequiredReputationFaction || proto.Duration)
            continue;

        if (uint32 const skill = proto.GetSkill(); skill && !skills.contains(skill))
            continue;

        std::vector<Pool> targets;
        bool const armor = proto.Class == ITEM_CLASS_ARMOR;
        bool const weapon = proto.Class == ITEM_CLASS_WEAPON;
        bool const bodyArmor = armor && proto.SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH
            && proto.SubClass <= ITEM_SUBCLASS_ARMOR_PLATE;

        switch (proto.InventoryType)
        {
            case INVTYPE_HEAD:      if (bodyArmor) targets = { POOL_HEAD }; break;
            case INVTYPE_SHOULDERS: if (bodyArmor) targets = { POOL_SHOULDERS }; break;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:      if (bodyArmor) targets = { POOL_CHEST }; break;
            case INVTYPE_WAIST:     if (bodyArmor) targets = { POOL_WAIST }; break;
            case INVTYPE_LEGS:      if (bodyArmor) targets = { POOL_LEGS }; break;
            case INVTYPE_FEET:      if (bodyArmor) targets = { POOL_FEET }; break;
            case INVTYPE_WRISTS:    if (bodyArmor) targets = { POOL_WRISTS }; break;
            case INVTYPE_HANDS:     if (bodyArmor) targets = { POOL_HANDS }; break;
            case INVTYPE_NECK:      if (armor) targets = { POOL_NECK }; break;
            case INVTYPE_FINGER:    if (armor) targets = { POOL_FINGER }; break;
            case INVTYPE_TRINKET:   if (armor) targets = { POOL_TRINKET }; break;
            case INVTYPE_CLOAK:     if (armor) targets = { POOL_BACK }; break;
            case INVTYPE_SHIELD:
                if (armor && proto.SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)
                    targets = { POOL_SHIELD };
                break;
            case INVTYPE_HOLDABLE:  targets = { POOL_HELD }; break;
            case INVTYPE_2HWEAPON:
                if (weapon && proto.SubClass != ITEM_SUBCLASS_WEAPON_FISHING_POLE)
                    targets = { POOL_TWO_HAND };
                break;
            case INVTYPE_WEAPON:        if (weapon) targets = { POOL_MAIN_HAND, POOL_OFF_HAND }; break;
            case INVTYPE_WEAPONMAINHAND: if (weapon) targets = { POOL_MAIN_HAND }; break;
            case INVTYPE_WEAPONOFFHAND: if (weapon) targets = { POOL_OFF_HAND }; break;
            case INVTYPE_RANGED:
            case INVTYPE_RANGEDRIGHT:
                if (weapon && proto.SubClass == ITEM_SUBCLASS_WEAPON_WAND)
                    targets = { POOL_WAND };
                else if (weapon && proto.SubClass != ITEM_SUBCLASS_WEAPON_THROWN)
                    targets = { POOL_RANGED };
                break;
            default:
                break;
        }

        if (targets.empty())
            continue;

        Candidate candidate;
        candidate.ItemId = itemId;
        candidate.ReqLevel = RequiredLevelOf(&proto);
        candidate.SubClass = proto.SubClass;

        StatVerdict fixed;
        for (uint32 i = 0; i < proto.StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
            if (proto.ItemStat[i].ItemStatValue > 0)
                fixed.Add(stats, proto.ItemStat[i].ItemStatType);

        if (fixed.Forbidden)
            continue;

        // Random stats: keep the properties/suffixes that suit the profile. An item whose random stats
        // never do is still usable for its armor or weapon damage, without them.
        int32 const randomGroup = proto.RandomProperty ? proto.RandomProperty : proto.RandomSuffix;
        if (randomGroup > 0)
        {
            if (auto const group = enchantGroups.find(uint32(randomGroup)); group != enchantGroups.end())
            {
                for (uint32 id : group->second)
                {
                    StatVerdict verdict;
                    if (proto.RandomProperty)
                    {
                        if (ItemRandomPropertiesEntry const* property = sItemRandomPropertiesStore.LookupEntry(id))
                            verdict = EnchantmentVerdict(stats, property->Enchantment);
                    }
                    else if (ItemRandomSuffixEntry const* suffix = sItemRandomSuffixStore.LookupEntry(id))
                        verdict = EnchantmentVerdict(stats, suffix->Enchantment);

                    if (verdict.Suits())
                        candidate.RandomIds.push_back(proto.RandomProperty ? int32(id) : -int32(id));
                }
            }
        }

        bool const onUse = std::any_of(std::begin(proto.Spells), std::end(proto.Spells),
            [](_Spell const& spell) { return spell.SpellId > 0; });

        candidate.Stats = fixed.Wanted || !candidate.RandomIds.empty()
            || (proto.InventoryType == INVTYPE_TRINKET && onUse);

        // Jewelry and trinkets are only worth their stats or effects.
        bool const statsOnly = proto.InventoryType == INVTYPE_NECK || proto.InventoryType == INVTYPE_FINGER
            || proto.InventoryType == INVTYPE_TRINKET;
        if (statsOnly && !candidate.Stats)
            continue;

        for (Pool pool : targets)
            pools[pool].push_back(candidate);
    }

    LOG_INFO("module.animus", "Gear pools for class {} profile {}: {} two-handers, {} one-handers, {} chests, "
        "{} trinkets", _class, uint32(stats), pools[POOL_TWO_HAND].size(), pools[POOL_MAIN_HAND].size(),
        pools[POOL_CHEST].size(), pools[POOL_TRINKET].size());
}

std::vector<Animus::Curriculum::GearBuilder::Candidate const*> Animus::Curriculum::GearBuilder::Window(Pool pool,
    uint8 level, StatProfile stats, int32 subclass, bool needStats) const
{
    std::vector<Candidate const*> found;
    auto const pools = _pools.find(stats);
    if (pools == _pools.end())
        return found;

    for (uint32 window : LEVEL_WINDOWS)
    {
        for (Candidate const& candidate : pools->second[pool])
        {
            if (candidate.ReqLevel > level || candidate.ReqLevel + window < level)
                continue;

            if ((subclass >= 0 && candidate.SubClass != uint32(subclass)) || (needStats && !candidate.Stats))
                continue;

            found.push_back(&candidate);
        }

        if (!found.empty())
            break;
    }

    return found;
}

bool Animus::Curriculum::GearBuilder::EquipFromPool(Player* bot, uint8 slot, Pool pool, StatProfile stats,
    int32 subclass) const
{
    uint8 const level = bot->GetLevel();

    // Body armor falls back to lighter armor types; everything falls back to items without the
    // profile's stats before leaving the slot empty.
    std::vector<int32> subclasses = { subclass };
    if (subclass > ITEM_SUBCLASS_ARMOR_CLOTH && pool <= POOL_HANDS && pool != POOL_NECK)
        for (int32 lighter = subclass - 1; lighter >= int32(ITEM_SUBCLASS_ARMOR_CLOTH); --lighter)
            subclasses.push_back(lighter);

    for (bool needStats : { true, false })
    {
        for (int32 armorSubclass : subclasses)
        {
            std::vector<Candidate const*> candidates = Window(pool, level, stats, armorSubclass, needStats);
            for (uint32 attempt = 0; attempt < EQUIP_ATTEMPTS && !candidates.empty(); ++attempt)
            {
                uint32 const pick = urand(0, uint32(candidates.size()) - 1);
                Candidate const* candidate = candidates[pick];
                candidates.erase(candidates.begin() + pick);

                int32 const randomId = candidate->RandomIds.empty() ? 0
                    : candidate->RandomIds[urand(0, uint32(candidate->RandomIds.size()) - 1)];

                Item* item = Item::CreateItem(candidate->ItemId, 1, bot, false, randomId);
                if (!item)
                    continue;

                uint16 dest = 0;
                if (bot->CanEquipItem(slot, dest, item, false) != EQUIP_ERR_OK)
                {
                    delete item;
                    continue;
                }

                bot->EquipItem(dest, item, true);
                return true;
            }
        }
    }

    return false;
}

void Animus::Curriculum::GearBuilder::LearnProficiencies(Player* bot)
{
    for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
    {
        SkillLineEntry const* line = sSkillLineStore.LookupEntry(i);
        if (!line || (line->categoryId != SKILL_CATEGORY_WEAPON && line->categoryId != SKILL_CATEGORY_ARMOR))
            continue;

        if (!bot->HasSkill(line->id) && GetSkillRaceClassInfo(line->id, bot->getRace(), bot->getClass()))
            bot->LearnDefaultSkill(line->id, 0);
    }

    bot->UpdateSkillsToMaxSkillsForLevel();
}

bool Animus::Curriculum::GearBuilder::EquipWeapons(Player* bot, SpecProfile const& spec, WeaponLayout layout) const
{
    StatProfile const stats = spec.Stats;

    switch (layout)
    {
        case WeaponLayout::TwoHand:
            return EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_TWO_HAND, stats);
        case WeaponLayout::Staff:
            return EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_TWO_HAND, stats, ITEM_SUBCLASS_WEAPON_STAFF);
        case WeaponLayout::DualWield:
            if (!bot->CanDualWield() || !EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_MAIN_HAND, stats))
                return false;
            EquipFromPool(bot, EQUIPMENT_SLOT_OFFHAND, POOL_OFF_HAND, stats);
            return true;
        case WeaponLayout::OneHandShield:
            if (!EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_MAIN_HAND, stats))
                return false;
            EquipFromPool(bot, EQUIPMENT_SLOT_OFFHAND, POOL_SHIELD, stats);
            return true;
        case WeaponLayout::OneHandHeld:
            if (!EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_MAIN_HAND, stats))
                return false;
            EquipFromPool(bot, EQUIPMENT_SLOT_OFFHAND, POOL_HELD, stats);
            return true;
        case WeaponLayout::TwoHandRanged:
        {
            bool const ranged = EquipFromPool(bot, EQUIPMENT_SLOT_RANGED, POOL_RANGED, stats);
            bool const melee = EquipFromPool(bot, EQUIPMENT_SLOT_MAINHAND, POOL_TWO_HAND, stats);
            return ranged || melee;
        }
    }

    return false;
}

void Animus::Curriculum::GearBuilder::Equip(Player* bot, SpecProfile const& spec) const
{
    // Starting outfit, the previous episode's set, bags and backpack contents.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);

    StatProfile const stats = spec.Stats;
    int32 const armor = int32(_kit.ArmorSubclass(bot->GetLevel()));

    EquipFromPool(bot, EQUIPMENT_SLOT_HEAD, POOL_HEAD, stats, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_NECK, POOL_NECK, stats);
    EquipFromPool(bot, EQUIPMENT_SLOT_SHOULDERS, POOL_SHOULDERS, stats, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_CHEST, POOL_CHEST, stats, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_WAIST, POOL_WAIST, stats, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_LEGS, POOL_LEGS, stats, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_FEET, POOL_FEET, stats, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_WRISTS, POOL_WRISTS, stats, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_HANDS, POOL_HANDS, stats, armor);
    EquipFromPool(bot, EQUIPMENT_SLOT_FINGER1, POOL_FINGER, stats);
    EquipFromPool(bot, EQUIPMENT_SLOT_FINGER2, POOL_FINGER, stats);
    EquipFromPool(bot, EQUIPMENT_SLOT_TRINKET1, POOL_TRINKET, stats);
    EquipFromPool(bot, EQUIPMENT_SLOT_TRINKET2, POOL_TRINKET, stats);
    EquipFromPool(bot, EQUIPMENT_SLOT_BACK, POOL_BACK, stats);

    for (WeaponLayout layout : spec.Weapons)
    {
        if (EquipWeapons(bot, spec, layout))
            break;

        for (uint8 slot : { EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND, EQUIPMENT_SLOT_RANGED })
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    }

    if (spec.Wand && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
        EquipFromPool(bot, EQUIPMENT_SLOT_RANGED, POOL_WAND, stats);

    StoreAmmo(bot);
    _kit.StoreReagents(bot);
}

void Animus::Curriculum::GearBuilder::StoreAmmo(Player* bot) const
{
    Item const* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!ranged)
        return;

    std::vector<std::pair<uint8, uint32>> const* ammo = nullptr;
    switch (ranged->GetTemplate()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            ammo = &_arrows;
            break;
        case ITEM_SUBCLASS_WEAPON_GUN:
            ammo = &_bullets;
            break;
        default:
            return;
    }

    // The best ammo the level allows.
    uint32 itemId = 0;
    for (auto const& [reqLevel, id] : *ammo)
        if (reqLevel <= bot->GetLevel())
            itemId = id;

    if (itemId && bot->StoreNewItemInBestSlots(itemId, AMMO_COUNT))
        bot->SetAmmo(itemId);
}

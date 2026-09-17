/*
 * This file is part of the Animus Forge project, based on AzerothCore.
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

#ifndef ANIMUS_LIB_CURRICULUM_GEAR_BUILDER_H
#define ANIMUS_LIB_CURRICULUM_GEAR_BUILDER_H

#include "ClassRoleProfile.h"
#include <array>
#include <map>
#include <vector>

class Item;
class Player;
class SpellInfo;

namespace Animus::Curriculum
{
    class ClassKit;

    /// Random level-appropriate gear for one class and its role's specs.
    ///
    /// Pools hold every obtainable item (dropped, sold, a quest reward or crafted) the class can use,
    /// whose stats suit the spec's StatProfile. Items with random stats keep only the random
    /// properties/suffixes whose stats suit the profile, and one of those is rolled when the item is
    /// created. For a bot of level L each slot takes a random item it may wear (required level <= L) whose
    /// item level is in the band players of level L wear (levelling gear, then dungeon gear at 80), reaching
    /// further below the band when the slot has nothing in it, never above. Epics are worn only at the level caps
    /// (70, 80), and PvP gear (resilience) only in the PvP stages.
    ///
    /// The set is then finished as players finish theirs (GearEnhancements.cpp): enchants and gems that suit the
    /// spec and that a player of the level could buy (every slot at the level caps, about half of them while
    /// levelling; gems matching socket colors for the socket bonus), a relic for the classes that use one, death
    /// knight runes, rogue poisons, and a hunter's quiver or ammo pouch.
    class GearBuilder
    {
    public:
        GearBuilder(ClassRoleProfile const& profile, ClassKit const& kit);

        /// Grants every weapon and armor skill the bot's race and class can have, at the level's value.
        static void LearnProficiencies(Player* bot);

        /// Destroys everything equipped or in the backpack and equips a new, enchanted set for the bot's level.
        void Equip(Player* bot, SpecProfile const& spec, bool pvp) const;

    private:
        enum Pool : uint8
        {
            POOL_HEAD,
            POOL_NECK,
            POOL_SHOULDERS,
            POOL_CHEST,
            POOL_WAIST,
            POOL_LEGS,
            POOL_FEET,
            POOL_WRISTS,
            POOL_HANDS,
            POOL_FINGER,
            POOL_TRINKET,
            POOL_BACK,
            POOL_TWO_HAND,
            POOL_MAIN_HAND,     // one-handers that can go in the main hand
            POOL_OFF_HAND,      // one-handers that can go in the off hand
            POOL_SHIELD,
            POOL_HELD,
            POOL_RANGED,        // bows, guns, crossbows
            POOL_WAND,
            POOL_RELIC,         // librams, idols, totems, sigils
            POOL_COUNT
        };

        /// Head, shoulders, chest, waist, legs, feet, wrists and hands: the pools of cloth to plate armor.
        [[nodiscard]] static bool IsBodyArmorPool(Pool pool)
        {
            switch (pool)
            {
                case POOL_HEAD:
                case POOL_SHOULDERS:
                case POOL_CHEST:
                case POOL_WAIST:
                case POOL_LEGS:
                case POOL_FEET:
                case POOL_WRISTS:
                case POOL_HANDS:
                    return true;
                default:
                    return false;
            }
        }

        struct Candidate
        {
            uint32 ItemId = 0;
            uint8 ReqLevel = 1;
            uint16 ItemLevel = 0;
            uint32 SubClass = 0;
            bool Pvp = false;               // has resilience
            bool Epic = false;
            uint8 Weight = 1;               // how often it is picked, by where it comes from
            bool Stats = false;             // has stats the profile wants (fixed or rolled)
            std::vector<int32> RandomIds;   // allowed random property (>0) / suffix (<0) ids
        };

        using Pools = std::array<std::vector<Candidate>, POOL_COUNT>;

        /// A permanent enchant a player can buy: an enchanting recipe or an enchanting item (armor kits, leg armor,
        /// arcanums, inscriptions), suited to a profile.
        struct EnchantCandidate
        {
            SpellInfo const* Spell = nullptr;
            uint32 EnchantId = 0;
            uint16 Skill = 0;               // enchanting skill it takes (items: the skill of their level)
        };

        struct GemCandidate
        {
            uint32 EnchantId = 0;
            uint32 Color = 0;               // SOCKET_COLOR_* bits
            uint8 ReqLevel = 0;
            uint16 ItemLevel = 0;
            uint8 Quality = 0;
        };

        void BuildPools(StatProfile stats);
        void BuildEnhancements(StatProfile stats);

        /// Enchants, gems, runes and poisons on the equipped set, then the quiver (GearEnhancements.cpp).
        void Enhance(Player* bot, SpecProfile const& spec) const;
        void EnchantItem(Player* bot, Item* item, StatProfile stats) const;
        void SocketItem(Player* bot, Item* item, StatProfile stats, std::vector<std::pair<Item*, uint8>>& metas) const;
        void Runeforge(Player* bot, SpecProfile const& spec) const;
        void ApplyPoisons(Player* bot) const;
        /// Shamans: the spec's standard weapon imbues (Windfury and Flametongue, Flametongue, Earthliving), the highest
        /// rank the character knows, as a player keeps them up.
        void ApplyImbues(Player* bot, SpecProfile const& spec) const;
        void EquipQuiver(Player* bot) const;

        /// Candidates for the level from a pool, reaching below the level's item level band as needed.
        [[nodiscard]] std::vector<Candidate const*> Window(Pool pool, uint8 level, StatProfile stats,
            int32 subclass, bool needStats, bool pvp) const;

        bool EquipFromPool(Player* bot, uint8 slot, Pool pool, StatProfile stats, bool pvp,
            int32 subclass = -1) const;
        bool EquipWeapons(Player* bot, SpecProfile const& spec, WeaponLayout layout, bool pvp) const;
        void StoreAmmo(Player* bot) const;

        ClassKit const& _kit;
        uint8 _class;
        std::map<StatProfile, Pools> _pools;
        std::vector<std::pair<uint8, uint32>> _arrows;     // (required level, item), sorted
        std::vector<std::pair<uint8, uint32>> _bullets;
        std::map<StatProfile, std::vector<EnchantCandidate>> _enchants;
        std::map<StatProfile, std::vector<GemCandidate>> _gems;
        std::vector<std::pair<uint8, uint32>> _quivers;     // hunters: quivers and ammo pouches, sorted
        std::vector<std::pair<uint8, uint32>> _instantPoisons;  // rogues: (required level, item), sorted
        std::vector<std::pair<uint8, uint32>> _deadlyPoisons;
        std::vector<std::pair<uint8, uint32>> _cripplingPoisons;
    };
}

#endif

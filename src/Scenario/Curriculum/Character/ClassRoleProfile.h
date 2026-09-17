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

#ifndef ANIMUS_LIB_CURRICULUM_CLASS_ROLE_PROFILE_H
#define ANIMUS_LIB_CURRICULUM_CLASS_ROLE_PROFILE_H

#include "Define.h"
#include "SharedDefines.h"
#include <array>
#include <string>
#include <vector>

namespace Animus::Curriculum
{
    enum class Role : uint8
    {
        Dps,
        Tank,
        Heal,
    };

    constexpr uint32 ROLE_COUNT = 3;

    /// The playable races and classes, in the order of every one-hot over them (observations, critic state).
    constexpr std::array<uint8, 10> PLAYABLE_RACES =
    {
        RACE_HUMAN, RACE_ORC, RACE_DWARF, RACE_NIGHTELF, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_GNOME, RACE_TROLL,
        RACE_BLOODELF, RACE_DRAENEI
    };

    constexpr std::array<uint8, 10> PLAYABLE_CLASSES =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_DEATH_KNIGHT, CLASS_SHAMAN,
        CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    /// Write a one-hot of `value` over `order` into `out` (nothing when `value` is not in it).
    template <std::size_t N>
    void WriteOneHot(std::array<uint8, N> const& order, uint8 value, float* out)
    {
        for (std::size_t i = 0; i < N; ++i)
            out[i] = order[i] == value ? 1.0f : 0.0f;
    }

    /// Which item stats a spec's gear is chosen for.
    enum class StatProfile : uint8
    {
        StrengthMelee,      // strength, attack power, melee ratings
        AgilityMelee,       // agility, attack power, melee ratings (rogue, feral cat, enhancement)
        Ranged,             // agility, attack power, ranged/melee ratings (hunter)
        Caster,             // intellect, spell power, spell ratings
        Healer,             // intellect, spirit, spell power, mp5
        Tank,               // stamina, avoidance, defense, block, threat stats
    };

    enum class RangeBand : uint8
    {
        Melee,              // stands next to the dummy
        Ranged,             // stands at casting/shooting range
    };

    /// How a spec wields weapons. Layouts are tried in order; the first whose slots can be filled at
    /// the bot's level and with its talents (dual wield) is used.
    enum class WeaponLayout : uint8
    {
        TwoHand,            // one two-handed melee weapon
        DualWield,          // two one-handed weapons (needs the dual wield skill)
        DualWieldDaggers,   // two daggers (Mutilate, Backstab and Ambush need them)
        OneHand,            // a main-hand weapon alone (before dual wield is learned)
        OneHandShield,
        OneHandHeld,        // one-hander plus a held-in-off-hand item
        Staff,
        TwoHandRanged,      // two-handed stat stick plus a bow, gun or crossbow (hunters)
    };

    struct SpecProfile
    {
        std::string Name;               // "arms"
        uint8 TabPage = 0;              // talent tab: 0, 1 or 2 in TalentTab.dbc order
        StatProfile Stats = StatProfile::StrengthMelee;
        RangeBand Range = RangeBand::Melee;
        std::vector<WeaponLayout> Weapons;
        bool Wand = false;              // also fill the ranged slot with a wand
    };

    /// One trained model: a class in one role, over every spec that plays the role.
    struct ClassRoleProfile
    {
        std::string Name;               // "<class>_<role>": the layout's name; its models add a stage suffix
        uint8 Class = 0;
        Role PlayRole = Role::Dps;
        std::vector<SpecProfile> Specs;
    };

    /// Every class/role model, in a stable order.
    std::vector<ClassRoleProfile> const& ClassRoleProfiles();

    [[nodiscard]] char const* RoleName(Role role);

    /// A random role: tank with `tankChance` percent, healer with `healerChance`, a damage dealer otherwise (one roll
    /// from the world thread's random numbers, so seeded episodes draw the same role).
    [[nodiscard]] Role RollRole(int32 tankChance, int32 healerChance);

    /// Per-decision damage scale of a level: roughly how a well-geared character's damage grows with level, so damage
    /// features and rewards have a similar size at every level (about 16 at level 1, 230 at 40, 3500 at 80).
    [[nodiscard]] float DamageScale(uint8 level);
}

#endif

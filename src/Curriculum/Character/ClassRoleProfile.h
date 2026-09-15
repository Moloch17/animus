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

#ifndef ANIMUS_CURRICULUM_CLASS_ROLE_PROFILE_H
#define ANIMUS_CURRICULUM_CLASS_ROLE_PROFILE_H

#include "Define.h"
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
        std::string ScenarioName;       // "<class>_<role>", the AnimusForge.Scenario name
        uint8 Class = 0;
        Role PlayRole = Role::Dps;
        std::vector<SpecProfile> Specs;
    };

    /// Every class/role model, in a stable order.
    std::vector<ClassRoleProfile> const& ClassRoleProfiles();

    [[nodiscard]] char const* RoleName(Role role);
}

#endif

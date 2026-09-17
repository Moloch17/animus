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

#include "ClassRoleProfile.h"
#include "Random.h"
#include <cmath>

namespace
{
    using Animus::Curriculum::RangeBand;
    using Animus::Curriculum::Role;
    using Animus::Curriculum::SpecProfile;
    using Animus::Curriculum::StatProfile;
    using Animus::Curriculum::WeaponLayout;

    SpecProfile Spec(std::string name, uint8 tabPage, StatProfile stats, RangeBand range,
        std::vector<WeaponLayout> weapons, bool wand = false)
    {
        SpecProfile spec;
        spec.Name = std::move(name);
        spec.TabPage = tabPage;
        spec.Stats = stats;
        spec.Range = range;
        spec.Weapons = std::move(weapons);
        spec.Wand = wand;
        return spec;
    }
}

char const* Animus::Curriculum::RoleName(Role role)
{
    switch (role)
    {
        case Role::Dps:  return "dps";
        case Role::Tank: return "tank";
        case Role::Heal: return "heal";
    }

    return "dps";
}

Animus::Curriculum::Role Animus::Curriculum::RollRole(int32 tankChance, int32 healerChance)
{
    int32 const roll = irand(0, 99);
    return roll < tankChance ? Role::Tank : roll < tankChance + healerChance ? Role::Heal : Role::Dps;
}

float Animus::Curriculum::DamageScale(uint8 level)
{
    return 15.0f * std::exp(0.068f * float(level));
}

std::vector<Animus::Curriculum::ClassRoleProfile> const& Animus::Curriculum::ClassRoleProfiles()
{
    using enum WeaponLayout;
    using SP = StatProfile;
    constexpr RangeBand Melee = RangeBand::Melee;
    constexpr RangeBand Ranged = RangeBand::Ranged;

    // Talent tabs follow TalentTab.dbc order (TabPage). The role decides the specs, the gear stats and where the bot
    // stands.
    static std::vector<ClassRoleProfile> const profiles =
    {
        { "warrior_dps", CLASS_WARRIOR, Role::Dps, {
            Spec("arms", 0, SP::StrengthMelee, Melee, { TwoHand }),
            Spec("fury", 1, SP::StrengthMelee, Melee, { DualWield, TwoHand }) } },
        { "warrior_tank", CLASS_WARRIOR, Role::Tank, {
            Spec("protection", 2, SP::Tank, Melee, { OneHandShield, TwoHand }) } },

        { "paladin_heal", CLASS_PALADIN, Role::Heal, {
            Spec("holy", 0, SP::Healer, Melee, { OneHandShield, OneHandHeld, TwoHand }) } },
        { "paladin_tank", CLASS_PALADIN, Role::Tank, {
            Spec("protection", 1, SP::Tank, Melee, { OneHandShield, TwoHand }) } },
        { "paladin_dps", CLASS_PALADIN, Role::Dps, {
            Spec("retribution", 2, SP::StrengthMelee, Melee, { TwoHand }) } },

        { "hunter_dps", CLASS_HUNTER, Role::Dps, {
            Spec("beast_mastery", 0, SP::Ranged, Ranged, { TwoHandRanged }),
            Spec("marksmanship", 1, SP::Ranged, Ranged, { TwoHandRanged }),
            Spec("survival", 2, SP::Ranged, Ranged, { TwoHandRanged }) } },

        { "rogue_dps", CLASS_ROGUE, Role::Dps, {
            Spec("assassination", 0, SP::AgilityMelee, Melee, { DualWieldDaggers, DualWield, OneHand }),
            Spec("combat", 1, SP::AgilityMelee, Melee, { DualWield, OneHand }),
            Spec("subtlety", 2, SP::AgilityMelee, Melee, { DualWieldDaggers, DualWield, OneHand }) } },

        { "priest_heal", CLASS_PRIEST, Role::Heal, {
            Spec("discipline", 0, SP::Healer, Ranged, { Staff, OneHandHeld }, true),
            Spec("holy", 1, SP::Healer, Ranged, { Staff, OneHandHeld }, true) } },
        { "priest_dps", CLASS_PRIEST, Role::Dps, {
            Spec("shadow", 2, SP::Caster, Ranged, { Staff, OneHandHeld }, true) } },

        { "deathknight_tank", CLASS_DEATH_KNIGHT, Role::Tank, {
            Spec("blood", 0, SP::Tank, Melee, { TwoHand }) } },
        { "deathknight_dps", CLASS_DEATH_KNIGHT, Role::Dps, {
            Spec("frost", 1, SP::StrengthMelee, Melee, { DualWield, TwoHand }),
            Spec("unholy", 2, SP::StrengthMelee, Melee, { TwoHand }) } },

        { "shaman_dps", CLASS_SHAMAN, Role::Dps, {
            Spec("elemental", 0, SP::Caster, Ranged, { OneHandShield, OneHandHeld, Staff }),
            Spec("enhancement", 1, SP::AgilityMelee, Melee, { DualWield, TwoHand }) } },
        { "shaman_heal", CLASS_SHAMAN, Role::Heal, {
            Spec("restoration", 2, SP::Healer, Ranged, { OneHandShield, OneHandHeld, Staff }) } },

        { "mage_dps", CLASS_MAGE, Role::Dps, {
            Spec("arcane", 0, SP::Caster, Ranged, { Staff, OneHandHeld }, true),
            Spec("fire", 1, SP::Caster, Ranged, { Staff, OneHandHeld }, true),
            Spec("frost", 2, SP::Caster, Ranged, { Staff, OneHandHeld }, true) } },

        { "warlock_dps", CLASS_WARLOCK, Role::Dps, {
            Spec("affliction", 0, SP::Caster, Ranged, { Staff, OneHandHeld }, true),
            Spec("demonology", 1, SP::Caster, Ranged, { Staff, OneHandHeld }, true),
            Spec("destruction", 2, SP::Caster, Ranged, { Staff, OneHandHeld }, true) } },

        { "druid_dps", CLASS_DRUID, Role::Dps, {
            Spec("balance", 0, SP::Caster, Ranged, { Staff, OneHandHeld }),
            Spec("feral_cat", 1, SP::AgilityMelee, Melee, { TwoHand, Staff }) } },
        { "druid_tank", CLASS_DRUID, Role::Tank, {
            Spec("feral_bear", 1, SP::Tank, Melee, { TwoHand, Staff }) } },
        { "druid_heal", CLASS_DRUID, Role::Heal, {
            Spec("restoration", 2, SP::Healer, Ranged, { Staff, OneHandHeld }) } },
    };

    return profiles;
}

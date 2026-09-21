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

#include "ClassProfile.h"
#include "Random.h"
#include <algorithm>
#include <cmath>

namespace
{
    using Animus::Curriculum::RangeBand;
    using Animus::Curriculum::Role;
    using Animus::Curriculum::SpecProfile;
    using Animus::Curriculum::StatProfile;
    using Animus::Curriculum::WeaponLayout;

    SpecProfile Spec(std::string name, uint8 tabPage, Role role, StatProfile stats, RangeBand range,
        std::vector<WeaponLayout> weapons, bool wand = false)
    {
        SpecProfile spec;
        spec.Name = std::move(name);
        spec.TabPage = tabPage;
        spec.PlayRole = role;
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

std::vector<Animus::Curriculum::ClassProfile> const& Animus::Curriculum::ClassProfiles()
{
    using enum WeaponLayout;
    using SP = StatProfile;
    constexpr RangeBand Melee = RangeBand::Melee;
    constexpr RangeBand Ranged = RangeBand::Ranged;

    // Talent tabs follow TalentTab.dbc order (TabPage). One entry per class, holding every spec it can be; the
    // spec decides the role, the gear stats and where the bot stands.
    static std::vector<ClassProfile> const profiles =
    {
        { "warrior", CLASS_WARRIOR, {
            Spec("arms", 0, Role::Dps, SP::StrengthMelee, Melee, { TwoHand }),
            Spec("fury", 1, Role::Dps, SP::StrengthMelee, Melee, { DualWield, TwoHand }),
            Spec("protection", 2, Role::Tank, SP::Tank, Melee, { OneHandShield, TwoHand }) } },

        { "paladin", CLASS_PALADIN, {
            Spec("holy", 0, Role::Heal, SP::Healer, Melee, { OneHandShield, OneHandHeld, TwoHand }),
            Spec("protection", 1, Role::Tank, SP::Tank, Melee, { OneHandShield, TwoHand }),
            Spec("retribution", 2, Role::Dps, SP::StrengthMelee, Melee, { TwoHand }) } },

        { "hunter", CLASS_HUNTER, {
            Spec("beast_mastery", 0, Role::Dps, SP::Ranged, Ranged, { TwoHandRanged }),
            Spec("marksmanship", 1, Role::Dps, SP::Ranged, Ranged, { TwoHandRanged }),
            Spec("survival", 2, Role::Dps, SP::Ranged, Ranged, { TwoHandRanged }) } },

        { "rogue", CLASS_ROGUE, {
            Spec("assassination", 0, Role::Dps, SP::AgilityMelee, Melee, { DualWieldDaggers, DualWield, OneHand }),
            Spec("combat", 1, Role::Dps, SP::AgilityMelee, Melee, { DualWield, OneHand }),
            Spec("subtlety", 2, Role::Dps, SP::AgilityMelee, Melee, { DualWieldDaggers, DualWield, OneHand }) } },

        { "priest", CLASS_PRIEST, {
            Spec("discipline", 0, Role::Heal, SP::Healer, Ranged, { Staff, OneHandHeld }, true),
            Spec("holy", 1, Role::Heal, SP::Healer, Ranged, { Staff, OneHandHeld }, true),
            Spec("shadow", 2, Role::Dps, SP::Caster, Ranged, { Staff, OneHandHeld }, true) } },

        { "deathknight", CLASS_DEATH_KNIGHT, {
            Spec("blood", 0, Role::Tank, SP::Tank, Melee, { TwoHand }),
            Spec("frost", 1, Role::Dps, SP::StrengthMelee, Melee, { DualWield, TwoHand }),
            Spec("unholy", 2, Role::Dps, SP::StrengthMelee, Melee, { TwoHand }) } },

        { "shaman", CLASS_SHAMAN, {
            Spec("elemental", 0, Role::Dps, SP::Caster, Ranged, { OneHandShield, OneHandHeld, Staff }),
            Spec("enhancement", 1, Role::Dps, SP::AgilityMelee, Melee, { DualWield, TwoHand }),
            Spec("restoration", 2, Role::Heal, SP::Healer, Ranged, { OneHandShield, OneHandHeld, Staff }) } },

        { "mage", CLASS_MAGE, {
            Spec("arcane", 0, Role::Dps, SP::Caster, Ranged, { Staff, OneHandHeld }, true),
            Spec("fire", 1, Role::Dps, SP::Caster, Ranged, { Staff, OneHandHeld }, true),
            Spec("frost", 2, Role::Dps, SP::Caster, Ranged, { Staff, OneHandHeld }, true) } },

        { "warlock", CLASS_WARLOCK, {
            Spec("affliction", 0, Role::Dps, SP::Caster, Ranged, { Staff, OneHandHeld }, true),
            Spec("demonology", 1, Role::Dps, SP::Caster, Ranged, { Staff, OneHandHeld }, true),
            Spec("destruction", 2, Role::Dps, SP::Caster, Ranged, { Staff, OneHandHeld }, true) } },

        { "druid", CLASS_DRUID, {
            Spec("balance", 0, Role::Dps, SP::Caster, Ranged, { Staff, OneHandHeld }),
            Spec("feral_cat", 1, Role::Dps, SP::AgilityMelee, Melee, { TwoHand, Staff }),
            Spec("feral_bear", 1, Role::Tank, SP::Tank, Melee, { TwoHand, Staff }),
            Spec("restoration", 2, Role::Heal, SP::Healer, Ranged, { Staff, OneHandHeld }) } },
    };

    return profiles;
}

std::vector<uint8> Animus::Curriculum::ClassProfile::SpecsOf(Role role) const
{
    std::vector<uint8> found;
    for (uint8 i = 0; i < uint8(Specs.size()); ++i)
        if (Specs[i].PlayRole == role)
            found.push_back(i);

    return found;
}

bool Animus::Curriculum::ClassProfile::Plays(Role role) const
{
    return std::any_of(Specs.begin(), Specs.end(),
        [role](SpecProfile const& spec) { return spec.PlayRole == role; });
}

uint8 Animus::Curriculum::DrawSpec(ClassProfile const& profile, Role role)
{
    if (profile.Specs.empty())
        return 0;

    std::vector<uint8> const of = profile.SpecsOf(role);
    if (of.empty())
        return uint8(urand(0, uint32(profile.Specs.size()) - 1));

    return of[urand(0, uint32(of.size()) - 1)];
}

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

#include "ClassRoleAssets.h"
#include "ObjectMgr.h"
#include <array>

namespace
{
    constexpr std::array<uint8, 10> PLAYABLE_RACES =
    {
        RACE_HUMAN, RACE_ORC, RACE_DWARF, RACE_NIGHTELF, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_GNOME, RACE_TROLL,
        RACE_BLOODELF, RACE_DRAENEI
    };

    /// A class's kit, talents and catalog: the same for all of its roles.
    struct ClassAssets
    {
        std::unique_ptr<Animus::ClassRole::ClassKit> Kit;
        std::unique_ptr<Animus::ClassRole::TalentBuilder> Talents;
        std::unique_ptr<Animus::ClassRole::ActionCatalog> Catalog;
    };
}

Animus::ClassRole::ClassRoleAssets const& Animus::ClassRole::ClassRoleAssets::For(ClassRoleProfile const& profile)
{
    static std::map<uint8, ClassAssets> classes;
    static std::map<ClassRoleProfile const*, ClassRoleAssets> assets;

    if (auto const itr = assets.find(&profile); itr != assets.end())
        return itr->second;

    ClassAssets& shared = classes[profile.Class];
    if (!shared.Kit)
    {
        shared.Kit = std::make_unique<ClassKit>(profile.Class);
        shared.Talents = std::make_unique<TalentBuilder>(profile.Class);
        shared.Catalog = std::make_unique<ActionCatalog>(profile.Class, *shared.Kit, *shared.Talents);
    }

    ClassRoleAssets& entry = assets[&profile];
    entry.Profile = &profile;
    for (uint8 race : PLAYABLE_RACES)
        if (sObjectMgr->GetPlayerInfo(race, profile.Class))
            entry.Races.push_back(race);

    entry.Kit = shared.Kit.get();
    entry.Talents = shared.Talents.get();
    entry.Catalog = shared.Catalog.get();
    entry.Gear = std::make_unique<GearBuilder>(profile, *shared.Kit);
    return entry;
}

Animus::ClassRole::ClassRoleProfile const* Animus::ClassRole::ClassRoleAssets::FindProfile(uint8 playerClass, Role role)
{
    for (ClassRoleProfile const& profile : ClassRoleProfiles())
        if (profile.Class == playerClass && profile.PlayRole == role)
            return &profile;

    return nullptr;
}

std::vector<uint8> Animus::ClassRole::ClassRoleAssets::ClassesForRole(uint8 level, Role role)
{
    std::vector<uint8> classes;
    for (ClassRoleProfile const& profile : ClassRoleProfiles())
        if (profile.PlayRole == role && For(profile).Kit->MinLevel() <= level && !For(profile).Races.empty())
            classes.push_back(profile.Class);

    return classes;
}

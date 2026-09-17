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

#include "ClassRoleAssets.h"
#include "ObjectMgr.h"
#include <algorithm>
#include <array>

namespace
{
    /// A class's kit, talents and catalog: the same for all of its roles.
    struct ClassAssets
    {
        std::unique_ptr<Animus::Curriculum::ClassKit> Kit;
        std::unique_ptr<Animus::Curriculum::TalentBuilder> Talents;
        std::unique_ptr<Animus::Curriculum::ActionCatalog> Catalog;
    };
}

Animus::Curriculum::ClassRoleAssets const& Animus::Curriculum::ClassRoleAssets::For(
    ClassRoleProfile const& profile)
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

Animus::Curriculum::ClassRoleProfile const* Animus::Curriculum::ClassRoleAssets::FindProfile(
    uint8 playerClass, Role role)
{
    for (ClassRoleProfile const& profile : ClassRoleProfiles())
        if (profile.Class == playerClass && profile.PlayRole == role)
            return &profile;

    return nullptr;
}

std::vector<uint8> Animus::Curriculum::ClassRoleAssets::ClassesForRole(uint8 level, Role role)
{
    // Cheap checks only: building a profile's assets takes seconds (see StageScenario, which warms them).
    std::vector<uint8> classes;
    for (ClassRoleProfile const& profile : ClassRoleProfiles())
    {
        if (profile.PlayRole != role || ClassKit::MinLevelOf(profile.Class) > level)
            continue;

        if (std::any_of(PLAYABLE_RACES.begin(), PLAYABLE_RACES.end(),
            [&profile](uint8 race) { return sObjectMgr->GetPlayerInfo(race, profile.Class) != nullptr; }))
            classes.push_back(profile.Class);
    }

    return classes;
}

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

#ifndef ANIMUS_LIB_CURRICULUM_CLASS_ASSETS_H
#define ANIMUS_LIB_CURRICULUM_CLASS_ASSETS_H

#include "ActionCatalog.h"
#include "ClassKit.h"
#include "ClassProfile.h"
#include "GearBuilder.h"
#include "TalentBuilder.h"
#include <map>
#include <memory>
#include <vector>

namespace Animus::Curriculum
{
    /// Everything needed to build and play a character of one class: its kit, talents and action catalog, gear for
    /// every spec it has, and the races that can be it.
    ///
    /// Built once per class and shared by every scenario and scripted player that needs it: item pools and trainer
    /// data take a few seconds each. The kit, the talents and the catalog were always shared by the class's roles;
    /// now the gear is too, because one GearBuilder covers every spec in the profile it was given.
    struct ClassAssets
    {
        ClassProfile const* Profile = nullptr;
        std::vector<uint8> Races;
        ClassKit const* Kit = nullptr;
        TalentBuilder const* Talents = nullptr;
        ActionCatalog const* Catalog = nullptr;
        std::unique_ptr<GearBuilder> Gear;

        /// The assets of a profile of ClassProfiles(), built on first use (world thread only).
        static ClassAssets const& For(ClassProfile const& profile);

        /// The profile of `playerClass`, if it is a class that is played.
        static ClassProfile const* FindProfile(uint8 playerClass);

        /// Classes a player of `level` can be that have a spec playing `role`.
        static std::vector<uint8> ClassesForRole(uint8 level, Role role);
    };
}

#endif

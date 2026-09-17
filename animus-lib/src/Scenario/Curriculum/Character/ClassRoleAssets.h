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

#ifndef ANIMUS_LIB_CURRICULUM_CLASS_ROLE_ASSETS_H
#define ANIMUS_LIB_CURRICULUM_CLASS_ROLE_ASSETS_H

#include "ActionCatalog.h"
#include "ClassKit.h"
#include "ClassRoleProfile.h"
#include "GearBuilder.h"
#include "TalentBuilder.h"
#include <map>
#include <memory>
#include <vector>

namespace Animus::Curriculum
{
    /// Everything needed to build and play a character of one class/role: the class's kit, talents and action
    /// catalog (shared by the class's roles), the role's gear, and the races that can be the class.
    ///
    /// Built once per class/role and shared by every scenario and scripted player that needs it: item pools and
    /// trainer data take a few seconds each.
    struct ClassRoleAssets
    {
        ClassRoleProfile const* Profile = nullptr;
        std::vector<uint8> Races;
        ClassKit const* Kit = nullptr;
        TalentBuilder const* Talents = nullptr;
        ActionCatalog const* Catalog = nullptr;
        std::unique_ptr<GearBuilder> Gear;

        /// The assets of a profile of ClassRoleProfiles(), built on first use (world thread only).
        static ClassRoleAssets const& For(ClassRoleProfile const& profile);

        /// The profile of `playerClass` in `role`, if the class has that role.
        static ClassRoleProfile const* FindProfile(uint8 playerClass, Role role);

        /// Classes a player of `level` can be that have `role`.
        static std::vector<uint8> ClassesForRole(uint8 level, Role role);
    };
}

#endif

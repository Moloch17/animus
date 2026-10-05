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

#ifndef ANIMUS_MOVEMENT_PLAYER_LINK_H
#define ANIMUS_MOVEMENT_PLAYER_LINK_H

#include "Client.h"
#include "Define.h"

/// **The realm's stand-in for the forge's Movement/PlayerLink.h** (module-owned, outside the bundle's cmp). The forge's
/// header is its server link over ClientMovement, a forge-core refactor of the movement handler that a stock core does
/// not have; the bundle's StageState.h includes it only for LinkMemory, the per-seat record a seat state keeps. That
/// record is here, field for field the forge's at 23b745802, so the shared files compile unchanged. The realm's server
/// link is the module's own (src/Client). To be retired when the forge moves LinkMemory into a header of its own.
namespace Animus::Movement
{
    struct LinkMemory
    {
        float GoodX = 0.0f;
        float GoodY = 0.0f;
        float GoodZ = 0.0f;
        float GoodYaw = 0.0f;
        bool HasGood = false;
        uint32 InvalidStreak = 0;
        uint32 Landings = 0;
        float FallDamage = 0.0f;
        uint32 FallDeaths = 0;
    };
}

#endif

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

#ifndef ANIMUS_LIB_FORGE_SHIM_H
#define ANIMUS_LIB_FORGE_SHIM_H

/// **The forge core's ForgeCore (src/server/game/Forge/Forge.h), as a realm answers it** (module-owned, outside the
/// bundle's cmp). At forge 23b745802 the bundle's MoveBlock.cpp and EncoderSupport.cpp still include Forge.h but call
/// nothing from it; the stock core has no such header, so this one stands in. The earlier refresh's MoveSpline
/// ReaimFacing shim is gone with the splines it turned (the player controller moves seats now).
namespace ForgeCore
{
    /// A realm's companions are always watched by their owners' clients.
    inline bool HasClients() { return true; }
}

#endif

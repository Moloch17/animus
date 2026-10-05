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

#include "Define.h"
#include "MoveSpline.h"

/*
 * The forge core's seams as a stock AzerothCore answers them: ForgeCore (src/server/game/Forge/Forge.h) and
 * MoveSpline::ReaimFacing, a forge core addition. The module's own file (tools/update-animus-lib.sh keeps Core/
 * and points the bundle's ReaimFacing calls here), so a refresh never replaces it with the forge's.
 *
 * The forge core's ForgeCore (src/server/game/Forge/Forge.h), as a realm answers it: the bundle's curriculum asks it
 * whether real clients watch the seats (to send them a run's turns, MoveBlock and EncoderSupport::ShowFacing). On a
 * realm they always do. The module's own file (tools/update-animus-lib.sh keeps Core/), so a refresh never replaces
 * it with the forge's, which needs the forge core.
 */
namespace ForgeCore
{
    /// A realm's companions are always watched by their owners' clients.
    inline bool HasClients() { return true; }
}

namespace AnimusLib
{
    /// The forge core's MoveSpline::ReaimFacing on a stock core, whose MoveSpline keeps those fields protected: a
    /// derived class may name them, and the member pointers it names reach any MoveSpline. Turns the head of an
    /// orientation-fixed run under way (and its final angle) without relaunching it; false for any other run and for
    /// a finished one.
    struct SplineReaim : Movement::MoveSpline
    {
        static bool Apply(Movement::MoveSpline& run, float angle)
        {
            Movement::MoveSplineFlag& flags = run.*(&SplineReaim::splineflags);
            if (flags.done || !flags.hasFlag(Movement::MoveSplineFlag::OrientationFixed))
                return false;
            run.*(&SplineReaim::initialOrientation) = angle;
            if (flags.final_angle)
                (run.*(&SplineReaim::facing)).angle = angle;
            return true;
        }
    };

    inline bool ReaimFacing(Movement::MoveSpline& run, float angle) { return SplineReaim::Apply(run, angle); }
}

#endif

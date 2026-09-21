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

#include "MoveBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include <boost/json/object.hpp>
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
#include "Player.h"
#include "SeatView.h"
#include <cmath>

namespace
{
    using Animus::Curriculum::MoveBlock;

    constexpr uint32 MOVE_POINT_ID = 0x4D56;    // "MV": this block's spline, distinct from the duel block's
    constexpr float YARD_SCALE = 40.0f;         // distances are reported as a fraction of this
    constexpr float RUN_SPEED = 7.0f;           // yards a second, unmounted and unhasted (TravelBlock's)

    /// The world angle a bearing points at, given where the seat is looking. Bearings run clockwise from straight
    /// ahead, and WoW orientation runs counter-clockwise, so the eighth-turns are subtracted.
    float HeadingOf(float facing, uint32 bearing)
    {
        float const heading = facing - float(bearing) * float(M_PI) / 4.0f;
        return Position::NormalizeOrientation(heading);
    }

    /// `to`'s direction in the seat's own frame: 0 straight ahead, wrapped to (-pi, pi].
    float RelativeBearing(Position const& from, float facing, WorldObject const& to)
    {
        float const relative = from.GetAngle(to.GetPositionX(), to.GetPositionY()) - facing;
        return std::atan2(std::sin(relative), std::cos(relative));
    }

    /// Point the seat's head without touching its feet: a spline overwrites orientation as it runs, so a facing set
    /// any other way is lost the moment the seat moves. This is what makes a strafe expressible.
    void FaceWhile(Player* bot, Unit const* target, uint32 mode, float heading)
    {
        Movement::MoveSplineInit init(bot);
        if (mode == MoveBlock::ACTION_FACE_TARGET && target)
            init.SetFacing(target);
        else if (mode == MoveBlock::ACTION_FACE_HEADING)
            init.SetFacing(heading);
        else
            init.SetFacing(bot->GetOrientation());
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::MoveBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

void Animus::Curriculum::MoveBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["bearings"] = uint32(BEARING_COUNT);
    block["step_yards"] = double(STEP_YARDS);
}

std::string Animus::Curriculum::MoveBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    static constexpr std::array<char const*, ACTION_COUNT> NAMES =
    {
        "move_forward", "move_forward_right", "move_right", "move_back_right",
        "move_back", "move_back_left", "move_left", "move_forward_left",
        "halt", "face_target", "face_heading", "face_hold",
    };

    return local < NAMES.size() ? NAMES[local] : std::string();
}

void Animus::Curriculum::MoveBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Layout const& layout = *view.L;
    BlockSlice const& slice = layout.Slice(BlockId::Move);
    float* out = obs + slice.ObsFirst;
    Player* bot = view.Bot;

    // Everything a seat needs to place its feet, and nothing about whether it has an enemy: this block is the one
    // that still works when there is nothing to fight.
    bool const canMove = bot && bot->IsAlive() && !bot->HasUnitState(Encoding::IMMOBILE_STATES);

    if (bot)
    {
        float const facing = bot->GetOrientation();
        out[OBS_MOVING] = bot->isMoving() ? 1.0f : 0.0f;
        out[OBS_SPEED] = bot->GetSpeed(MOVE_RUN) / RUN_SPEED;
        out[OBS_FACING_SIN] = std::sin(facing);
        out[OBS_FACING_COS] = std::cos(facing);

        if (view.HeldBearing < BEARING_COUNT)
            out[OBS_BEARING_HELD + view.HeldBearing] = 1.0f;
        else
            out[OBS_BEARING_NONE] = 1.0f;

        if (Unit const* target = view.Target)
        {
            float const relative = RelativeBearing(*bot, facing, *target);
            out[OBS_TARGET_BEARING_SIN] = std::sin(relative);
            out[OBS_TARGET_BEARING_COS] = std::cos(relative);
            out[OBS_TARGET_DISTANCE] = std::min(1.0f, bot->GetExactDist2d(target) / YARD_SCALE);
        }

        // The nearest ground effect it is not standing in, in the same frame: which way it lies and how wide, so
        // the seat can walk round one rather than only out of one. Its bearing is already relative to facing.
        if (view.NearestHazard.Present)
        {
            out[OBS_HAZARD_BEARING_SIN] = std::sin(view.NearestHazard.Bearing);
            out[OBS_HAZARD_BEARING_COS] = std::cos(view.NearestHazard.Bearing);
            out[OBS_HAZARD_DISTANCE] = std::min(1.0f, view.NearestHazard.Distance / YARD_SCALE);
            out[OBS_HAZARD_RADIUS] = std::min(1.0f, view.NearestHazard.Radius / YARD_SCALE);
        }
    }

    if (!mask)
        return;

    uint8* allowed = mask + slice.ActionFirst;
    for (uint32 bearing = 0; bearing < BEARING_COUNT; ++bearing)
        allowed[ACTION_BEARING_FIRST + bearing] = canMove ? 1 : 0;

    // The bearing already being walked is masked: pressing it again would be a repeat of a key already held, and
    // the option machinery re-issues the spline each decision without being asked.
    if (canMove && view.HeldBearing < BEARING_COUNT && view.Option
        && view.Option->Running(SeatOptionKind::MoveBearing, view.NowMs))
        allowed[ACTION_BEARING_FIRST + view.HeldBearing] = 0;

    // Halting is only worth offering while something is being walked.
    allowed[ACTION_HALT] = canMove && view.HeldBearing < BEARING_COUNT ? 1 : 0;

    // Where it looks is a choice it can always make, alive and able to turn. Facing the target needs one.
    bool const canTurn = bot && bot->IsAlive() && !bot->HasUnitState(Encoding::STUN_STATES);
    allowed[ACTION_FACE_TARGET] = canTurn && view.Target && view.Target->IsAlive() ? 1 : 0;
    allowed[ACTION_FACE_HEADING] = canTurn ? 1 : 0;
    allowed[ACTION_FACE_HOLD] = canTurn ? 1 : 0;
    for (uint32 face = ACTION_FACE_TARGET; face <= ACTION_FACE_HOLD; ++face)
        if (view.FacingMode == face)
            allowed[face] = 0;              // already holding its head that way
}

void Animus::Curriculum::MoveBlock::BeforeApply(SeatView& view, SeatActionResult& /*result*/) const
{
    Player* bot = view.Bot;
    if (!bot || !view.Option)
        return;

    if (!view.Option->Running(SeatOptionKind::MoveBearing, view.NowMs) || view.HeldBearing >= BEARING_COUNT)
    {
        view.HeldBearing = 0xFF;
        return;
    }

    if (!bot->IsAlive() || bot->HasUnitState(Encoding::IMMOBILE_STATES))
        return;

    // Re-aimed from where the seat is now, every decision it keeps walking. Aiming once at a point chosen when the
    // key went down would walk it into the first wall the ground put in the way; recomputing lets the path bend.
    float const heading = HeadingOf(bot->GetOrientation(), view.HeldBearing);
    Position destination = *bot;
    destination.Relocate(bot->GetPositionX() + STEP_YARDS * std::cos(heading),
        bot->GetPositionY() + STEP_YARDS * std::sin(heading), bot->GetPositionZ());
    Encoding::SnapToGround(bot->GetMap(), bot->GetPhaseMask(), destination, bot->GetPositionZ(), STEP_YARDS);

    // Pathfinding on, which is the default: a bearing is where the seat wants to go, not a licence to walk through
    // a wall to get there.
    Encoding::MoveTo(bot, MOVE_POINT_ID, destination.GetPositionX(), destination.GetPositionY(),
        destination.GetPositionZ());
    FaceWhile(bot, view.Target, view.FacingMode, heading);
}

void Animus::Curriculum::MoveBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    Player* bot = view.Bot;
    if (!bot || !view.Option)
        return;

    if (local < ACTION_HALT)
    {
        view.HeldBearing = uint8(local - ACTION_BEARING_FIRST);
        view.Option->Start(SeatOptionKind::MoveBearing, view.NowMs + view.Options.MoveBearingMs);
        // BeforeApply walks it on the next decision; start it now so the seat is not still for one.
        BeforeApply(view, result);
        return;
    }

    if (local == ACTION_HALT)
    {
        view.HeldBearing = 0xFF;
        view.Option->Stop(SeatOptionKind::MoveBearing);
        bot->StopMoving();
        return;
    }

    if (local > ACTION_FACE_HOLD)
        return;

    view.FacingMode = uint8(local);
    FaceWhile(bot, view.Target, local, HeadingOf(bot->GetOrientation(),
        view.HeldBearing < BEARING_COUNT ? view.HeldBearing : 0u));
}

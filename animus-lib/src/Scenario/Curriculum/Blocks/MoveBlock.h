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

#ifndef ANIMUS_LIB_CURRICULUM_MOVE_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_MOVE_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// Where the seat puts its feet, answered without reference to anything it is fighting.
    ///
    /// Every movement the curriculum had before this was target-relative: DuelBlock's MOVE_TO_TARGET, MOVE_TO_RANGE,
    /// MOVE_BEHIND, BACK_OFF, KEEP_RANGE, STAY_ON_TARGET and STOP all open with
    /// `if (!target || !target->IsAlive()) return false`, so a seat with nothing to fight could not move at all. The
    /// hazard drill found that the hard way: four million steps with allowed_actions at 1.00, entropy at 0 and the
    /// seats standing still while the fire burned them, until an unkillable emitter was handed to them purely so the
    /// movement actions would unmask. This block is what that hack was standing in for. It needs no target, no enemy
    /// and no objective -- only legs.
    ///
    /// **Bearings are egocentric and durative.** An action picks a compass point relative to where the seat is facing
    /// now (forward, forward-right, right, and so on round the eight), and the seat walks that way until it chooses
    /// otherwise. It is a held key, not a step: a decision is 250 ms of game time and a step measured in one would be
    /// a stutter, which is the same reason KEEP_RANGE is an option rather than a press. The destination is recomputed
    /// from the seat's current position each decision the bearing is held, so the path bends with the ground rather
    /// than aiming at a point chosen when the key went down.
    ///
    /// **Facing is chosen apart from movement**, which is what makes strafing and backpedalling expressible at all.
    /// A player can run one way and look another; a spline that sets its own orientation cannot. FACE_TARGET keeps
    /// the seat turned to what it is fighting while it moves anywhere, FACE_HEADING turns it the way it is going, and
    /// FACE_HOLD leaves it pointing where it already points. Combined with a bearing, those give every gait a player
    /// has: run in facing the enemy, circle it, back away still casting.
    ///
    /// **Resolution comes from the tick, not from more decisions.** AnimusForge.TicksPerDecision cuts a decision into
    /// several world updates, so a spline issued once per decision is still walked in fine steps by the world. Making
    /// the policy decide faster instead would cost an observation, a learner round trip and an action every time.
    ///
    /// This block does not replace the duel block's movement. Closing to melee reach and holding a caster's range are
    /// jobs about a target, and they read better as one action than as a bearing the policy has to steer. What this
    /// adds is everywhere a target cannot help: dodging what lands underfoot, taking a corner, backing out of a
    /// cleave, crossing ground with nothing on it.
    class MoveBlock final : public Block
    {
    public:
        /// Bearings, clockwise from straight ahead. Egocentric: relative to the seat's facing when it chooses, not
        /// to the world or to any enemy.
        enum Bearing : uint32
        {
            BEARING_FORWARD         = 0,
            BEARING_FORWARD_RIGHT   = 1,
            BEARING_RIGHT           = 2,
            BEARING_BACK_RIGHT      = 3,
            BEARING_BACK            = 4,
            BEARING_BACK_LEFT       = 5,
            BEARING_LEFT           = 6,
            BEARING_FORWARD_LEFT    = 7,
            BEARING_COUNT           = 8
        };

        enum Action : uint32
        {
            ACTION_BEARING_FIRST    = 0,
            ACTION_HALT             = ACTION_BEARING_FIRST + BEARING_COUNT,
            /// Face what the seat is fighting while it moves anywhere: the strafe, and the reason a caster can back
            /// away without turning its back on a cast.
            ACTION_FACE_TARGET,
            /// Face the way it is going.
            ACTION_FACE_HEADING,
            /// Leave it facing where it already faces, whatever it does with its feet.
            ACTION_FACE_HOLD,
            ACTION_COUNT
        };

        enum Obs : uint32
        {
            OBS_MOVING              = 0,
            OBS_SPEED               = 1,    // current run speed / 7 yards a second, unmounted and unhasted
            OBS_BEARING_HELD        = 2,    // one-hot over the bearings being walked (BEARING_COUNT); all 0 if none
            OBS_BEARING_NONE        = 2 + BEARING_COUNT,
            /// Which way the seat is looking, as sin and cos of its orientation. Two features rather than one angle,
            /// because an angle wraps and a network asked to learn that 6.28 is 0.01 learns a seam instead.
            OBS_FACING_SIN          = 3 + BEARING_COUNT,
            OBS_FACING_COS          = 4 + BEARING_COUNT,
            /// Where the target is, in the seat's own frame: sin and cos of the bearing to it, and its distance.
            /// All zero without one -- which is the case this block exists for.
            OBS_TARGET_BEARING_SIN  = 5 + BEARING_COUNT,
            OBS_TARGET_BEARING_COS  = 6 + BEARING_COUNT,
            OBS_TARGET_DISTANCE     = 7 + BEARING_COUNT,    // yards / 40
            /// The nearest hostile ground effect the seat is not standing in (SeatView::NearestHazard), in the same
            /// frame: which way it lies, how far, and how wide. Without this the block can dodge only what it is
            /// already burning in.
            OBS_HAZARD_BEARING_SIN  = 8 + BEARING_COUNT,
            OBS_HAZARD_BEARING_COS  = 9 + BEARING_COUNT,
            OBS_HAZARD_DISTANCE     = 10 + BEARING_COUNT,   // yards / 40
            OBS_HAZARD_RADIUS       = 11 + BEARING_COUNT,   // yards / 40
            OBS_COUNT               = 12 + BEARING_COUNT
        };

        /// How far ahead a held bearing aims each decision. Far enough that the seat is still walking when the next
        /// decision comes (7 yards a second unhasted, so a 250 ms decision covers under two), short enough
        /// that the path is recomputed against ground the seat can see.
        static constexpr float STEP_YARDS = 8.0f;

        [[nodiscard]] BlockId Id() const override { return BlockId::Move; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void BeforeApply(SeatView& view, SeatActionResult& result) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        /// Every bearing and the halt are movement: they take the movement repeat pacing and are never charged for
        /// repeating (Actions.Repeat is not levied on movement). The facing actions are not -- turning to look at
        /// something is a press like any other, and a policy that spams it should pay.
        [[nodiscard]] bool IsMovement(uint32 local) const override
        {
            return local <= ACTION_HALT;
        }

        /// A bearing has no fixed sense of toward or away: which way BEARING_FORWARD leads depends on where the seat
        /// is looking. The reverse-move pacing therefore does not apply, and 0 is the honest answer. IsMovement is
        /// what marks these as movement; MoveDirection only says which way a *target-relative* order went.
        [[nodiscard]] int8 MoveDirection(uint32 /*local*/) const override { return 0; }
    };
}

#endif

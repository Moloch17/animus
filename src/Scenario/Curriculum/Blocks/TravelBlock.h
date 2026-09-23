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

#ifndef ANIMUS_LIB_CURRICULUM_TRAVEL_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_TRAVEL_BLOCK_H

#include "Block.h"
#include "Position.h"

class Player;
class SpellInfo;

namespace Animus::Curriculum
{
    /// The mount and the air: summon a ground or flying mount (SeatCharacter gives the level's riding and mounts)
    /// and dismount. Dismounting in the air falls, with a player's fall damage. The seat decides whether a trip is
    /// long enough to be worth the mount's cast time.
    ///
    /// **Where it goes is no longer asked here.** This block used to own MOVE_TO_OBJECTIVE -- a pathfind-to-a-point
    /// order the policy issued once and then watched -- and ASCEND/DESCEND, two coarse fifteen-yard hops. All three
    /// are gone: MoveBlock steers, on the ground and in the air alike, with a held bearing under a held yaw and
    /// pitch, which is how a player actually does it. The hops in particular were the cause of the altitude ratchet
    /// this file used to document: a seat that drifted up stayed up, because coming down again meant choosing
    /// DESCEND often enough to satisfy the arrival check, and altitude costs the whole trip.
    class TravelBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_MOUNTED                 = 0,
            OBS_FLYING_MOUNT            = 1,    // mounted on something that flies here
            OBS_CAN_MOUNT               = 2,    // a ground mount can be summoned now (outdoors, out of combat, ...)
            OBS_CAN_FLY                 = 3,    // a flying mount can be summoned now (a flyable zone, the skill)
            OBS_RIDING_SKILL            = 4,    // / 300
            OBS_INDOORS                 = 5,
            OBS_HEIGHT                  = 6,    // yards above the ground / 50
            OBS_OBJECTIVE               = 7,    // there is an objective
            OBS_OBJECTIVE_DISTANCE      = 8,    // yards on the ground / 500
            OBS_OBJECTIVE_BEARING_SIN   = 9,    // relative to the bot's facing
            OBS_OBJECTIVE_BEARING_COS   = 10,
            OBS_OBJECTIVE_HEIGHT        = 11,   // its height minus the bot's / 50, clamped to [-1, 1]
            OBS_AT_OBJECTIVE            = 12,   // within ARRIVE_DISTANCE on the ground
            OBS_IN_COMBAT               = 13,
            OBS_SPEED                   = 14,   // current movement speed / 7 yd/s / 4
            OBS_MOVING                  = 15,
            /// The route, in the least a policy needs to time ACTION_FOLLOW_ROUTE: that there is one, how much
            /// of it is left, and which way it goes next. The same argument as OBS_CAN_JUMP -- an action whose
            /// mask the policy cannot see is one it cannot learn to press.
            OBS_ROUTE_OK                = 16,
            OBS_ROUTE_REMAIN            = 17,   // yards left along the way / 500, as OBS_OBJECTIVE_DISTANCE
            OBS_ROUTE_SIN               = 18,   // the next corner's direction, in the seat's own frame
            OBS_ROUTE_COS               = 19,
            OBS_COUNT                   = 20
        };

        enum Action : uint32
        {
            ACTION_MOUNT_GROUND         = 0,    // the fastest ground mount it has
            ACTION_MOUNT_FLYING         = 1,    // the fastest flying mount it has
            ACTION_DISMOUNT             = 2,
            /// Walk the next leg of the planned route, and keep walking it.
            ///
            /// The one action in this block that is a journey rather than a state change, and the one the
            /// curriculum retired once already as MOVE_TO_OBJECTIVE. It is back because a trip of thousands of
            /// yards is not the same problem as a trip of a hundred: steering every eight yards for ten minutes
            /// rehearses nothing the first hundred yards did not teach. Masked unless the arena says Routes,
            /// which no arena does by default -- so the stages whose lesson is the steering keep it.
            ACTION_FOLLOW_ROUTE         = 3,
            ACTION_COUNT                = 4
        };

        static constexpr float ARRIVE_DISTANCE = 6.0f;
        /// No vertical limit worth the name: what outdoor arrival has always meant.
        static constexpr float ARRIVE_ANY_RISE = 1000.0f;
        /// What an interior arena uses instead -- under a storey, so a floor above or below is not "arrived".
        static constexpr float ARRIVE_SAME_FLOOR = 4.0f;
        static constexpr float BASE_RUN_SPEED = 7.0f;   // yards a second, unmounted and unhasted
        /// How high a seat may climb above the ground. MoveBlock's pitch reads it: the ceiling is a fact about the
        /// air, which is this block's subject, not about steering.
        static constexpr float MAX_ALTITUDE = 150.0f;

        [[nodiscard]] BlockId Id() const override { return BlockId::Travel; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;

        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void BeforeApply(SeatView& view, SeatActionResult& result) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        // No IsMovement override: summoning a mount and stepping off one are casts and presses, not movement. What
        // is movement now lives entirely in MoveBlock.

        /// The fastest ground and flying mount spells `bot` knows (null when none).
        [[nodiscard]] static SpellInfo const* GroundMount(Player const* bot);
        [[nodiscard]] static SpellInfo const* FlyingMount(Player const* bot);
        /// Whether ACTION_MOUNT_FLYING would be offered right now: the same check the mask makes.
        [[nodiscard]] static bool CanSummonFlying(Player* bot);
        /// Keep MOVEMENTFLAG_CAN_FLY with the seat's flying aura: nothing else sets it without a client.
        static void AllowFlight(Player* bot);
        /// Yards between `bot` and the ground below it (0 when the ground cannot be found).
        [[nodiscard]] static float HeightAboveGround(Player const* bot);
        /// Whether `bot` stands within ARRIVE_DISTANCE of `objective`, on the ground.
        /// `maxRise` bounds how far above or below the objective the seat may stand and still have arrived.
        ///
        /// Arrival is two-dimensional by design -- on a slope the seat stands several yards off the objective's
        /// own z and has plainly got there -- and indoors that is exactly wrong: a seat on the ground floor of
        /// an inn is six yards from an objective on the floor above and has arrived at nothing. The default is
        /// wide enough to change nothing outdoors; an interior arena passes a storey's worth instead.
        [[nodiscard]] static bool AtObjective(Player const* bot, Position const& objective,
            float maxRise = ARRIVE_ANY_RISE);
        /// Without flight in the air (a dismount, a cast that took the mount away): fall to the ground and take a
        /// player's fall damage.
        static void FallIfAirborne(Player* bot);
        /// The level's riding skill and mounts of the bot's side, as a player of that level has them.
        static void LearnRiding(Player* bot);
    };
}

#endif

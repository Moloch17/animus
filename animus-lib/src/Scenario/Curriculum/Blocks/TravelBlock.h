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
            OBS_COUNT                   = 16
        };

        enum Action : uint32
        {
            ACTION_MOUNT_GROUND         = 0,    // the fastest ground mount it has
            ACTION_MOUNT_FLYING         = 1,    // the fastest flying mount it has
            ACTION_DISMOUNT             = 2,
            ACTION_COUNT                = 3
        };

        static constexpr float ARRIVE_DISTANCE = 6.0f;
        static constexpr float BASE_RUN_SPEED = 7.0f;   // yards a second, unmounted and unhasted
        /// How high a seat may climb above the ground. MoveBlock's pitch reads it: the ceiling is a fact about the
        /// air, which is this block's subject, not about steering.
        static constexpr float MAX_ALTITUDE = 150.0f;

        [[nodiscard]] BlockId Id() const override { return BlockId::Travel; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
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
        [[nodiscard]] static bool AtObjective(Player const* bot, Position const& objective);
        /// Without flight in the air (a dismount, a cast that took the mount away): fall to the ground and take a
        /// player's fall damage.
        static void FallIfAirborne(Player* bot);
        /// The level's riding skill and mounts of the bot's side, as a player of that level has them.
        static void LearnRiding(Player* bot);
    };
}

#endif

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
    /// Getting somewhere as a player does: summon a ground or flying mount (SeatCharacter gives the level's riding
    /// and mounts), dismount, head for the objective, climb and descend in the air. Dismounting in the air falls, with
    /// a player's fall damage. The seat decides whether a trip is long enough to be worth the mount's cast time.
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
            ACTION_MOVE_TO_OBJECTIVE    = 3,    // on the ground by path; in the air straight, never lower than now
            ACTION_ASCEND               = 4,    // flying: CLIMB_STEP yards up
            ACTION_DESCEND              = 5,    // flying: CLIMB_STEP yards down, to the ground at most
            ACTION_COUNT                = 6
        };

        static constexpr float ARRIVE_DISTANCE = 6.0f;
        static constexpr float CLIMB_STEP = 15.0f;
        static constexpr float MAX_ALTITUDE = 150.0f;   // above the ground

        [[nodiscard]] BlockId Id() const override { return BlockId::Travel; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void BeforeApply(SeatView& view) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] bool IsMovement(uint32 local) const override
        {
            return local == ACTION_MOVE_TO_OBJECTIVE || local == ACTION_ASCEND || local == ACTION_DESCEND;
        }

        /// The fastest ground and flying mount spells `bot` knows (null when none).
        [[nodiscard]] static SpellInfo const* GroundMount(Player const* bot);
        [[nodiscard]] static SpellInfo const* FlyingMount(Player const* bot);
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

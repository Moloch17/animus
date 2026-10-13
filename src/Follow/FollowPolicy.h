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

#ifndef ANIMUS_FOLLOW_POLICY_H
#define ANIMUS_FOLLOW_POLICY_H

#include "Define.h"
#include "FreeLook.h"
#include <array>
#include <memory>
#include <string>

namespace Animus
{
    class MlpPolicy;
}

namespace Animus::Follow
{
    /// The goal slots a follower holds: the primary and the secondary (the forge's GOAL_SLOTS, StageState.h, which the
    /// module cannot include). The learner's wire carries four goal positions a seat (primary, secondary, its queue)
    /// and a cell word for each (EnvPool's GOAL_SLOTS_ON_WIRE); only the first two become holds
    /// (StageScenario::ApplyGoals), and move4_follow has no cell goals, so the policy hands back these two alone.
    constexpr uint32 GOAL_SLOTS = 2;

    /// One decision's inputs, as the forge's learner receives a seat's row: the observation and the action mask
    /// (SeatEncoder::Observe, then the paced mask), the camera's image bytes (Vision::ImageBytes of the configured
    /// camera, at the canonical size) and the mental map's crop bytes (Vision::CROP_BYTES).
    struct FollowPolicyInput
    {
        float const* Obs = nullptr;
        uint8 const* Mask = nullptr;
        uint8 const* Image = nullptr;
        uint8 const* MapCrop = nullptr;
        uint32 ObsDim = 0;
        uint32 NumActions = 0;
        uint32 ImageBytes = 0;
        uint32 MapBytes = 0;
        /// The first decision of an episode (the driver also calls Reset at every episode start).
        bool EpisodeStart = false;
    };

    /// What the policy chose: the action (0 = noop), the look head's choice per head (Vision::FreeLook::HEAD_SIZES;
    /// applied with FreeLook::Apply, which ignores an out-of-range choice), and the goal joints for the two slots
    /// (kind * GOAL_TARGETS + target; NO_GOAL = -1 or anything outside [0, GOAL_JOINT_COUNT) is none).
    struct FollowDecision
    {
        int32 Action = 0;
        std::array<int32, Vision::FreeLook::HEADS> Look{ Vision::FreeLook::NEUTRAL };
        std::array<int32, GOAL_SLOTS> Goals{ -1, -1 };
    };

    /// **A follower's model** (the .amdl format 10 forward pass: image and map crop encoders, the trunk and its
    /// recurrent memory, the action, look and goal heads, and the goal cadence). One instance per follower: it owns
    /// that follower's recurrent state and goal choice, which Reset clears at every episode start.
    class FollowPolicy
    {
    public:
        virtual ~FollowPolicy() = default;

        /// One decision; false when the model could not decide (the driver then holds a noop and logs it).
        virtual bool Decide(FollowPolicyInput const& input, FollowDecision& decision) = 0;
        /// A new episode: the recurrent memory and the goal state cleared.
        virtual void Reset() = 0;
    };

    /// The policy for `modelName` (`<class>_follow`, the layout's ModelName), from the module's model directory
    /// (sAnimusMod->ModelDir()); null when there is none it can run, which refuses the summon.
    [[nodiscard]] std::unique_ptr<FollowPolicy> MakeFollowPolicy(std::string const& modelName, MlpPolicy* model);
}

#endif

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

#ifndef ANIMUS_WARRIOR_DUMMY_POLICY_IO_H
#define ANIMUS_WARRIOR_DUMMY_POLICY_IO_H

#include "Define.h"

class Player;
class Unit;

/// Model inputs and outputs of the warrior_dummy policy.
///
/// KEEP IN SYNC with mod-animus-forge/src/Scenario/WarriorDummyScenario (Action and Obs enums,
/// Observe, ApplyActions, Reward). The model file records the scenario name and shapes it was
/// trained with, and MlpPolicy::Load refuses a mismatch; a change in meaning with the same shapes
/// is not caught.
namespace Animus::WarriorDummy
{
    inline constexpr char const* SCENARIO_NAME = "warrior_dummy";

    enum Action : int32
    {
        ACTION_NOOP                 = 0,
        ACTION_QUEUE_HEROIC_STRIKE  = 1,
        ACTION_CANCEL_QUEUED        = 2,
        ACTION_COUNT
    };

    enum Obs : uint32
    {
        OBS_RAGE                    = 0,    // rage / max rage
        OBS_SWING_REMAINING         = 1,    // main-hand swing timer remaining / weapon speed
        OBS_WEAPON_SPEED            = 2,    // weapon speed in seconds / 4
        OBS_HEROIC_STRIKE_QUEUED    = 3,
        OBS_AUTO_ATTACKING          = 4,
        OBS_IN_MELEE_FRONT          = 5,    // target in melee range and in the frontal arc
        OBS_HEROIC_STRIKE_COST      = 6,    // cost / max rage
        OBS_LAST_STEP_DAMAGE        = 7,    // damage since the last decision / damage scale
        OBS_LAST_STEP_RAGE_DELTA    = 8,    // rage change since the last decision / max rage
        OBS_COUNT
    };

    /// What happened since the previous decision; the scenario computes it in Reward.
    struct StepFeedback
    {
        float LastStepDamage = 0.0f;
        float LastStepRageDelta = 0.0f;
    };

    /// Divisor for OBS_LAST_STEP_DAMAGE: the main-hand weapon's max hit.
    [[nodiscard]] float DamageScale(Player const* bot);

    /// obs: [OBS_COUNT], mask: [ACTION_COUNT].
    void Observe(Player* bot, Unit const* target, StepFeedback const& feedback, float* obs, uint8* mask);

    /// Carry out a model action. Actions that are not currently allowed are ignored.
    void Apply(Player* bot, Unit* target, int32 action);
}

#endif

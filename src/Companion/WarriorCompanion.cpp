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

#include "WarriorCompanion.h"
#include "Log.h"
#include "MlpPolicy.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include <algorithm>

namespace
{
    /// A target further than this from the bot is dropped and the bot returns to its owner.
    constexpr float TARGET_LEASH_DISTANCE = 100.0f;
}

Animus::WarriorCompanion::WarriorCompanion(ObjectGuid owner, ObjectGuid bot, float damageScale)
    : _owner(owner), _bot(bot), _damageScale(damageScale)
{
}

Animus::WarriorCompanion::Status Animus::WarriorCompanion::Update(uint32 diff, uint32 decisionMs,
    MovementTree const& tree, MlpPolicy& policy)
{
    Player* owner = ObjectAccessor::FindPlayer(_owner);
    Player* bot = ObjectAccessor::FindPlayer(_bot);
    if (!owner || !bot || owner->GetMap() != bot->GetMap())
        return Status::Dismiss;

    if (!bot->IsAlive())
        return Status::Active;

    Unit* target = ResolveTarget(bot);

    MovementTree::Context const context{ bot, owner, target };
    MovementTree::Leaf const leaf = tree.Evaluate(context);
    if (leaf != _lastLeaf)
    {
        LOG_DEBUG("module.animus", "{}: {} -> {}", bot->GetName(), MovementTree::LeafName(_lastLeaf),
            MovementTree::LeafName(leaf));
        _lastLeaf = leaf;
    }

    MovementTree::Execute(leaf, context);

    if (!target)
    {
        ResetStep(bot);
        return Status::Active;
    }

    _sinceDecisionMs += diff;
    if (_sinceDecisionMs >= decisionMs)
    {
        _sinceDecisionMs %= decisionMs;
        Decide(bot, target, policy);
    }

    return Status::Active;
}

void Animus::WarriorCompanion::SetTarget(ObjectGuid target)
{
    _target = target;

    if (Player* bot = ObjectAccessor::FindPlayer(_bot))
        ResetStep(bot);
}

void Animus::WarriorCompanion::RecordDamage(Unit const* victim, uint32 damage)
{
    if (victim->GetGUID() == _target)
        _stepDamage.fetch_add(damage, std::memory_order_relaxed);
}

Unit* Animus::WarriorCompanion::ResolveTarget(Player* bot)
{
    if (_target.IsEmpty())
        return nullptr;

    Unit* target = ObjectAccessor::GetUnit(*bot, _target);
    if (target && target->IsInWorld() && target->IsAlive() && bot->IsWithinDistInMap(target, TARGET_LEASH_DISTANCE)
        && bot->IsValidAttackTarget(target))
        return target;

    LOG_DEBUG("module.animus", "{} dropped target {}", bot->GetName(), _target.ToString());
    _target.Clear();
    ResetStep(bot);
    return nullptr;
}

void Animus::WarriorCompanion::Decide(Player* bot, Unit* target, MlpPolicy& policy)
{
    // What the scenario's Reward computes: damage and rage change since the previous decision.
    uint32 const rage = bot->GetPower(POWER_RAGE);
    float const maxRage = float(std::max<uint32>(1, bot->GetMaxPower(POWER_RAGE)));
    _feedback.LastStepDamage = float(_stepDamage.exchange(0, std::memory_order_relaxed)) / _damageScale;
    _feedback.LastStepRageDelta = (float(rage) - float(_lastRage)) / maxRage;
    _lastRage = rage;

    float obs[WarriorDummy::OBS_COUNT];
    uint8 mask[WarriorDummy::ACTION_COUNT];
    WarriorDummy::Observe(bot, target, _feedback, obs, mask);

    int32 const action = policy.Decide(obs, mask);
    if (action != WarriorDummy::ACTION_NOOP)
        LOG_DEBUG("module.animus", "{} action {} | rage {:.3f} swing {:.3f} queued {} attacking {} melee {} "
            "dmg {:.3f} drage {:.3f}", bot->GetName(), action, obs[WarriorDummy::OBS_RAGE],
            obs[WarriorDummy::OBS_SWING_REMAINING], obs[WarriorDummy::OBS_HEROIC_STRIKE_QUEUED],
            obs[WarriorDummy::OBS_AUTO_ATTACKING], obs[WarriorDummy::OBS_IN_MELEE_FRONT],
            obs[WarriorDummy::OBS_LAST_STEP_DAMAGE], obs[WarriorDummy::OBS_LAST_STEP_RAGE_DELTA]);

    WarriorDummy::Apply(bot, target, action);
}

void Animus::WarriorCompanion::ResetStep(Player const* bot)
{
    _sinceDecisionMs = 0;
    _lastRage = bot->GetPower(POWER_RAGE);
    _stepDamage.store(0, std::memory_order_relaxed);
    _feedback = WarriorDummy::StepFeedback();
}

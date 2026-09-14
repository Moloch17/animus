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

#ifndef ANIMUS_WARRIOR_COMPANION_H
#define ANIMUS_WARRIOR_COMPANION_H

#include "Define.h"
#include "MovementTree.h"
#include "ObjectGuid.h"
#include "WarriorDummyPolicyIO.h"
#include <atomic>

class Player;
class Unit;

namespace Animus
{
    class MlpPolicy;

    /// A level 1 warrior bot owned by a player. The movement tree runs every world update; every
    /// DecisionMs of game time while it has a target, the model picks the combat action.
    ///
    /// Holds GUIDs only; objects are resolved each update.
    class WarriorCompanion
    {
    public:
        enum class Status : uint8
        {
            Active,
            Dismiss,    // owner or bot is gone or on another map; the caller removes the companion
        };

        WarriorCompanion(ObjectGuid owner, ObjectGuid bot, float damageScale);

        /// World thread, while no map is updating.
        Status Update(uint32 diff, uint32 decisionMs, MovementTree const& tree, MlpPolicy& policy);

        /// World thread. An empty GUID clears the target.
        void SetTarget(ObjectGuid target);

        /// Map thread of the bot's map (UnitScript::DealDamage). Counts damage the bot deals to its
        /// current target.
        void RecordDamage(Unit const* victim, uint32 damage);

        [[nodiscard]] ObjectGuid GetOwnerGUID() const { return _owner; }
        [[nodiscard]] ObjectGuid GetBotGUID() const { return _bot; }

    private:
        /// The target if it can still be fought, else nullptr (and the stored GUID is cleared).
        Unit* ResolveTarget(Player* bot);

        void Decide(Player* bot, Unit* target, MlpPolicy& policy);
        void ResetStep(Player const* bot);

        ObjectGuid _owner;
        ObjectGuid _bot;

        /// Written on the world thread, read by RecordDamage on the bot's map thread. Maps only update
        /// while the world thread waits in MapMgr::Update, so the two never overlap.
        ObjectGuid _target;

        float _damageScale;
        uint32 _sinceDecisionMs = 0;
        uint32 _lastRage = 0;
        std::atomic<uint32> _stepDamage{ 0 };
        MovementTree::Leaf _lastLeaf = MovementTree::Leaf::Hold;
        WarriorDummy::StepFeedback _feedback;
    };
}

#endif

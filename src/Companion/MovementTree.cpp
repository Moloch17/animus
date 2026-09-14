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

#include "MovementTree.h"
#include "MotionMaster.h"
#include "Player.h"

namespace
{
    constexpr float FOLLOW_DISTANCE = 2.0f;
    constexpr float FOLLOW_ANGLE = float(M_PI) / 2;

    /// The arc the player melee check requires the victim to be in (Player::Update).
    constexpr float MELEE_ARC = 2 * float(M_PI) / 3;

    bool HasTarget(Animus::MovementTree::Context const& context)
    {
        return context.Target != nullptr;
    }

    bool InMeleeRange(Animus::MovementTree::Context const& context)
    {
        return context.Bot->IsWithinMeleeRange(context.Target);
    }

    bool FacingTarget(Animus::MovementTree::Context const& context)
    {
        return context.Bot->HasInArc(MELEE_ARC, context.Target);
    }

    bool AutoAttackingTarget(Animus::MovementTree::Context const& context)
    {
        return context.Bot->GetVictim() == context.Target;
    }
}

Animus::MovementTree::MovementTree()
{
    uint8 const attacking = AddBranch(AutoAttackingTarget, AddLeaf(Leaf::Hold), AddLeaf(Leaf::StartAutoAttack));
    uint8 const facing = AddBranch(FacingTarget, attacking, AddLeaf(Leaf::FaceTarget));
    uint8 const inRange = AddBranch(InMeleeRange, facing, AddLeaf(Leaf::ChaseTarget));
    _root = AddBranch(HasTarget, inRange, AddLeaf(Leaf::FollowOwner));
}

uint8 Animus::MovementTree::AddLeaf(Leaf leaf)
{
    Node node;
    node.Result = leaf;
    _nodes.push_back(node);
    return uint8(_nodes.size() - 1);
}

uint8 Animus::MovementTree::AddBranch(Condition test, uint8 ifTrue, uint8 ifFalse)
{
    Node node;
    node.Test = test;
    node.IfTrue = ifTrue;
    node.IfFalse = ifFalse;
    _nodes.push_back(node);
    return uint8(_nodes.size() - 1);
}

Animus::MovementTree::Leaf Animus::MovementTree::Evaluate(Context const& context) const
{
    Node const* node = &_nodes[_root];
    while (node->Test)
        node = &_nodes[node->Test(context) ? node->IfTrue : node->IfFalse];

    return node->Result;
}

void Animus::MovementTree::Execute(Leaf leaf, Context const& context)
{
    Player* bot = context.Bot;
    MotionMaster* motion = bot->GetMotionMaster();

    switch (leaf)
    {
        case Leaf::FollowOwner:
            if (bot->GetVictim())
                bot->AttackStop();

            if (motion->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            {
                motion->Clear(false);
                motion->MoveFollow(context.Owner, FOLLOW_DISTANCE, FOLLOW_ANGLE);
            }
            break;
        case Leaf::ChaseTarget:
            if (motion->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
            {
                motion->Clear(false);
                motion->MoveChase(context.Target);
            }
            break;
        case Leaf::FaceTarget:
            // SetFacingToObject does nothing while a spline is running; the next update retries.
            if (bot->IsStopped())
            {
                bot->SetFacingToObject(context.Target);
                // The melee arc check reads the server-side orientation immediately.
                bot->SetOrientation(bot->GetAngle(context.Target));
            }
            break;
        case Leaf::StartAutoAttack:
            bot->Attack(context.Target, true);
            break;
        case Leaf::Hold:
            break;
    }
}

char const* Animus::MovementTree::LeafName(Leaf leaf)
{
    switch (leaf)
    {
        case Leaf::FollowOwner:     return "FollowOwner";
        case Leaf::ChaseTarget:     return "ChaseTarget";
        case Leaf::FaceTarget:      return "FaceTarget";
        case Leaf::StartAutoAttack: return "StartAutoAttack";
        case Leaf::Hold:            return "Hold";
    }

    return "?";
}

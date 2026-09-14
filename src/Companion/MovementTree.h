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

#ifndef ANIMUS_MOVEMENT_TREE_H
#define ANIMUS_MOVEMENT_TREE_H

#include "Define.h"
#include <vector>

class Player;
class Unit;

namespace Animus
{
    /// Binary decision tree for a companion's movement and engagement: follow the owner, close to
    /// melee, face the target, start auto-attack. It never chooses abilities; those come from the
    /// model.
    ///
    ///     has target?
    ///     ├─ no  → FollowOwner
    ///     └─ yes → in melee range?
    ///              ├─ no  → ChaseTarget
    ///              └─ yes → facing target?
    ///                       ├─ no  → FaceTarget
    ///                       └─ yes → auto-attacking target?
    ///                                ├─ no  → StartAutoAttack
    ///                                └─ yes → Hold
    class MovementTree
    {
    public:
        enum class Leaf : uint8
        {
            FollowOwner,
            ChaseTarget,
            FaceTarget,
            StartAutoAttack,
            Hold,
        };

        /// Target is null when the companion has none; the companion validates it beforehand.
        struct Context
        {
            Player* Bot = nullptr;
            Player* Owner = nullptr;
            Unit* Target = nullptr;
        };

        MovementTree();

        [[nodiscard]] Leaf Evaluate(Context const& context) const;

        /// Carry out a leaf. Idempotent: repeating it every update does not restart movement.
        static void Execute(Leaf leaf, Context const& context);

        [[nodiscard]] static char const* LeafName(Leaf leaf);

    private:
        using Condition = bool (*)(Context const&);

        struct Node
        {
            Condition Test = nullptr;   // null for a leaf
            uint8 IfTrue = 0;           // node indices
            uint8 IfFalse = 0;
            Leaf Result = Leaf::Hold;
        };

        uint8 AddLeaf(Leaf leaf);
        uint8 AddBranch(Condition test, uint8 ifTrue, uint8 ifFalse);

        std::vector<Node> _nodes;
        uint8 _root = 0;
    };
}

#endif

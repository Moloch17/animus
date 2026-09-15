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

#ifndef ANIMUS_CURRICULUM_TALENT_BUILDER_H
#define ANIMUS_CURRICULUM_TALENT_BUILDER_H

#include "Define.h"
#include <array>
#include <vector>

class Player;

namespace Animus::Curriculum
{
    /// A class's three talent trees and random builds over them.
    ///
    /// A build spends points one at a time, each on a uniformly chosen talent that can take a point
    /// right now (row requirement: 5 points per row in the tree; prerequisite talent at its required
    /// rank). The spec's tree gets points until it holds SPEC_TREE_POINTS (the capstone row) or the
    /// level's points run out; every remaining point goes to the other two trees the same way.
    class TalentBuilder
    {
    public:
        static constexpr uint32 SPEC_TREE_POINTS = 51;
        static constexpr uint32 TREE_COUNT = 3;

        struct Talent
        {
            uint32 TalentId = 0;
            uint8 Tab = 0;                          // tab page 0-2
            uint32 Row = 0;
            uint8 MaxRank = 0;
            std::array<uint32, 5> RankSpells{};
            int32 DependsOn = -1;                   // index into Talents()
            uint8 DependsOnRanks = 0;               // ranks the prerequisite needs
        };

        /// One learn step: LearnTalent(TalentId, Rank) with Rank 0-based.
        struct Step
        {
            uint32 Index = 0;                       // into Talents()
            uint8 Rank = 0;
        };

        struct Build
        {
            std::vector<uint8> Ranks;               // per talent
            std::vector<Step> Order;                // valid learning order
            std::array<uint32, TREE_COUNT> TreePoints{};
        };

        explicit TalentBuilder(uint8 playerClass);

        [[nodiscard]] std::vector<Talent> const& Talents() const { return _talents; }

        [[nodiscard]] Build Random(uint8 specTab, uint32 points) const;

        /// Learns the build on a bot with no talents. Returns the points left unspent (0 when the build
        /// and Player::LearnTalent agree).
        uint32 Apply(Player* bot, Build const& build) const;

    private:
        void Spend(Build& build, uint32 treeMask, uint32 points) const;

        std::vector<Talent> _talents;               // by tab, row, column
    };
}

#endif

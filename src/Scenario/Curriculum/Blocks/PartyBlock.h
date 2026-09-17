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

#ifndef ANIMUS_LIB_CURRICULUM_PARTY_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_PARTY_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// The other learned party members: PARTY_MEMBERS teammate slots, each with what it is doing and the goal it
    /// says it is pursuing, so a party can divide the work (the owner has the companion block). Actions:
    /// follow the tank, assist and guard each teammate, then each revive on each teammate. Heals, shields and buffs on
    /// them are core actions aimed by the support block's friend selection.
    class PartyBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_ALIVE                   = 0,    // living party players (bot and owner included) / 5
            OBS_LOWEST_HEALTH           = 1,    // the most hurt living ally's health (owner and teammates)
            OBS_HAS_TANK                = 2,    // a living tank other than the bot
            OBS_HAS_HEALER              = 3,    // a living healer other than the bot
            OBS_GLOBAL_COUNT            = 4

            // Then PARTY_MEMBERS teammate slots of MEMBER_FEATURES.
        };

        enum MemberFeature : uint32
        {
            MEMBER_PRESENT              = 0,
            MEMBER_ALIVE                = 1,
            MEMBER_HEALTH               = 2,
            MEMBER_MANA                 = 3,
            MEMBER_DISTANCE             = 4,    // yards / 40
            MEMBER_BEARING_SIN          = 5,
            MEMBER_BEARING_COS          = 6,
            MEMBER_IN_COMBAT            = 7,
            MEMBER_ROLE_FIRST           = 8,    // one-hot: damage, tank, healer
            MEMBER_CLASS_FIRST          = 11,   // one-hot over PLAYABLE_CLASSES
            MEMBER_ATTACKERS            = 21,   // enemies attacking it / PACK_SLOTS
            MEMBER_TARGET_FIRST         = 22,   // one-hot: which enemy slot it attacks
            MEMBER_NO_TARGET            = 26,
            MEMBER_SLOT_ON_FIRST        = 27,   // per enemy slot: attacking it
            MEMBER_GOAL_FIRST           = 31,   // one-hot over GOAL_COUNT: the goal it is pursuing (none: all 0)
            MEMBER_FEATURES             = 31 + GOAL_COUNT
        };

        enum Action : uint32
        {
            ACTION_FOLLOW_TANK          = 0,
            ACTION_ASSIST_FIRST         = 1,                        // + member
            ACTION_GUARD_FIRST          = 1 + PARTY_MEMBERS,        // + member
            ACTION_REVIVE_FIRST         = 1 + 2 * PARTY_MEMBERS     // + member * revives + revive
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Party; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] bool IsMovement(uint32 local) const override { return local == ACTION_FOLLOW_TANK; }
    };
}

#endif

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

#ifndef ANIMUS_LIB_CURRICULUM_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_BLOCK_H

#include "Define.h"
#include <array>
#include <boost/json/fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

/*
 * A layout block: one group of observation features and actions of a class/role policy (the class's spells, the
 * duel's movement, the pack's enemy slots, ...). A stage is an ordered list of blocks (see StageDefinition), and a
 * layout places each block's features and actions after the previous block's (see Layout).
 *
 * Blocks are stateless: everything they read comes from the world and the SeatView, so the same code encodes a seat
 * in training and a companion in play.
 */
namespace Animus::Curriculum
{
    struct Layout;
    struct SeatActionResult;
    struct SeatView;

    enum class BlockId : uint8
    {
        Core,           // the character, its spells, trinkets and talents
        Duel,           // movement, auto-attack, pets, stopping casts and forms, the opponent's position
        Pack,           // enemy slots, target selection, tactical spells
        Gauntlet,       // pull timing, food, drink, sustain spells
        Companion,      // the owner: follow, assist, guard, heal it
        Party,          // three teammates: follow the tank, assist, guard and heal them
        Pvp,            // the enemy player's class, role and state
        Context,        // the situation: allies, hostile players and creatures, PvP flag, map kind (no actions)
        Hostiles,       // per enemy slot: player or creature, class, healing, stealth, pet (no actions)
        Pet,            // the pet bar: abilities, stance, follow and stay (classes with a controllable pet)
        Travel,         // mounts, flying and an objective to get to
        Flag,           // a flag match: both flags, both bases, the score (no actions)
        Support,        // friends (self, owner, teammates) to heal, shield and buff, and the heals' rank tier
        Count
    };

    constexpr std::size_t BLOCK_COUNT = std::size_t(BlockId::Count);

    /// Kinds of standing choice a player makes and keeps (SeatMemory: a change of one kind holds for a while).
    enum class ModeGroup : uint8
    {
        None,
        Form,           // stances, forms, presences, Shadowform
        Aspect,         // hunter aspects
        Aura,           // paladin auras
        Seal,           // paladin seals
        Armor,          // mage and warlock armors, shaman shields
        PetStance,      // passive, defensive, aggressive
        RankTier,       // which rank a rankable spell is cast at (CoreBlock::ACTION_RANK_TIERS)
        Count
    };

    // Sizes several blocks and the scenario agree on.
    constexpr uint32 RAID_GROUPS = 8;       // a raid's groups
    constexpr uint32 GROUP_SEATS = 5;       // seats in a group: a party is one of them
    /// Learned agents per env: 1, an arena's 2, a party's 1-5, or a raid's groups of five.
    constexpr uint32 MAX_SEATS = RAID_GROUPS * GROUP_SEATS;
    constexpr uint32 GROUP_MEMBERS = GROUP_SEATS - 1;    // the seat's own group, itself aside
    /// Raiders outside the seat's group that it still has to act on: the raid's main tank, its most hurt member,
    /// and the nearest one. Empty in every stage below a raid, where the group is the whole of it.
    constexpr uint32 SPOTLIGHT_SLOTS = 3;
    /// Teammate slots a seat observes and acts on (PartyBlock). Bounded on purpose: a raider heals, assists and
    /// guards its own group and a few named others, never 39 people, and a slot is 36 features and three actions.
    constexpr uint32 PARTY_MEMBERS = GROUP_MEMBERS + SPOTLIGHT_SLOTS;
    constexpr uint32 PACK_SLOTS = 4;        // enemies observed
    constexpr uint32 STABLE_SLOTS = 4;      // a hunter's stabled beasts
    /// Friends a seat heals, shields and buffs (SupportBlock): itself, the owner, the teammate slots.
    constexpr uint32 FRIEND_SLOTS = 2 + PARTY_MEMBERS;
    constexpr uint32 FRIEND_SELF = 0;
    constexpr uint32 FRIEND_OWNER = 1;
    constexpr uint32 FRIEND_TEAMMATE_FIRST = 2;
    /// Rank tiers of a heal with ranks: the highest known, about two thirds up the known ranks, about a third up.
    constexpr uint32 RANK_TIERS = 3;

    /// What a seat is trying to do over the next few seconds. The learner's goal head picks one every
    /// mappo.goal_every_decisions and keeps it until the next choice (MappoConfig), and sends it with the actions;
    /// the sim scores whether the seat's decisions match it (StageScenario::GoalHeld), pays Goals.Match for the ones
    /// that do, reports how each goal was used, and shows a party its teammates' goals. A goal is a statement of
    /// intent, not an order: nothing is masked by it.
    enum class SeatGoal : uint8
    {
        Fight,          // damage the enemy it is fighting
        Control,        // hold the other enemies out of the fight
        Recover,        // heal, eat or drink itself back up
        Protect,        // keep the owner or a teammate alive
        Position,       // get to where its spec fights from
        Prepare,        // buffs, summons and stealth before the fight
        Count
    };

    constexpr uint32 GOAL_COUNT = uint32(SeatGoal::Count);
    constexpr int32 NO_GOAL = -1;

    [[nodiscard]] std::string_view GoalName(SeatGoal goal);
    /// The episode clock's scale: the longest arena's episode, so it rises through every episode instead of
    /// saturating. Elapsed time, not the fraction of an episode's own limit: a companion has no limit, and the
    /// critic already sees the fraction (StageScenario::STATE_EPISODE_TIME).
    constexpr float EPISODE_TIME_SCALE_MS = 300000.0f;

    [[nodiscard]] std::string_view BlockName(BlockId id);
    [[nodiscard]] std::optional<BlockId> FindBlock(std::string_view name);

    /// A block's place in a layout's observation row and action mask.
    struct BlockSlice
    {
        uint32 ObsFirst = 0;
        uint32 ObsCount = 0;
        uint32 ActionFirst = 0;
        uint32 ActionCount = 0;

        [[nodiscard]] bool ContainsAction(uint32 action) const
        {
            return action >= ActionFirst && action < ActionFirst + ActionCount;
        }
    };

    struct BlockSize
    {
        uint32 Obs = 0;
        uint32 Actions = 0;
    };

    class Block
    {
    public:
        virtual ~Block() = default;

        [[nodiscard]] virtual BlockId Id() const = 0;

        /// The features and actions the block adds to `layout` (profile, assets and ally heals are set).
        [[nodiscard]] virtual BlockSize Size(Layout const& layout) const = 0;

        /// Block-specific manifest entries (spell lists, slot counts), written inside the block's manifest object.
        virtual void DescribeManifest(Layout const& /*layout*/, boost::json::object& /*block*/) const { }

        /// Write the block's features and action mask for a living bot: `obs` and `mask` point at the block's slice.
        /// `mask` is null when no mask is wanted (an ended episode's final observation): skip the cast checks.
        virtual void Observe(SeatView const& view, float* obs, uint8* mask) const = 0;

        /// Before an action of a layout with this block is applied (whichever block the action belongs to).
        /// Every decision before the chosen action, whatever it is: where a durative action (SeatOption) acts. What
        /// it does is recorded in `result` as a press would be.
        virtual void BeforeApply(SeatView& /*view*/, SeatActionResult& /*result*/) const { }

        /// Apply the block's action `local` (0-based within the block) as the client would. Masked actions do nothing.
        virtual void Apply(SeatView& /*view*/, uint32 /*local*/, SeatActionResult& /*result*/) const { }

        /// Whether action `local` is a movement order, which is paced by CurriculumTuning::ActionTuning::MoveRepeatMs
        /// rather than RepeatMs: steering has to be re-issued more often than a spell or an order.
        [[nodiscard]] virtual bool IsMovement(uint32 /*local*/) const { return false; }

        /// For a movement order, which way it goes relative to the target: +1 in (move to it, behind it), -1 away
        /// (back off, break line of sight), 0 neither (to casting range, stop, follow).
        [[nodiscard]] virtual int8 MoveDirection(uint32 /*local*/) const { return 0; }

        /// The kind of standing choice action `local` makes, if any (ModeGroup).
        [[nodiscard]] virtual ModeGroup ModeGroupOf(Layout const& /*layout*/, uint32 /*local*/) const
        {
            return ModeGroup::None;
        }

        /// A readable name for action `local`, for the manifest and the evaluation's per-action counts; empty for
        /// the default, "<block>_<local>".
        [[nodiscard]] virtual std::string ActionName(Layout const& /*layout*/, uint32 /*local*/) const { return {}; }
    };

    /// The block implementation of `id`.
    [[nodiscard]] Block const& GetBlock(BlockId id);
}

#endif

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

#ifndef ANIMUS_LIB_CURRICULUM_DUEL_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_DUEL_BLOCK_H

#include "Block.h"
#include "Position.h"
#include <array>

class Player;
class Unit;

namespace Animus::Curriculum
{
    /// Fighting something that fights back: where the target is and what it does, the bot's movement, casting, form
    /// and pet, what it carries (potions, healthstones, bandages, a soulstone), death, and a hunter's stable. Actions:
    /// movement, auto-attack, pet attack, stop casting, cancel form, the consumables, resurrecting itself when dead,
    /// call a stabled beast.
    class DuelBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_DISTANCE                = 0,    // yards / 60
            OBS_BEARING_SIN             = 1,    // direction to the target relative to the bot's facing
            OBS_BEARING_COS             = 2,
            OBS_BEHIND_TARGET           = 3,    // the bot is in the target's back arc
            OBS_TARGET_FACING_BOT       = 4,
            OBS_TARGET_IN_COMBAT        = 5,
            OBS_TARGET_ATTACKS_BOT      = 6,
            OBS_TARGET_CASTING          = 7,
            OBS_BOT_MOVING              = 8,
            OBS_BOT_IN_COMBAT           = 9,
            OBS_BOT_STEALTHED           = 10,
            OBS_BOT_AUTO_ATTACKING      = 11,
            OBS_DAMAGE_TAKEN            = 12,   // since the last decision / bot max health
            OBS_PET_OUT                 = 13,
            OBS_PET_HEALTH              = 14,
            OBS_PET_ATTACKING           = 15,   // the pet's victim is the target
            OBS_COMBAT_TIME             = 16,   // time the bot has been in combat / 60 s; 0 out of combat
            OBS_CAST_PROGRESS           = 17,   // fraction of the current cast time done; 0 when not casting
            OBS_CAST_REMAINING          = 18,   // seconds left of the current cast / 3
            OBS_SHAPESHIFTED            = 19,   // in a form the bot can cancel
            OBS_HEALTH_POTIONS          = 20,   // carried / CONSUMABLE_COUNT
            OBS_MANA_POTIONS            = 21,
            OBS_HEALTHSTONES            = 22,   // carried (at most 1)
            OBS_BANDAGES                = 23,   // carried / CONSUMABLE_COUNT
            OBS_POTION_COOLDOWN         = 24,   // the shared potion cooldown left, as a fraction
            OBS_HEALTHSTONE_COOLDOWN    = 25,
            OBS_RECENTLY_BANDAGED       = 26,   // no bandage can be used yet
            OBS_SOULSTONE_ON_BOT        = 27,   // the bot will be able to resurrect itself if it dies
            OBS_DEAD                    = 28,
            OBS_SELF_RESURRECT          = 29,   // dead, and able to resurrect itself (Soulstone, Reincarnation)
            // The target hides (stealth, invisibility): every live target feature above is 0, these remember it.
            OBS_TARGET_HIDDEN           = 30,
            OBS_TARGET_UNSEEN_TIME      = 31,   // time since the bot last saw it / 20 s
            OBS_LAST_SEEN_DISTANCE      = 32,   // yards to where it was last seen / 60
            OBS_LAST_SEEN_BEARING_SIN   = 33,   // direction to that place relative to the bot's facing
            OBS_LAST_SEEN_BEARING_COS   = 34,
            OBS_TARGET_IN_LINE_OF_SIGHT = 35,   // nothing in the way: casts can reach it, and it can reach the bot
            // What the target is, which decides what works on it: a player reads the same off a nameplate and a
            // tooltip, and learns it after a first spell fails. Without it a Fear on a fear-immune undead or a fire
            // spell on a fire elemental just failed with nothing to say why.
            OBS_TARGET_TYPE_FIRST       = 36,   // one-hot over Encoding::OPPONENT_TYPES (7); a player is humanoid
            OBS_TARGET_MAX_HEALTH       = 43,   // its max health / the bot's / 4, clamped
            OBS_TARGET_DAMAGE_MODIFIER  = 44,   // its template's damage multiplier / 2 (1 for a player)
            OBS_TARGET_ARMOR            = 45,   // the share of the bot's physical hits its armor takes off (0-0.75)
            OBS_TARGET_RUN_SPEED        = 46,   // run speed rate / 2
            OBS_TARGET_LEVEL_DIFFERENCE = 47,   // (its level - the bot's) / 5, clamped to [-1, 1]
            OBS_TARGET_IMMUNE_SCHOOL_FIRST = 48, // immune to Encoding::OBSERVED_SCHOOLS (6)
            OBS_TARGET_IMMUNE_MECHANIC_FIRST = 54, // immune to Encoding::OBSERVED_MECHANICS (6)
            // The bot's own crowd control, with or without a target.
            OBS_BOT_STUNNED             = 60,
            OBS_BOT_FEARED              = 61,   // feared or confused
            OBS_BOT_ROOTED              = 62,
            OBS_BOT_SILENCED            = 63,
            OBS_BOT_SNARED              = 64,
            OBS_STABLE_FIRST            = 65,   // hunters: per stable slot STABLE_FEATURES
            OBS_COUNT_WITHOUT_STABLE    = 65
        };

        /// Per stabled beast: offered, family / 50, ferocity, tenacity, cunning.
        static constexpr uint32 STABLE_FEATURES = 5;

        enum Action : uint32
        {
            ACTION_MOVE_TO_TARGET       = 0,    // run to melee reach, on the side the bot is on; to where a hidden
                                                // target was last seen
            ACTION_MOVE_BEHIND          = 1,    // run to melee reach behind the target
            ACTION_MOVE_TO_RANGE        = 2,    // run to casting range (MOVE_TO_RANGE_DISTANCE)
            ACTION_BACK_OFF             = 3,    // run BACK_OFF_DISTANCE further away
            ACTION_STOP                 = 4,
            ACTION_START_ATTACK         = 5,    // start auto-attack on the target
            ACTION_PET_ATTACK           = 6,    // send pets and guardians at the target
            ACTION_STOP_CASTING         = 7,    // cancel the current cast or channel
            ACTION_CANCEL_FORM          = 8,    // leave the current shapeshift form, as right-clicking it does
            ACTION_HEALTH_POTION        = 9,    // drink a healing potion
            ACTION_MANA_POTION          = 10,
            ACTION_HEALTHSTONE          = 11,
            ACTION_BANDAGE              = 12,   // bandage itself (a channel, broken by damage)
            ACTION_SOULSTONE_SELF       = 13,   // warlocks: soulstone itself
            ACTION_SELF_RESURRECT       = 14,   // dead: use its Soulstone or Reincarnation (not in the PvP stages)
            ACTION_BREAK_LINE_OF_SIGHT  = 15,   // run to the nearest place the target cannot see (a pillar, a hill)
            /// A ranged spec: back to casting range whenever the target closes in, decision after decision, until
            /// Options.KeepRangeMs runs out or the policy does something else (SeatOption).
            ACTION_KEEP_RANGE           = 16,
            /// A melee spec: back into melee reach whenever the target leaves it, decision after decision, until
            /// Options.StayOnTargetMs runs out or the seat moves itself (SeatOption). One press instead of the
            /// order re-issued every decision a fight leaves spare.
            ACTION_STAY_ON_TARGET       = 17,
            ACTION_CALL_BEAST_FIRST     = 18,   // hunters: call stable slot 0..STABLE_SLOTS-1
            ACTION_COUNT_WITHOUT_STABLE = 18
        };

        static constexpr float MOVE_TO_RANGE_DISTANCE = 24.0f;
        static constexpr float BACK_OFF_DISTANCE = 10.0f;

        /// Cover: where the bot looks for a place out of its target's sight.
        static constexpr std::array<float, 3> COVER_DISTANCES = { 8.0f, 16.0f, 26.0f };
        static constexpr uint32 COVER_BEARINGS = 12;

        /// The nearest place around the bot, on walkable ground, its target cannot see; false if there is none.
        [[nodiscard]] static bool FindCover(Player* bot, Unit* target, Position& cover);

        [[nodiscard]] BlockId Id() const override { return BlockId::Duel; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void BeforeApply(SeatView& view, SeatActionResult& result) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;
        [[nodiscard]] bool IsMovement(uint32 local) const override
        {
            return local <= ACTION_STOP || local == ACTION_BREAK_LINE_OF_SIGHT || local == ACTION_KEEP_RANGE
                || local == ACTION_STAY_ON_TARGET;
        }

        [[nodiscard]] int8 MoveDirection(uint32 local) const override
        {
            return local == ACTION_MOVE_TO_TARGET || local == ACTION_MOVE_BEHIND ? 1
                : local == ACTION_BACK_OFF || local == ACTION_BREAK_LINE_OF_SIGHT ? -1 : 0;
        }

        [[nodiscard]] ModeGroup ModeGroupOf(Layout const& /*layout*/, uint32 local) const override
        {
            return local == ACTION_CANCEL_FORM ? ModeGroup::Form : ModeGroup::None;
        }

        /// A dead bot's features and mask (every other block stays empty): dead, and whether it can resurrect itself.
        static void ObserveDead(SeatView const& view, float* obs, uint8* mask);
    };
}

#endif

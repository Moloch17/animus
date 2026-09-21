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

#ifndef ANIMUS_LIB_CURRICULUM_CORE_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_CORE_BLOCK_H

#include "Block.h"

namespace Animus::Curriculum
{
    /// The character and its class: level, race, spec, resources, swing timers and stats; per catalog action (spell or
    /// trinket) whether it is known, its cooldown and its auras; the talent build. Actions: the catalog.
    class CoreBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_LEVEL                   = 0,    // level / 80
            OBS_RACE_FIRST              = 1,    // one-hot over PLAYABLE_RACES
            /// Which role the seat is playing this episode, one-hot over Role. One model per class has to be told,
            /// because the role is not a fact about the character but the contract it is graded under: a paladin
            /// that cannot see whether it is holding the line or healing it has no way to choose between them.
            ///
            /// There is deliberately no spec here. The spec is a name for a talent build, and the build itself is
            /// already observed further down -- every talent's rank and every tree's points -- so a label would be
            /// a redundant shortcut that invites the policy to play the name instead of the talents. It would also
            /// sometimes be false: TalentPlan::Noisy and ::Random hand the seat a build its nominal spec did not
            /// choose, and the label would keep insisting on the spec. What the seat can do is what it has.
            OBS_ROLE_FIRST              = 11,
            OBS_HEALTH                  = 14,
            OBS_MANA                    = 15,   // fraction of max; 0 without mana
            OBS_RAGE                    = 16,   // / 100
            OBS_ENERGY                  = 17,   // fraction of max
            OBS_RUNIC_POWER             = 18,   // / 100
            OBS_RUNE_FIRST              = 19,   // 6 runes: 1 ready, else 1 - cooldown / 10 s
            OBS_COMBO_POINTS            = 25,   // on the target, / 5
            OBS_FORM_FIRST              = 26,   // one-hot over the tracked forms (13)
            OBS_GCD                     = 39,   // remaining / 1.5 s
            OBS_CASTING                 = 40,   // casting or channeling
            OBS_QUEUED_NEXT_SWING       = 41,
            OBS_MAIN_HAND_SWING         = 42,   // swing timer remaining / weapon speed
            OBS_OFF_HAND_SWING          = 43,
            OBS_RANGED_SWING            = 44,
            OBS_MAIN_HAND_SPEED         = 45,   // seconds / 4
            OBS_TARGET_HEALTH           = 46,
            OBS_TARGET_DISTANCE         = 47,   // yards / 40
            OBS_IN_MELEE_FRONT          = 48,
            OBS_ATTACK_POWER            = 49,   // / (100 + 50 * level)
            OBS_SPELL_POWER             = 50,   // / (50 + 30 * level)
            OBS_MELEE_CRIT              = 51,   // percent / 100
            OBS_SPELL_CRIT              = 52,
            OBS_MELEE_HASTE             = 53,   // rating bonus percent / 100
            OBS_SPELL_HASTE             = 54,
            OBS_MELEE_HIT               = 55,
            OBS_SPELL_HIT               = 56,
            OBS_EXPERTISE               = 57,   // / 30
            OBS_ARMOR_PENETRATION       = 58,   // rating bonus percent / 100
            OBS_LAST_STEP_DAMAGE        = 59,   // damage since the last decision / damage scale
            OBS_LAST_STEP_POWER_DELTA   = 60,   // primary power change since the last decision, as a fraction
            /// Time into the episode / 5 min, clamped. Without it a bot that stands still out of combat sees the
            /// same rows over and over, and a deterministic policy cycles through the same decisions for good:
            /// stage1_duel evaluation had warlocks start and stop one cast 299 times, 0 damage, on six seeds.
            OBS_EPISODE_TIME            = 61,
            /// What the seat has been doing (SeatMemory): one observation says nothing of it, so a policy re-decided
            /// from scratch every decision, running in and backing off by turns and dancing between stances.
            OBS_SINCE_MOVE              = 62,   // time since its last movement order / 5 s; 1 = none yet
            OBS_LAST_MOVE_DIRECTION     = 63,   // +1 in toward the target, -1 away, 0 neither
            OBS_SINCE_MODE_CHANGE       = 64,   // time since its last stance, form, aspect, aura, seal, armor or pet
                                                // stance change / 10 s; 1 = none yet
            OBS_HEALTH_TREND            = 65,   // its health now - its average over the last few seconds
            OBS_TARGET_HEALTH_TREND     = 66,   // the same for its target
            /// Per durative action (SeatOptionKind without None): how much of its clock is left / 30 s, 0 when it is
            /// not running. Without them a running option is hidden state: the policy could not tell that it is
            /// already resting, holding an interrupt or keeping range -- and the seat runs two at once (a
            /// positioning option and a standby), so one slot with one clock could not say which.
            OBS_OPTION_FIRST            = 67,
            OBS_GLOBAL_COUNT            = 71

            // Then, per catalog action: ACTION_FEATURES features (known, cooldown, aura on target, aura on self,
            // stacks, time since the seat pressed it / 10 s). Then per talent of the class: rank / max rank. Then
            // per tree: points / 71.
        };

        /// After the catalog's actions: which rank of a rankable spell to cast (RANK_TIERS: the highest known, about
        /// two thirds up, about a third up). It lived in the support block, which only the stages from the gauntlet
        /// on have -- so in the duel a seat could only ever cast the biggest heal it knew, at any deficit, and
        /// overhealing was not a habit it could break. Down-ranking is a property of casting, so it belongs here.
        static constexpr uint32 ACTION_RANK_TIERS = RANK_TIERS;

        /// The first two catalog actions are the no-op and cancel-queued.
        static constexpr uint32 FIRST_CAST_ACTION = 2;
        static constexpr uint32 ACTION_FEATURES = 6;

        void BeforeApply(SeatView& view, SeatActionResult& result) const override;

        [[nodiscard]] BlockId Id() const override { return BlockId::Core; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void DescribeManifest(Layout const& layout, boost::json::object& block) const override;
        [[nodiscard]] std::string ActionName(Layout const& layout, uint32 local) const override;
        [[nodiscard]] ModeGroup ModeGroupOf(Layout const& layout, uint32 local) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;

        /// Whether the seat knows a spell that interrupts a cast (Kick, Counterspell, Mind Freeze, Shield Bash),
        /// whatever its cooldown: what makes holding an interrupt worth offering at all. Cooldowns are not counted --
        /// the hold runs for seconds and the mask would flicker under it.
        [[nodiscard]] static bool KnowsInterrupt(SeatView const& view);

        /// The character as built (level, race, spec, talent build): written whether the bot is alive or not.
        static void ObserveCharacter(SeatView const& view, float* obs);

        [[nodiscard]] static uint32 TalentObsFirst(Layout const& layout);
        [[nodiscard]] static uint32 TreeObsFirst(Layout const& layout);
    };
}

#endif

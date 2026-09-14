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

#ifndef ANIMUS_CLASS_ROLE_LAYOUT_H
#define ANIMUS_CLASS_ROLE_LAYOUT_H

#include "ActionCatalog.h"
#include "ClassRoleAssets.h"
#include "ClassRoleProfile.h"
#include <string>
#include <vector>

/*
 * What a class/role policy sees and does at a curriculum stage: the observation features and actions, their
 * positions, and the stage blocks each stage appends. mod-animus-forge trains with these layouts and mod-animus
 * plays with them; both build them here, so a model's inputs and outputs mean the same thing in training and in
 * play. A layout's manifest (Manifest) records everything its meaning depends on; a model carries the manifest it
 * was trained with, and a server whose own manifest differs must not use the model.
 */
namespace Animus::ClassRole
{
    /// A curriculum stage. Each stage is its own scenario (`class_role`, `class_role_duel`, ...), so every earlier
    /// stage stays repeatable, and each stage's observations and actions extend the previous stage's per layout,
    /// so a stage's model seeds the next.
    enum class Stage : uint8
    {
        /// Stage 1 (`class_role`): a training dummy in range that takes no damage; maximise damage.
        Dummy,
        /// Stage 2 (`class_role_duel`): a same-level hostile creature spawned out of aggro range that fights back;
        /// move to it and kill it quickly while taking little damage.
        Duel,
        /// Stage 3 (`class_role_pack`): a pack of 2-4 same-level creatures (casters included), often linked; pick
        /// targets, interrupt, crowd-control and clear it fast with little damage taken.
        Pack,
        /// Stage 4 (`class_role_gauntlet`): pull after pull (1-4 creatures, sometimes elite or higher level) with a
        /// short break between, until death or the episode ends; recover with heals, food and drink.
        Gauntlet,
        /// Stage 5 (`class_role_companion`): the gauntlet beside a scripted owner (a player of a random class near
        /// the bot's level); follow, assist, guard and heal it.
        Companion,
        /// Stage 6 (`class_role_party`): four learned agents -- a tank, a healer and two damage dealers of random
        /// classes -- and the scripted owner as the fifth player, against elite-heavy pulls. Every agent sees and
        /// can assist, guard and heal the other three.
        Party,
        /// Stage 7 (`class_role_pvp`): one-on-one against a scripted enemy player of a random class and role.
        Pvp,
        /// Stage 8 (`class_role_arena`): self-play one-on-one: two learned agents of random classes and roles, in
        /// one env, played by the same policy.
        Arena,
    };

    /// Whether `stage` includes `block`'s features and actions (every stage keeps the previous stages' blocks).
    [[nodiscard]] inline bool HasBlock(Stage stage, Stage block) { return stage >= block; }

    /// Stage scenario names: class_role, class_role_duel, ...; and the suffix a stage adds to model names.
    [[nodiscard]] char const* StageScenarioName(Stage stage);
    [[nodiscard]] char const* StageSuffix(Stage stage);

    /// Feature and action positions, relative to their stage block.
    struct LayoutConstants
    {
        enum Obs : uint32
        {
            OBS_LEVEL                   = 0,    // level / 80
            OBS_RACE_FIRST              = 1,    // one-hot over PLAYABLE_RACES
            OBS_SPEC_FIRST              = 11,   // one-hot over the role's specs (3 slots)
            OBS_HEALTH                  = 14,
            OBS_MANA                    = 15,   // fraction of max; 0 without mana
            OBS_RAGE                    = 16,   // / 100
            OBS_ENERGY                  = 17,   // fraction of max
            OBS_RUNIC_POWER             = 18,   // / 100
            OBS_RUNE_FIRST              = 19,   // 6 runes: 1 ready, else 1 - cooldown / 10 s
            OBS_COMBO_POINTS            = 25,   // on the dummy, / 5
            OBS_FORM_FIRST              = 26,   // one-hot over TRACKED_FORMS
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
            OBS_GLOBAL_COUNT            = 61

            // Then, per action: ACTION_FEATURES features (known, cooldown, aura on target, aura on self,
            // stacks). Then per talent of the class: rank / max rank. Then per tree: points / 71.
        };

        static constexpr uint32 ACTION_FEATURES = 5;
        static constexpr uint32 MAX_SPECS = 3;

        /// Duel observation features, after all of stage 1's.
        enum DuelObs : uint32
        {
            DUEL_OBS_DISTANCE           = 0,    // yards / 60
            DUEL_OBS_BEARING_SIN        = 1,    // direction to the opponent relative to the bot's facing
            DUEL_OBS_BEARING_COS        = 2,
            DUEL_OBS_BEHIND_TARGET      = 3,    // the bot is in the opponent's back arc
            DUEL_OBS_TARGET_FACING_BOT  = 4,
            DUEL_OBS_TARGET_IN_COMBAT   = 5,
            DUEL_OBS_TARGET_ATTACKS_BOT = 6,
            DUEL_OBS_TARGET_CASTING     = 7,
            DUEL_OBS_BOT_MOVING         = 8,
            DUEL_OBS_BOT_IN_COMBAT      = 9,
            DUEL_OBS_BOT_STEALTHED      = 10,
            DUEL_OBS_BOT_AUTO_ATTACKING = 11,
            DUEL_OBS_DAMAGE_TAKEN       = 12,   // since the last decision / bot max health
            DUEL_OBS_PET_OUT            = 13,
            DUEL_OBS_PET_HEALTH         = 14,
            DUEL_OBS_PET_ATTACKING      = 15,   // the pet's victim is the opponent
            DUEL_OBS_EPISODE_TIME       = 16,   // elapsed / episode length
            DUEL_OBS_CAST_PROGRESS      = 17,   // fraction of the current cast time done; 0 when not casting
            DUEL_OBS_CAST_REMAINING     = 18,   // seconds left of the current cast / 3
            DUEL_OBS_SHAPESHIFTED       = 19,   // in a form the bot can cancel
            DUEL_OBS_STABLE_FIRST       = 20,   // hunters' stable: per slot STABLE_FEATURES
            DUEL_OBS_COUNT_WITHOUT_STABLE = 20
        };

        /// Hunters' stable: beasts per episode, and features per beast (offered, family / 50,
        /// ferocity, tenacity, cunning).
        static constexpr uint32 STABLE_SLOTS = 4;
        static constexpr uint32 STABLE_FEATURES = 5;

        /// Duel actions, after all of stage 1's.
        enum DuelAction : uint32
        {
            DUEL_ACTION_MOVE_TO_TARGET  = 0,    // run to melee reach, on the side the bot is on
            DUEL_ACTION_MOVE_BEHIND     = 1,    // run to melee reach behind the opponent
            DUEL_ACTION_MOVE_TO_RANGE   = 2,    // run to casting range (25 yd)
            DUEL_ACTION_BACK_OFF        = 3,    // run 10 yd further away
            DUEL_ACTION_STOP            = 4,
            DUEL_ACTION_START_ATTACK    = 5,    // start auto-attack on the opponent
            DUEL_ACTION_PET_ATTACK      = 6,    // send pets and guardians at the opponent
            DUEL_ACTION_STOP_CASTING    = 7,    // cancel the current cast or channel
            DUEL_ACTION_CANCEL_FORM     = 8,    // leave the current shapeshift form, as right-clicking it does
            DUEL_ACTION_CALL_BEAST_FIRST = 9,   // hunters: call stable slot 0..STABLE_SLOTS-1
            DUEL_ACTION_COUNT_WITHOUT_STABLE = 9
        };

        /// Pack enemies observed, and features per enemy slot.
        static constexpr uint32 PACK_SLOTS = 4;

        enum PackSlotFeature : uint32
        {
            SLOT_PRESENT                = 0,
            SLOT_ALIVE                  = 1,
            SLOT_HEALTH                 = 2,
            SLOT_DISTANCE               = 3,    // yards / 60
            SLOT_BEARING_SIN            = 4,
            SLOT_BEARING_COS            = 5,
            SLOT_BEHIND                 = 6,    // the bot is in its back arc
            SLOT_ATTACKS_BOT            = 7,
            SLOT_ATTACKS_PET            = 8,
            SLOT_CASTING                = 9,
            SLOT_IN_COMBAT              = 10,
            SLOT_CROWD_CONTROLLED       = 11,   // stunned, feared, confused, rooted, silenced or polymorphed
            SLOT_CURRENT_TARGET         = 12,
            SLOT_ELITE                  = 13,
            SLOT_LEVEL_DIFFERENCE       = 14,   // (its level - the bot's) / 5
            SLOT_FEATURES
        };

        /// Pack observation features, after all of stage 2's: PACK_OBS_GLOBAL_COUNT globals, the enemy slots,
        /// then per tactical action: known, cooldown.
        enum PackObs : uint32
        {
            PACK_OBS_ALIVE              = 0,    // living enemies / PACK_SLOTS
            PACK_OBS_IN_COMBAT          = 1,    // enemies in combat / PACK_SLOTS
            PACK_OBS_GLOBAL_COUNT       = 2
        };

        /// Pack actions, after all of stage 2's: target slot 0..PACK_SLOTS-1, then the tactical spells.
        static constexpr uint32 PACK_ACTION_TARGET_FIRST = 0;

        /// Gauntlet observation features, after all of stage 3's, then per sustain action: known, cooldown.
        enum GauntletObs : uint32
        {
            GAUNTLET_OBS_PULLS_CLEARED  = 0,    // / 10
            GAUNTLET_OBS_PULL_ACTIVE    = 1,
            GAUNTLET_OBS_NEXT_PULL      = 2,    // time until the next pull / 20 s
            GAUNTLET_OBS_PULL_TIME      = 3,    // time into the current pull / 60 s
            GAUNTLET_OBS_ELITE_PULL     = 4,
            GAUNTLET_OBS_EATING         = 5,
            GAUNTLET_OBS_DRINKING       = 6,
            GAUNTLET_OBS_FOOD_LEFT      = 7,    // / CONSUMABLE_COUNT
            GAUNTLET_OBS_DRINK_LEFT     = 8,
            GAUNTLET_OBS_GLOBAL_COUNT   = 9
        };

        /// Gauntlet actions, after all of stage 3's: eat, drink, then the sustain spells.
        enum GauntletAction : uint32
        {
            GAUNTLET_ACTION_EAT         = 0,
            GAUNTLET_ACTION_DRINK       = 1,
            GAUNTLET_ACTION_SUSTAIN_FIRST = 2
        };

        static constexpr uint32 CONSUMABLE_COUNT = 5;

        /// Companion observation features, after all of stage 4's, then per owner-heal action: known,
        /// cooldown.
        enum CompanionObs : uint32
        {
            COMPANION_OBS_OWNER_PRESENT     = 0,
            COMPANION_OBS_OWNER_ALIVE       = 1,
            COMPANION_OBS_OWNER_HEALTH      = 2,
            COMPANION_OBS_OWNER_MANA        = 3,    // 0 without mana
            COMPANION_OBS_OWNER_DISTANCE    = 4,    // yards / 40
            COMPANION_OBS_OWNER_BEARING_SIN = 5,
            COMPANION_OBS_OWNER_BEARING_COS = 6,
            COMPANION_OBS_OWNER_IN_COMBAT   = 7,
            COMPANION_OBS_OWNER_MOVING      = 8,
            COMPANION_OBS_OWNER_LEVEL_DIFF  = 9,    // (owner level - bot level) / 5
            COMPANION_OBS_OWNER_CLASS_FIRST = 10,   // one-hot over OWNER_CLASSES (10)
            COMPANION_OBS_OWNER_ATTACKERS   = 20,   // enemies attacking the owner / PACK_SLOTS
            COMPANION_OBS_OWNER_TARGET_FIRST = 21,  // one-hot: which enemy slot the owner attacks
            COMPANION_OBS_OWNER_NO_TARGET   = 25,
            COMPANION_OBS_SLOT_ON_OWNER_FIRST = 26, // per enemy slot: attacking the owner
            COMPANION_OBS_GLOBAL_COUNT      = 30
        };

        /// Companion actions, after all of stage 4's: follow, assist, guard, then one "cast on the owner"
        /// action per single-target heal.
        enum CompanionAction : uint32
        {
            COMPANION_ACTION_FOLLOW         = 0,    // run to just behind the owner
            COMPANION_ACTION_ASSIST         = 1,    // target the owner's target
            COMPANION_ACTION_GUARD          = 2,    // target an enemy attacking the owner
            COMPANION_ACTION_HEAL_FIRST     = 3
        };

        /// A party seat's teammates: the other three seats (the owner has the companion block).
        static constexpr uint32 PARTY_MEMBERS = 3;

        /// Party features per member slot.
        enum PartyMemberFeature : uint32
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
            MEMBER_CLASS_FIRST          = 11,   // one-hot over the 10 classes
            MEMBER_ATTACKERS            = 21,   // enemies attacking it / PACK_SLOTS
            MEMBER_TARGET_FIRST         = 22,   // one-hot: which enemy slot it attacks
            MEMBER_NO_TARGET            = 26,
            MEMBER_SLOT_ON_FIRST        = 27,   // per enemy slot: attacking it
            MEMBER_FEATURES             = 31
        };

        /// Party observation features, after all of stage 5's: globals, then PARTY_MEMBERS teammate slots.
        enum PartyObs : uint32
        {
            PARTY_OBS_ALIVE             = 0,    // living party players (bot and owner included) / 5
            PARTY_OBS_LOWEST_HEALTH     = 1,    // the most hurt living ally's health (owner and teammates)
            PARTY_OBS_HAS_TANK          = 2,    // a living tank other than the bot
            PARTY_OBS_HAS_HEALER        = 3,    // a living healer other than the bot
            PARTY_OBS_GLOBAL_COUNT      = 4
        };

        /// Party actions, after all of stage 5's: follow the tank, then per teammate assist and guard, then per
        /// teammate one "cast on it" action per ally heal.
        enum PartyAction : uint32
        {
            PARTY_ACTION_FOLLOW_TANK    = 0,
            PARTY_ACTION_ASSIST_FIRST   = 1,    // + member
            PARTY_ACTION_GUARD_FIRST    = 1 + PARTY_MEMBERS,
            PARTY_ACTION_HEAL_FIRST     = 1 + 2 * PARTY_MEMBERS  // + teammate * ally heals + heal
        };

        /// PvP observation features (stages 7 and 8), after all of stage 6's.
        enum PvpObs : uint32
        {
            PVP_OBS_OPPONENT_CLASS_FIRST = 0,   // one-hot over the 10 classes
            PVP_OBS_OPPONENT_ROLE_FIRST = 10,   // one-hot: damage, tank, healer
            PVP_OBS_OPPONENT_LEVEL_DIFF = 13,   // (its level - the bot's) / 5
            PVP_OBS_OPPONENT_MANA       = 14,
            PVP_OBS_OPPONENT_RAGE_ENERGY = 15,  // rage, energy or runic power as a fraction
            PVP_OBS_OPPONENT_CONTROLLED = 16,   // stunned, feared, confused, rooted, silenced or polymorphed
            PVP_OBS_OPPONENT_STEALTHED  = 17,
            PVP_OBS_OPPONENT_PET_OUT    = 18,
            PVP_OBS_OPPONENT_HEALING    = 19,   // casting a heal
            PVP_OBS_BOT_STUNNED         = 20,   // stunned, feared or confused: no actions land
            PVP_OBS_BOT_ROOTED          = 21,
            PVP_OBS_BOT_SILENCED        = 22,
            PVP_OBS_MIRROR              = 23,   // stage 8: the opponent is a learned agent too
            PVP_OBS_COUNT               = 24
        };
    };

    /// One class/role's observation and action layout at one stage: where each stage block starts.
    struct Layout : LayoutConstants
    {
        uint16 Index = 0;                           // position among the layouts of a run (the learner's id)
        Stage StageId = Stage::Dummy;
        ClassRoleProfile const* Profile = nullptr;
        ClassRoleAssets const* Assets = nullptr;
        uint32 ObsDim = 0;
        uint32 NumActions = 0;
        uint32 ActionObsFirst = OBS_GLOBAL_COUNT;
        uint32 TalentObsFirst = 0;
        uint32 TreeObsFirst = 0;
        uint32 DuelObsFirst = 0;
        uint32 DuelActionFirst = 0;
        uint32 DuelActionCount = 0;
        uint32 PackObsFirst = 0;
        uint32 PackActionFirst = 0;
        uint32 PackActionCount = 0;
        uint32 GauntletObsFirst = 0;
        uint32 GauntletActionFirst = 0;
        uint32 GauntletActionCount = 0;
        uint32 CompanionObsFirst = 0;
        uint32 CompanionActionFirst = 0;
        uint32 CompanionActionCount = 0;
        uint32 PartyObsFirst = 0;
        uint32 PartyActionFirst = 0;
        uint32 PartyActionCount = 0;
        uint32 PvpObsFirst = 0;
        std::vector<ActionCatalog::Action> AllyHeals;   // single-target heals that can be cast on an ally

        /// The layout of `profile` at `stage` (Index 0). Builds the profile's assets on first use.
        [[nodiscard]] static Layout Build(ClassRoleProfile const& profile, Stage stage);

        [[nodiscard]] ActionCatalog const& Catalog() const { return *Assets->Catalog; }
        [[nodiscard]] Role PlayRole() const { return Profile->PlayRole; }
        [[nodiscard]] bool Has(Stage block) const { return HasBlock(StageId, block); }

        /// The model name of this layout: <class>_<role> plus the stage suffix (warrior_dps_party).
        [[nodiscard]] std::string ModelName() const;

        /// Everything the layout's meaning depends on, as JSON: stage, class/role, dimensions, block offsets, every
        /// action (kind, first rank, trinket slot), the tactical, sustain and ally-heal spells, and every talent.
        /// Two servers with the same manifest for a model feed it and read it the same way.
        [[nodiscard]] std::string Manifest() const;
    };
}

#endif

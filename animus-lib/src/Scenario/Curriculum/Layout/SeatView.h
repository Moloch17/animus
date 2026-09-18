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

#ifndef ANIMUS_LIB_CURRICULUM_SEAT_VIEW_H
#define ANIMUS_LIB_CURRICULUM_SEAT_VIEW_H

#include "Block.h"
#include "ClassRoleProfile.h"
#include "CurriculumTuning.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "Supplies.h"
#include "TalentBuilder.h"
#include <array>
#include <optional>
#include <vector>

class Player;
class SpellInfo;
class Unit;

namespace Animus::Curriculum
{
    struct Layout;
    class SeatMemory;

    /// One bot's situation at a decision: what the blocks cannot read from the world themselves. The scenario fills
    /// it; each part is only used by the blocks that need it.
    /// What a durative action ("option") the seat started is doing. One press stands for many decisions -- resting
    /// until it is ready to fight, holding an interrupt for the target's next cast, keeping a caster's distance --
    /// which is how a plan longer than a decision is expressed at all: 1800 decisions of a 450 s episode are far more
    /// than credit reaches back over. The block that owns the action starts it, the block that can act runs it every
    /// decision until its own stop condition or UntilMs, and any other action the policy takes cancels it.
    enum class SeatOptionKind : uint8
    {
        None = 0,
        RestUntilReady,     // eat and drink between pulls until health and mana are back
        HoldInterrupt,      // interrupt the target as soon as it casts
        KeepRange,          // a ranged spec: back to its range whenever the target closes in
        StayOnTarget,       // a melee spec: back into melee reach whenever the target leaves it
        Count
    };

    /// Positioning options (KeepRange, StayOnTarget) hold the seat where its spec fights from. Only the seat moving
    /// itself takes over from one: a fight is spells and swings between steps, and cancelling on those left a melee
    /// seat re-issuing its own movement every decision (stage1_duel 2026-09-17: the rogue pressed one every 0.39 s
    /// while it stood in melee reach 96% of the time).
    [[nodiscard]] constexpr bool IsPositioning(SeatOptionKind kind)
    {
        return kind == SeatOptionKind::KeepRange || kind == SeatOptionKind::StayOnTarget;
    }

    /// Holding an interrupt is a standby, not something the seat does: it waits for the target to cast while the seat
    /// keeps fighting, so every other action leaves it running. Cancelling it on any press left it lasting 0.6 s
    /// against casts of 1.5-2.5 s (stage2_pack 2026-09-18: the warlock pressed it 7.8 times a fight and interrupted
    /// 0.01 casts, the druid 10.4 times for none).
    [[nodiscard]] constexpr bool IsStandby(SeatOptionKind kind)
    {
        return kind == SeatOptionKind::HoldInterrupt;
    }

    /// A hostile ground effect: where its centre is and how wide it is, so a seat can see both which way out is
    /// shortest and, for one it is not in yet, which way not to walk.
    struct Hazard
    {
        float Distance = 0.0f;      // yards from the unit to its centre
        float Radius = 0.0f;        // ... and its radius, so Radius - Distance is the way out (negative: outside it)
        float Bearing = 0.0f;       // the direction of its centre, relative to the unit's facing
        Position Centre;            // where it is, so a cached one can be measured again as the seat moves
        bool Present = false;
    };

    struct SeatOption
    {
        SeatOptionKind Kind = SeatOptionKind::None;
        uint64 UntilMs = 0;                         // the clock (SeatView::NowMs) it runs out at

        [[nodiscard]] bool Running(SeatOptionKind kind, uint64 nowMs) const
        {
            return Kind == kind && nowMs < UntilMs;
        }
    };

    /// Which option a kind occupies: a seat runs one positioning option and one standby option at a time. Keeping a
    /// caster at range and waiting for its cast are not alternatives, and with a single slot each press of one threw
    /// the other away -- a melee seat holding an interrupt stopped staying on its target.
    enum class SeatOptionSlot : uint8
    {
        Positioning = 0,
        Standby,
        Count
    };

    [[nodiscard]] constexpr SeatOptionSlot SlotOf(SeatOptionKind kind)
    {
        return IsPositioning(kind) ? SeatOptionSlot::Positioning : SeatOptionSlot::Standby;
    }

    /// The durative actions a seat is running, one per slot.
    struct SeatOptionSet
    {
        std::array<SeatOption, std::size_t(SeatOptionSlot::Count)> Slots{};

        [[nodiscard]] SeatOption& Of(SeatOptionKind kind) { return Slots[std::size_t(SlotOf(kind))]; }
        [[nodiscard]] SeatOption const& Of(SeatOptionKind kind) const { return Slots[std::size_t(SlotOf(kind))]; }

        [[nodiscard]] bool Running(SeatOptionKind kind, uint64 nowMs) const { return Of(kind).Running(kind, nowMs); }
        [[nodiscard]] bool Any(uint64 nowMs) const
        {
            for (SeatOption const& option : Slots)
                if (option.Kind != SeatOptionKind::None && nowMs < option.UntilMs)
                    return true;
            return false;
        }

        void Start(SeatOptionKind kind, uint64 untilMs) { Of(kind) = SeatOption{ kind, untilMs }; }
        void Stop(SeatOptionKind kind) { if (Of(kind).Kind == kind) Of(kind) = SeatOption(); }
        void Clear() { Slots = {}; }
    };

    struct SeatView
    {
        Layout const* L = nullptr;
        Player* Bot = nullptr;
        /// The seat's durative action, to read, start and stop. Null for a view without one.
        /// The nearest hostile ground effect the seat is not standing in (StageScenario::TrackHazards): what makes
        /// avoiding one possible rather than only leaving one.
        Hazard NearestHazard;
        SeatOptionSet* Option = nullptr;
        /// How long each durative action may run (CurriculumTuning::OptionTuning).
        CurriculumTuning::OptionTuning Options;
        /// What the actions aim at: the opponent, the selected enemy. May be null (between pulls).
        Unit* Target = nullptr;
        /// The target when the bot can neither see nor detect it (stealth, invisibility). Target is null then, so no
        /// block reads what a player could not know; the duel block searches where it was last seen.
        Unit* HiddenTarget = nullptr;
        bool TargetSeen = false;                    // LastSeen holds where the target was when the bot last saw it
        Position LastSeen;
        float TargetUnseenTime = 0.0f;              // time since the bot last saw the target / 20 s, clamped

        /// Per catalog action, the highest rank the bot knows (SeatState::KnownRanks); null for a view built
        /// without one, where Encoding::KnownRank resolves the chain itself.
        std::vector<SpellInfo const*> const* KnownRanks = nullptr;

        // Core: the character, as built, and what happened since the last decision.
        uint8 Level = 1;
        uint8 Race = 0;
        uint8 Spec = 0;
        TalentBuilder::Build const* Build = nullptr;
        float LastStepDamage = 0.0f;                // damage done / the level's damage scale
        float LastStepPowerDelta = 0.0f;            // primary power change, as a fraction of max
        float LastStepDamageTaken = 0.0f;           // / the bot's max health
        float EpisodeTime = 0.0f;                   // time into the episode / EPISODE_TIME_SCALE_MS, clamped
        SeatMemory const* Memory = nullptr;         // what the seat has been doing; null: none (features at rest)
        uint64 NowMs = 0;                           // the clock Memory was kept with

        // Duel: time in combat, what the bot brought (potions, bandages, stones), whether it may resurrect itself, and
        // a hunter's beasts on offer.
        float CombatTime = 0.0f;                    // time in combat / 60 s, clamped; 0 out of combat
        BattleSupplies Supplies;
        bool SelfResurrectAllowed = true;           // not in the PvP stages
        std::array<uint32, STABLE_SLOTS> Stable{};
        uint32 StableCount = 0;

        // Pack: the current pull's enemies, in slot order (null for a slot whose enemy is gone).
        std::array<Unit*, PACK_SLOTS> Enemies{};
        uint32 EnemyCount = 0;                      // slots in use; 0 between pulls
        uint32 TargetSlot = 0;                      // the selected enemy; target selection updates it

        // Support: the selected friend (FRIEND_SELF, FRIEND_OWNER, FRIEND_TEAMMATE_FIRST + teammate) that positive
        // single-target spells are cast on, and the rank tier heals with ranks are cast at (0 = highest known). Only
        // read with the support block; the actions update them.
        uint32 FriendSlot = FRIEND_SELF;
        uint32 RankTier = 0;

        // Gauntlet.
        uint32 PullsCleared = 0;
        float QuietTime = 0.0f;                     // time since the last fight ended / 20 s, clamped
        float PullTime = 0.0f;                      // time into the current pull / 60 s, clamped
        bool ElitePull = false;
        uint32 FoodItem = 0;
        uint32 DrinkItem = 0;
        uint32 GauntletSupplies = CONSUMABLE_COUNT; // food and drink stocked, each
        float PullArrival = 0.0f;                   // an unengaged pull comes to the seat in this / 30 s; else 0
        float NextPull = 0.0f;                      // between pulls: the next one spawns in this / 20 s

        // Companion: the player the bot fights for.
        Player* Owner = nullptr;
        std::optional<Role> OwnerRole;              // what the owner plays, when the scenario knows it

        // Party: the other learned players, and the party's living tank (may be the bot).
        struct Teammate
        {
            Player* Bot = nullptr;
            int32 Goal = NO_GOAL;                   // what it is pursuing (SeatGoal), as its policy last sent
            Role PlayRole = Role::Dps;
            uint8 Class = 0;
        };

        std::array<Teammate, PARTY_MEMBERS> Teammates{};
        Player* Tank = nullptr;

        /// The raid the seat's group belongs to, in aggregate: a seat acts on its own group and the spotlight slots,
        /// but it has to know how the rest of the raid is doing. All zero below a party.
        struct RaidView
        {
            uint32 Group = 0;                       // the seat's group index (0 in a party)
            float Alive = 0.0f;                     // living seats, as a share of the seats in play
            float GroupAlive = 0.0f;                // ... of the seat's own group
            float InCombat = 0.0f;                  // seats in combat, as a share of the living
            float LowestHealth = 1.0f;              // the most hurt living seat
            float TanksAlive = 0.0f;                // living tanks / RAID_GROUPS, clamped
            float HealersAlive = 0.0f;              // living healers / RAID_GROUPS, clamped
        };

        RaidView Raid;

        // Travel: where the seat is going.
        bool HasObjective = false;
        Position Objective;

        // Flag match: the seat's flag and the other side's, from the seat's side.
        enum class FlagState : uint8 { AtBase, Carried, Dropped };
        struct FlagMatch
        {
            bool Active = false;
            FlagState Own = FlagState::AtBase;      // carried: by the enemy
            FlagState Enemy = FlagState::AtBase;    // carried: by the seat
            Position OwnBase;
            Position EnemyBase;
            Position OwnDropped;                    // where each lies when dropped
            Position EnemyDropped;
            uint32 OwnScore = 0;
            uint32 EnemyScore = 0;
        } Flags;

        // PvP: the enemy player.
        Player* Opponent = nullptr;
        bool OpponentHidden = false;                // the bot can neither see nor detect it
        uint8 OpponentClass = 0;
        Role OpponentRole = Role::Dps;
        bool Mirror = false;                        // the opponent is a learned agent too
    };

    /// What an applied action did, for the scenario's bookkeeping and rewards.
    /// A pet order a seat gave (SeatActionResult::PetOrderGiven), for the per-order episode counts.
    enum class PetOrder : uint8
    {
        None,
        Attack,                                     // sent pets and guardians at the target
        Passive,
        Defensive,
        Aggressive,
        Follow,
        Stay,
        Count
    };

    struct SeatActionResult
    {
        uint32 SpellCasts = 0;
        uint32 TrinketUses = 0;
        uint32 ItemUses = 0;                        // use effects of an equipped weapon or off-hand item
        uint32 SustainCasts = 0;
        uint32 FoodUsed = 0;
        uint32 DrinkUsed = 0;
        uint32 HealsOnFull = 0;                     // direct heals started on a friend at full health (masked: 0)
        uint32 DefensiveCasts = 0;                  // short damage reductions and immunities started
        uint32 HealingCasts = 0;                    // heals, HoTs and absorbs started ...
        uint32 DownrankedCasts = 0;                 // ... below the highest known rank
        uint32 FoodFailed = 0;                      // eat or drink pressed and allowed, but nothing was consumed
        uint32 DrinkFailed = 0;
        bool StealthOpener = false;                 // a harmful spell that breaks stealth started from stealth
        ObjectGuid StealthUtilityTarget;            // a harmful spell that keeps stealth (Sap, Distract) aimed here
        ObjectGuid PendingInterrupt;                // an interrupt was cast at this casting enemy
        uint32 CallBeast = 0;                       // hunters: call this stable beast (the scenario creates the pet)
        uint32 ConsumablesUsed = 0;                 // potions, healthstones, bandages, soulstones
        /// Whether the press did something in the world that costs a resource or a global cooldown: a spell that
        /// started casting, an item or trinket used, food or drink, a pet ability. Pressing the same button again is
        /// only waste when the button did nothing -- a caster's rotation is the same nuke over and over, and
        /// charging it as a repeat charges the correct play (stage1_duel at 10M: warlock_dps 39.7 repeated presses
        /// an episode, mage_dps 16.6, the two lowest-scoring layouts in the run).
        [[nodiscard]] bool DidSomething() const
        {
            return SpellCasts || ItemUses || TrinketUses || PetAbilities || FoodUsed || DrinkUsed;
        }

        uint32 PreparationMs = 0;                   // a helpful spell started out of combat: its cast time or a GCD
        bool SelfResurrected = false;
        uint32 Revives = 0;                         // resurrection spells started on a dead ally
        uint32 PetAbilities = 0;                    // pet bar abilities the pet started
        uint32 PetOrders = 0;                       // pet stances, follow and stay, and sending the pet in
        PetOrder PetOrderGiven = PetOrder::None;    // which of them, when one was given
    };
}

#endif

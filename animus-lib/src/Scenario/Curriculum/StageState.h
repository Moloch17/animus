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

#ifndef ANIMUS_LIB_CURRICULUM_STAGE_STATE_H
#define ANIMUS_LIB_CURRICULUM_STAGE_STATE_H

#include "Block.h"
#include "BotSlot.h"
#include "ObjectGuid.h"
#include "Position.h"
#include "RewardLedger.h"
#include "SeatMemory.h"
#include "SeatView.h"
#include "SeatCharacter.h"
#include "Supplies.h"
#include "TalentBuilder.h"
#include <array>
#include <vector>

/*
 * The state every curriculum stage has: the seats' characters and their episode totals. What only some stages have
 * (pulls, the owner, the party group, the enemy player) is kept by the encounter that needs it.
 */
class SpellInfo;

namespace Animus::Curriculum
{
    struct Layout;

    /// A seat's fight against whatever fights back (duel, pulls, PvP): what the rewards and episode info share.
    struct CombatTally
    {
        uint64 DamageTaken = 0;
        float LastDistance = -1.0f;             // approach shaping: excess distance at the last reward; < 0 = none yet
        bool Killed = false;                    // its opponent died (pack: the pull was cleared)
        uint32 KillTimeMs = 0;
        bool Died = false;                      // died at least once
        uint32 Deaths = 0;
        bool DeathCounted = false;              // the current death has been paid for (again after standing up)
        uint32 DeathMs = 0;                     // episode time of the current death
        uint32 StealthOpeners = 0;              // harmful spells from stealth that broke it
        bool StepStealthOpener = false;         // one started since the last reward
        uint32 StealthUtilityCasts = 0;         // harmful spells from stealth that kept it, paid ones
        uint32 StepStealthUtility = 0;          // paid ones since the last reward
        std::vector<ObjectGuid> StealthUtilityTargets;  // targets already paid for during the current stealth
        bool Engaged = false;                   // the fight has started: the bot or its opponent entered combat
        uint32 EngageMs = 0;                    // episode time it started; the fast kill bonus counts from here
        bool PetSummoned = false;
        uint32 CastsCompleted = 0;
        uint32 CastsCancelled = 0;
        uint64 CastMsWasted = 0;
        uint32 CastsStopped = 0;
        uint32 CastsMoved = 0;
        uint32 CastsTargetLost = 0;
        uint32 CastsOther = 0;
        bool TimedOut = false;                  // creature duel: the clock ran out with neither side dead
        uint32 TargetEvadeMs = 0;               // creature duel: time the opponent spent evading (leashed, unreachable)
        uint32 OutOfSightMs = 0;                // creature duel: time engaged without line of sight to the opponent
        uint32 UnreachableMs = 0;               // creature duel: time the opponent had no path to its victim
        uint32 UnreachableStreakMs = 0;         // ... without a break, up to now
        uint32 OpponentTeleports = 0;           // ... times it was put back beside its victim for it
        // Style, over the time the fight was on with the bot alive (one-on-one arenas): how much of it the bot spent
        // within melee reach of its opponent, and how much the opponent spent attacking the bot's pet or guardian.
        uint32 FightMs = 0;
        uint32 InMeleeMs = 0;
        uint32 OnPetMs = 0;
        // ... and how much the opponent spent rooted or snared by the bot, its pet or its totems, and how often the
        // bot put a root or snare on it (a new one where there was none).
        uint32 RootedMs = 0;
        uint32 SnaredMs = 0;
        uint32 RootsApplied = 0;
        uint32 SnaresApplied = 0;
        bool WasRooted = false;
        bool WasSnared = false;
        // Feign death (one-on-one arenas): how often the bot feigned, and how often its opponent then went home to
        // evade (and heal to full) because nothing else held it, rather than turning on the pet.
        uint32 FeignDeaths = 0;
        uint32 FeignDeathResets = 0;
        bool WasFeigning = false;
        uint32 FeignEndMs = 0;                  // episode time the last feign ended (or now, while feigning)
        bool FeignResetCounted = false;         // the last feign's evade has been counted
    };

    /// One learned agent: its character, as built for the episode, and its episode totals.
    struct SeatState
    {
        Layout const* L = nullptr;              // null for a party seat left empty this episode
        BotSlot Bot;

        uint8 Race = 0;
        uint8 Level = 1;
        uint8 Spec = 0;
        SeatCharacter::TalentPlan TalentPlan = SeatCharacter::TalentPlan::Standard;
        TalentBuilder::Build Build;
        uint32 UnspentTalentPoints = 0;
        uint32 EquippedItems = 0;
        float DamageScale = 1.0f;
        std::vector<uint32> Stable;             // hunters: beasts offered this episode
        bool PetAtStart = false;                // the episode started with the seat's pet out

        /// The highest rank of every catalog action the bot knows, resolved once when the character is built:
        /// walking the rank chain per action per decision is most of what observing a seat costs, and the
        /// spellbook does not change inside an episode. Empty until the seat has a character.
        std::vector<SpellInfo const*> KnownRanks;

        uint32 LastPower = 0;
        float LastStepDamage = 0.0f;
        float LastStepPowerDelta = 0.0f;
        float LastStepDamageTaken = 0.0f;
        uint32 SpellCasts = 0;
        uint32 TrinketUses = 0;
        uint32 ItemUses = 0;
        bool InCombat = false;
        uint32 CombatStartMs = 0;               // episode time the bot entered its current combat
        uint32 TargetSlot = 0;                  // the selected enemy (pulls)

        // Where the bot last saw its target, for when the target hides (SeatView::HiddenTarget).
        ObjectGuid LastSeenGuid;
        Position LastSeen;
        uint32 LastSeenMs = 0;

        // What the character brought (potions, bandages, stones), and what it did with it.
        BattleSupplies Supplies;
        uint32 ConsumablesUsed = 0;
        uint32 SelfResurrections = 0;
        uint32 PetAbilities = 0;
        uint32 PetOrders = 0;
        // Which pet orders the seat gave (by PetOrder), and what its pet was doing while out: attacking something,
        // set passive, told to stay.
        std::array<uint32, std::size_t(PetOrder::Count)> PetOrderCounts{};
        uint32 PetOutMs = 0;
        uint32 PetAttackingMs = 0;
        uint32 PetPassiveMs = 0;
        uint32 PetStayingMs = 0;
        bool PetDied = false;                   // a pet the seat had died this episode
        float LastPetHealth = 0.0f;             // the pet's health at the last decision (0 = no pet)
        ObjectGuid LastPetGuid;                 // the pet given its default stance (PetBlock::DefaultStance)
        uint32 Revives = 0;                   // dead allies (owner, teammates) the seat resurrected
        bool StepRevivedAlly = false;           // an ally the seat resurrected stood up this decision

        // Pacing (CurriculumTuning::ActionTuning) and what the seat has been doing, on the episode clock (sized to the
        // layout at the episode's first observation).
        SeatMemory Memory;
        uint32 ActionsPressed = 0;              // actions other than the no-op the seat took

        // Repeats (ActionTuning::Repeat): per layout action, the episode times of its presses within the window (empty
        // until the first press); the charged presses since the last reward, and over the episode.
        std::vector<std::vector<uint32>> PressTimes;
        uint32 StepRepeats = 0;
        uint32 RepeatedPresses = 0;

        CombatTally Combat;
        RewardLedger Rewards;

        /// Clear the episode totals (not the character).
        void ResetEpisode()
        {
            LastStepDamage = 0.0f;
            LastStepPowerDelta = 0.0f;
            LastStepDamageTaken = 0.0f;
            SpellCasts = 0;
            TrinketUses = 0;
            ItemUses = 0;
            InCombat = false;
            CombatStartMs = 0;
            TargetSlot = 0;
            LastSeenGuid.Clear();
            LastSeenMs = 0;
            Supplies = BattleSupplies();
            ConsumablesUsed = 0;
            SelfResurrections = 0;
            PetAbilities = 0;
            PetOrders = 0;
            PetOrderCounts.fill(0);
            PetOutMs = 0;
            PetAttackingMs = 0;
            PetPassiveMs = 0;
            PetStayingMs = 0;
            PetDied = false;
            LastPetHealth = 0.0f;
            LastPetGuid.Clear();
            Revives = 0;
            StepRevivedAlly = false;
            Memory.Reset(0);
            ActionsPressed = 0;
            PressTimes.clear();
            StepRepeats = 0;
            RepeatedPresses = 0;
            Combat = CombatTally();
            Rewards.ResetEpisode();
        }
    };

    /// EnvState::Arena before the env's first episode.
    constexpr uint32 NO_ARENA = ~uint32(0);

    /// StageScenario::ForceLayout and ForceTier when nothing is forced.
    constexpr uint32 NO_LAYOUT = ~uint32(0);
    constexpr uint32 NO_TIER = ~uint32(0);

    struct EnvState
    {
        uint32 Arena = NO_ARENA;                // index into the stage's arenas: what this episode is
        std::array<SeatState, MAX_SEATS> Seats;
        uint32 ActiveSeats = 1;                 // seats with a character this episode (the first ones)
        bool Fresh = false;                     // built by Setup, not yet reset
        bool BuildFailed = false;               // the last reset could not build the episode: end it and retry
        uint32 OpponentEntry = 0;               // creature entry: the duel's opponent, the first pull's first member
    };
}

#endif

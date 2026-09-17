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
#include "ObjectGuid.h"
#include "Position.h"
#include "Supplies.h"
#include "TalentBuilder.h"
#include <array>
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
    struct SeatView
    {
        Layout const* L = nullptr;
        Player* Bot = nullptr;
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

        // Party: the other learned players, and the party's living tank (may be the bot).
        struct Teammate
        {
            Player* Bot = nullptr;
            Role PlayRole = Role::Dps;
            uint8 Class = 0;
        };

        std::array<Teammate, PARTY_MEMBERS> Teammates{};
        Player* Tank = nullptr;

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
        uint32 FoodFailed = 0;                      // eat or drink pressed and allowed, but nothing was consumed
        uint32 DrinkFailed = 0;
        bool StealthOpener = false;                 // a harmful spell that breaks stealth started from stealth
        ObjectGuid StealthUtilityTarget;            // a harmful spell that keeps stealth (Sap, Distract) aimed here
        ObjectGuid PendingInterrupt;                // an interrupt was cast at this casting enemy
        uint32 CallBeast = 0;                       // hunters: call this stable beast (the scenario creates the pet)
        uint32 ConsumablesUsed = 0;                 // potions, healthstones, bandages, soulstones
        uint32 PreparationMs = 0;                   // a helpful spell started out of combat: its cast time or a GCD
        bool SelfResurrected = false;
        uint32 Revives = 0;                         // resurrection spells started on a dead ally
        uint32 PetAbilities = 0;                    // pet bar abilities the pet started
        uint32 PetOrders = 0;                       // pet stances, follow and stay, and sending the pet in
        PetOrder PetOrderGiven = PetOrder::None;    // which of them, when one was given
    };
}

#endif

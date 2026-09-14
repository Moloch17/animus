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

#ifndef ANIMUS_CLASS_ROLE_SEAT_ENCODER_H
#define ANIMUS_CLASS_ROLE_SEAT_ENCODER_H

#include "ClassRoleLayout.h"
#include "ObjectGuid.h"
#include "TalentBuilder.h"
#include <array>

class Item;
class Player;
class SpellInfo;
class Unit;

/*
 * A class/role policy's inputs and outputs in the world: the observation row and action mask of one bot, and what
 * each action does, as the models were trained to see and act. The caller describes the bot's situation (SeatView)
 * -- who its enemies, owner, teammates and opponent are, and the episode facts only it knows -- and the encoder reads
 * everything else from the world.
 */
namespace Animus::ClassRole
{
    /// One bot's situation at a decision.
    struct SeatView : LayoutConstants
    {
        Layout const* L = nullptr;
        Player* Bot = nullptr;
        /// What the actions aim at: the dummy, the opponent, the selected enemy. May be null (between pulls).
        Unit* Target = nullptr;

        // The character, as built.
        uint8 Level = 1;
        uint8 Race = 0;
        uint8 Spec = 0;
        TalentBuilder::Build const* Build = nullptr;

        // Since the last decision.
        float LastStepDamage = 0.0f;                // damage done / the level's damage scale
        float LastStepPowerDelta = 0.0f;            // primary power change, as a fraction of max
        float LastStepDamageTaken = 0.0f;           // / the bot's max health
        float CombatTime = 0.0f;                    // time in combat / 60 s, clamped; 0 out of combat

        // Duel on: hunters' beasts on offer.
        std::array<uint32, STABLE_SLOTS> Stable{};
        uint32 StableCount = 0;

        // Pack on: the current pull's enemies, in slot order (null for a slot whose enemy is gone).
        std::array<Unit*, PACK_SLOTS> Enemies{};
        uint32 EnemyCount = 0;                      // slots in use, at most PACK_SLOTS; 0 between pulls
        uint32 TargetSlot = 0;                      // the selected enemy; Apply updates it

        // Gauntlet on.
        uint32 PullsCleared = 0;
        float QuietTime = 0.0f;                     // time since the last fight ended / 20 s, clamped (used between fights)
        float PullTime = 0.0f;                      // time into the current pull / 60 s, clamped (used during one)
        bool ElitePull = false;
        uint32 FoodItem = 0;
        uint32 DrinkItem = 0;

        // Companion on: the player the bot fights for.
        Player* Owner = nullptr;

        // Party on: the other learned players, and the party's living tank (may be the bot).
        struct Teammate
        {
            Player* Bot = nullptr;
            Role PlayRole = Role::Dps;
            uint8 Class = 0;
        };

        std::array<Teammate, PARTY_MEMBERS> Teammates{};
        Player* Tank = nullptr;

        // PvP on: the enemy player.
        Player* Opponent = nullptr;
        uint8 OpponentClass = 0;
        Role OpponentRole = Role::Dps;
        bool Mirror = false;                        // the opponent is a learned agent too
    };

    /// What an applied action did, for the caller's bookkeeping and rewards.
    struct SeatActionResult
    {
        uint32 SpellCasts = 0;
        uint32 TrinketUses = 0;
        uint32 SustainCasts = 0;
        uint32 FoodUsed = 0;
        uint32 DrinkUsed = 0;
        bool StealthOpener = false;                 // a stealth opener at the target started
        ObjectGuid PendingInterrupt;                // an interrupt was cast at this casting enemy
        uint32 CallBeast = 0;                       // hunters: call this stable beast (the caller creates the pet)
    };

    class SeatEncoder : public LayoutConstants
    {
    public:
        /// Write the layout's observation (view.L->ObsDim values) and action mask (view.L->NumActions); action 0 is
        /// always allowed. Nothing is written past the layout's sizes.
        static void Observe(SeatView const& view, float* obs, uint8* mask);

        /// Apply `action` as the client would: casts, item uses, movement, target selection, pet commands. Masked
        /// actions do nothing.
        static void Apply(SeatView& view, int32 action, SeatActionResult& result);

        /// Put the Call Pet global cooldown on the bot, as calling a beast does (after a successful CallBeast).
        static void StartCallBeastCooldown(Player* bot);

        /// Send the bot's pets and guardians at `target`, as the pet bar's Attack does. True if any was ordered.
        static bool PetAttack(Player* bot, Unit* target);

        /// The on-use spell of an equipped item, or null.
        [[nodiscard]] static SpellInfo const* TrinketSpell(Item const* item);
    };
}

#endif

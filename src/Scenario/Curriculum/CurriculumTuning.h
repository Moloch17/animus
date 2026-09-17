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

#ifndef ANIMUS_LIB_CURRICULUM_CURRICULUM_TUNING_H
#define ANIMUS_LIB_CURRICULUM_CURRICULUM_TUNING_H

#include "Define.h"
#include <boost/json/fwd.hpp>
#include <string>

/*
 * Every value that shapes what the curriculum trains on and what it is paid for: the characters, parties,
 * pulls and scripted players it meets, and the reward weights. Each is the config key <prefix><key>, the prefix being
 * the host's (StageSettings::TuningPrefix: AnimusForge.Curriculum.<key> in mod_animus_forge.conf.dist, which documents
 * them, and Animus.Curriculum.<key> in mod-animus); the effective values are recorded in each stage's stage.json and so
 * in every run directory. Visit lists them once, for loading and for writing.
 *
 * Per-decision reward terms are tuned per 50 ms decision and scaled with StageSettings::DecisionMs.
 */
namespace Animus::Curriculum
{
    struct CurriculumTuning
    {
        /// The seat characters.
        struct CharacterTuning
        {
            uint32 HighLevelFirst = 61;         // levels at or above this are "high"
            int32 HighLevelChance = 50;         // percent of characters drawn from the high levels
            /// Levels at or below this are "low". With half the characters at 61-80 and the rest spread over every
            /// level, only one in eight was 1-20, and those fights were the ones lost most (stage1_duel at 20M: 69%
            /// won at 1-10, 77% at 11-20, 85%+ from 31 on).
            uint32 LowLevelLast = 20;
            int32 LowLevelChance = 15;          // percent of characters drawn from the low levels (the rest: any level)
            // How a character's talents are spent (see TalentBuilder). A standard build is always the same for a
            // spec and a level, so a policy trained on those alone has nothing to read in its talent features: some
            // characters move a few points, some spend them all at random, and the policy has to play what it got.
            int32 NoisyTalentChance = 30;       // percent of characters: the standard build with points moved
            int32 RandomTalentChance = 10;      // percent: every point spent at random (the rest: standard)
            uint32 TalentNoisePoints = 5;       // a noisy build moves 1 to this many of its last points
            // Percent of pet-class characters that start the episode with their pet out, as a player arrives with
            // one: the rest summon it themselves (or not).
            int32 PetOutChance = 50;
        } Characters;

        /// Which party seats have a character, and their roles.
        struct PartyTuning
        {
            int32 SizeWeight1 = 20;             // relative chance of 1, 2, 3 or 4 seats with a character
            int32 SizeWeight2 = 20;
            int32 SizeWeight3 = 20;
            int32 SizeWeight4 = 40;
            int32 ClassicChance = 50;           // percent: tank, healer, damage dealers instead of drawn roles
            int32 RoleTankChance = 25;          // drawn roles: percent tanks, healers, the rest damage dealers
            int32 RoleHealerChance = 25;
            // Rewards added to the owner's, per teammate.
            float TeammateDamageTakenDps = 0.5f;        // damage dealers: a non-tank teammate's damage taken
            float TeammateDamageTakenProtector = 1.0f;  // tanks and healers
            float TeammateHealing = 2.0f;               // healers: effective healing, fraction of its health
            float TankLoseTeammate = 0.02f;             // tanks: per enemy on a non-tank teammate, per decision
            float TeammateDeath = 3.0f;
        } Party;

        /// One-on-one fights against a creature (the duel) or a player (PvP).
        struct DuelTuning
        {
            float DamageDealt = 2.0f;           // fraction of the opponent's health: a kill is worth this in damage
            float DamageTaken = 1.0f;           // fraction of the bot's health
            float Approach = 0.5f;              // shaping toward the spec's range, per 40 yd closed
            float StealthOpener = 0.5f;         // a harmful spell from stealth that breaks it (Ambush, Cheap Shot,
                                                // a feral druid's Pounce or Ravage out of Prowl)
            float StealthUtility = 0.05f;       // one that keeps it (Sap, Distract), once per target per stealth
            float StepCost = 0.0002f;           // per decision
            /// Winning is what the stage is for, so the kill and the losses dwarf the rest. With Kill 3, FastKill up to
            /// 3 and a loss at -3, a risky fast opener (time bonus ~2.4) beat a sure slow win (~0.6) as soon as it won
            /// 79% of the time: the reward traded one fight in five for speed. At Kill 10, FastKill 1 and a loss at
            /// -10, that break-even is ~97%. The dense terms (damage, approach) stay small, as guidance.
            float Kill = 10.0f;
            float FastKill = 1.0f;              // times the fraction of the episode length left, from the engagement
            /// Times the fraction of the bot's health not lost. Damage taken is already charged as it happens
            /// (DamageTaken, dense), so this pays for the same thing again at the kill; together they were
            /// worth three times the damage dealt term, which reads as "survive" more than "win". stage1_duel
            /// bore that out: the policy beat the baseline on score everywhere while killing less often than it
            /// did as a rogue (0.87 against 0.94) and below level 20 (0.78 against 0.83), banking the difference
            /// in health it never spent. Halved, against a larger Kill, so that winning the fight outweighs
            /// finishing it untouched -- deaths were 0.002 an episode, so there is room to push.
            float HealthKept = 0.5f;
            float Death = 10.0f;
            /// A creature duel that runs out the clock without a kill (and without a death, which Death already
            /// charges). The duel is won by killing, so a timeout is a lost fight and ends the episode as one, not
            /// a cut-off the critic bootstraps across: without it the cheapest fight to lose was the one never
            /// started (stage1_duel at 30M: none of the 11 failed warlock episodes took any damage). As Death, so
            /// neither way of losing is the cheaper one to learn.
            float Timeout = 10.0f;
            /// Creature duel: per second the fight has not started once StallGraceMs of the episode are gone. Timeout
            /// alone charges standing still only at the end of the clock, 900 decisions away: stage1_duel at 20M had
            /// its deterministic policy stand where it spawned for all 90 s in 67 of 2048 episodes (21 without a
            /// single action), fights the same policy sampled won.
            float Stall = 0.05f;
            uint32 StallGraceMs = 15000;        // summoning a pet, buffing and sneaking up in stealth fit in this
            /// Preparing is not stalling: the grace grows by the time the seat spent starting buffs, forms, stances,
            /// stealth and pet summons out of combat (CombatTally::PreparationMs), up to this much.
            uint32 PreparationRefundMaxMs = 30000;
            /// Creature duel, ranged specs: per second the opponent stands in melee range hitting the bot. The approach
            /// shaping only pays for closing in, so nothing told a hunter, mage or warlock to keep the range it
            /// fights best at (stage1_duel at 20M: 88 of 96 hunter kills ended within 5 yd).
            float Spacing = 0.03f;
            float MeleeRange = 3.5f;            // the range the approach shaping aims for, melee specs
            float RangedRange = 25.0f;          // ... ranged specs
        } Duel;

        /// How hard the creature duel's opponents are, per class/role. Tier t below EliteTier is a normal creature
        /// t x LevelsPerTier levels above the seat; from EliteTier on an elite, (t - EliteTier) x LevelsPerTier levels
        /// above. A class/role moves up a tier when it wins (kills without dying) RaiseAbove of Window fights at its
        /// tier, and down when it wins fewer than LowerBelow.
        struct DifficultyTuning
        {
            uint32 MaxTier = 6;
            uint32 EliteTier = 4;               // > MaxTier: no elites
            uint32 LevelsPerTier = 1;
            float RaiseAbove = 0.9f;
            float LowerBelow = 0.6f;
            uint32 Window = 200;                // fights at a tier before it is judged
            int32 ReviewChance = 25;            // percent of training fights drawn from a lower tier, so none is lost
        } Difficulty;

        /// Cast-time spells, from the duel stage on.
        struct CastingTuning
        {
            float TimeWasted = 0.03f;           // per second spent on a cast that did not finish
            /// Per second of cast time of a cast that finished, in combat (not out of it, so casting long spells at
            /// nothing earns nothing). It pays a long useless cast as readily as the right one, so keep it small next
            /// to what a cast does, which damage, healing and the kill already pay for.
            float TimeCompleted = 0.03f;
            /// Per cast the bot cut short itself, whatever it had spent on it. TimeWasted is proportional to the
            /// seconds lost, so a cast stopped on the decision after it began costs almost nothing: under a
            /// deterministic policy that leaves start-cast / stop-cast a free loop to sit in for a whole episode
            /// (stage1_duel: a quarter of the warlock evaluation episodes, up to 299 cancels in one). A flat charge
            /// prices the loop -- hundreds of them outweigh anything an episode can pay -- while leaving the
            /// handful of deliberate stops a fight actually wants cheap next to the kill, so when to cut a cast
            /// short stays the policy's call.
            float Cancel = 0.05f;
        } Casting;

        /// How often a seat may press the same button, as a player would. Each decision is 100 ms apart, and a
        /// policy free to act on every one of them re-issues orders nobody would: stage1_duel's warlocks sent their
        /// pet in 125 times an episode and started and stopped a cast 26 times while standing out of the fight. A
        /// paced action is masked until it may be pressed again, so the policy never sees the loop as an option.
        /// Spells keep their own global cooldown and cooldowns as well. 0 turns a pace off.
        struct ActionTuning
        {
            uint32 RepeatMs = 1000;             // the same action again: spells, orders, consumables, targeting
            uint32 MoveRepeatMs = 300;          // the same movement order again (steering stays responsive)
            uint32 StopCastMinMs = 500;         // a cast the bot is in cannot be stopped before it ran this long
            uint32 RecastAfterStopMs = 2000;    // a spell the bot stopped itself cannot be started again for this long
            /// A movement order back the way the last one went (in toward the target, then away, or the reverse)
            /// waits this long: stage1_duel's seats gave 60 to 140 movement orders a fight, running in and backing
            /// off by turns. Movement is still free; only the reversal waits.
            uint32 ReverseMoveMs = 1000;
            /// A stance, form, presence, aspect, aura, seal, armor or pet stance holds this long before another change
            /// of its kind: warrior tanks changed stance 22 times a fight, hunters their aspect 12.
            uint32 ModeLockMs = 5000;
            /// Pressing the same action over and over. Pacing caps how often an action can be pressed, not how many
            /// times in a row: stage1_duel's warlocks gave their pet 93 orders an episode, a second apart, and the
            /// pet dealt 1% of their damage. Each press of an action counts the presses of that same action within
            /// the last RepeatWindowMs; past the free ones, each costs Repeat. How often the seat acts overall is not
            /// charged, only the same button again, and movement orders never are: steering is always free.
            float Repeat = 0.02f;
            uint32 RepeatWindowMs = 10000;
            uint32 RepeatFree = 3;              // presses of one action within the window that cost nothing
        } Actions;

        /// Packs and the gauntlet's pull after pull.
        struct PullTuning
        {
            int32 LinkedChance = 70;            // percent of pulls whose members aggro together
            int32 EliteChance = 15;             // gauntlet: a single elite instead of a pack
            int32 HigherLevelChance = 25;       // gauntlet: a pack 1-3 levels above (1 below level 20, 2 below 30)
            int32 PartyEliteChance = 50;        // party: per pack member
            uint32 NextPullMinMs = 8000;        // gauntlet: the break between pulls
            uint32 NextPullMaxMs = 20000;
            uint32 OwnerEngageMinMs = 1500;     // with an owner: when it walks over to a new pull
            uint32 OwnerEngageMaxMs = 5000;
            uint32 PartyOwnerEngageMinMs = 4000;    // ... in a party, after the tank has had time to pull
            uint32 PartyOwnerEngageMaxMs = 7000;
            uint32 OwnerPullsMinMs = 500;       // ... when the owner starts the pull itself
            uint32 OwnerPullsMaxMs = 1500;
            int32 OwnerPullsChance = 30;        // percent of pulls a damage dealer or healer owner starts (tanks: all)
            float RecoverFraction = 0.5f;       // owner stages: health and mana the dead stand up with after a pull
            // Rewards.
            float DamageDealt = 2.0f;           // fraction of the pull's total health
            float DamageTaken = 1.0f;           // pack: fraction of the bot's health
            float GauntletDamageTaken = 1.5f;   // gauntlet on: surviving many pulls matters more than any one
            float Approach = 0.5f;
            float StealthOpener = 0.5f;
            float StealthUtility = 0.05f;
            float Interrupt = 0.3f;
            float Kill = 0.5f;
            float StepCost = 0.0002f;           // per decision
            float Clear = 2.0f;                 // gauntlet and owner stages: each pull cleared
            float FastPull = 2.0f;              // gauntlet: times 1 - time since the pull engaged / 60 s
            float HealthKept = 2.0f;            // gauntlet and owner stages: times the health kept through the pull
            float GauntletDeath = 5.0f;
            /// A single pack is won or lost, as the duel is: the clear outweighs finishing it untouched, and dying or
            /// running out of time costs as much as the clear pays. Clear and HealthKept had the pack worth 2 + 2, so
            /// keeping health paid as much as winning, and a death cost only 3.
            float PackClear = 10.0f;
            float FastClear = 1.0f;             // pack: times the episode fraction left after engaging
            float PackHealthKept = 0.5f;        // pack: times the health kept through the pack
            float PackDeath = 10.0f;
            /// Pack: the top rung of the single pack's ladder (PullsEncounter's PACK_RUNGS, 0-5), climbed per
            /// class/role with the Difficulty.* rates. 0 keeps every pack on the first rung.
            uint32 MaxTier = 5;
            float Timeout = 10.0f;              // pack: the clock ran out with the pack and the seat both alive
            /// A timeout charged only at the end is 150 s away when the kiting starts: the discount leaves about a
            /// fifth of it, against a whole death now, so running out the clock looked safe. A fight engaged longer
            /// than OvertimeGraceMs is charged as it drags on, and a death in overtime is charged the overtime left,
            /// so dying never ends it more cheaply than the timeout would.
            float Overtime = 0.1f;              // pack: per second of a fight past OvertimeGraceMs since it was engaged
            uint32 OvertimeGraceMs = 60000;
            float Stall = 0.05f;                // pack: per second not engaged once StallGraceMs are gone
            uint32 StallGraceMs = 15000;
            uint32 PreparationRefundMaxMs = 30000;  // pack: as the duel's
            float Spacing = 0.03f;              // pack: per second a ranged spec is hit in melee reach
            /// A gauntlet alone (no owner) is won by lasting: pull after pull until the episode ends, and a death ends
            /// it with every pull left unfought. Clear 2 + FastPull 2 and HealthKept 2 had each pull worth up to 6
            /// against a death at 5, so a seat could trade its life for a fast pull. As the single pack: the clear
            /// outweighs finishing it fast or untouched, and a death costs two clears besides the pulls it forfeits.
            /// Stall and Spacing apply to its pulls as to a single pack's (Stall from each pull's spawn). Owner stages
            /// keep Clear, FastPull, HealthKept and GauntletDeath.
            float SoloGauntletClear = 5.0f;
            float SoloGauntletFastPull = 1.0f;  // times 1 - time since the pull engaged / 60 s
            float SoloGauntletHealthKept = 0.5f;
            float SoloGauntletDeath = 10.0f;
            /// Paid when a pull is engaged, times the seat's health fraction the decision before, or the lower of its
            /// health and mana fractions if it uses mana: entering a fight ready is what resting between pulls is for.
            float SoloGauntletReadiness = 0.5f;
            /// Lasting to the end wins only with this many pulls cleared: a gauntlet is endured by fighting it, not
            /// by staying away from it.
            uint32 SoloGauntletWinPulls = 5;
            uint32 GauntletSupplies = 7;        // solo gauntlet: food and drink stocked, each
            /// Solo gauntlet: per second per pack member kept out of the fight once the pull is engaged -- stunned,
            /// incapacitated, asleep, polymorphed, feared, or rooted out of melee reach and not casting -- other than
            /// the seat's target, while another member is alive. It stops when the control breaks, so controlling an
            /// add and hitting it pays nothing. At most SoloGauntletControlMax per pull: a fight isn't worth dragging
            /// out for it.
            float SoloGauntletControl = 0.02f;
            float SoloGauntletControlMax = 1.5f;
            /// Solo gauntlet pacing. A pull nobody has engaged comes to the seat ArriveMinMs-ArriveMaxMs after it
            /// spawns, so resting has a clock; each pull cleared brings the next one sooner (ArriveShrinkMs, down to
            /// ArriveFloorMs) and shortens the break before it (NextPullShrinkMs, down to NextPullFloorMs).
            uint32 ArriveMinMs = 20000;
            uint32 ArriveMaxMs = 40000;
            uint32 ArriveShrinkMs = 1500;
            uint32 ArriveFloorMs = 10000;
            uint32 NextPullShrinkMs = 1000;
            uint32 NextPullFloorMs = 4000;
            float OwnerClearScale = 2.0f;       // owner stages: kills and clears count this many times
        } Pulls;

        /// The scripted owner of the companion and party stages.
        struct OwnerTuning
        {
            int32 LevelSpread = 2;              // its level: the bot's plus or minus this
            int32 TankChance = 25;              // percent tanks, healers, the rest damage dealers
            int32 HealerChance = 25;
            // Rewards added to the pulls'.
            float DamageTakenDps = 1.0f;        // damage dealers: the owner's damage taken, fraction of its health
            float DamageTakenProtector = 2.0f;  // tanks and healers exist to prevent it
            float TankOwnerDamageShare = 0.25f; // a tank owner is hit by design: its damage taken counts this much
            float Healing = 2.0f;               // healers: effective healing, fraction of the owner's health
            float TankDamageRefund = 0.5f;      // tanks: soften the pulls' damage taken
            float TankHold = 0.002f;            // tanks: per enemy on the tank, per decision
            float TankLose = 0.02f;             // tanks: per enemy on the owner, per decision
            float PulledThreat = 0.004f;        // damage dealers and healers: per enemy on the bot, per decision
            float SoloFight = 0.01f;            // per decision in combat while the owner is not
            float FollowFar = 0.002f;           // per decision out of combat beyond FollowFarDistance
            float FollowNear = 0.0005f;         // per decision out of combat within FollowNearDistance
            float FollowFarDistance = 25.0f;
            float FollowNearDistance = 12.0f;
            float Death = 6.0f;
        } Owner;

        /// Resurrecting: a seat's own Soulstone or Reincarnation, and revives on allies (companion and party stages).
        struct ResurrectionTuning
        {
            uint32 GraceMs = 20000;             // the dead wait this long for a resurrection they can get before solo
                                                // stages end and owner stages stand them up (the next pull waits too)
            float ReviveAlly = 1.5f;            // a dead ally the seat resurrected stood up
        } Resurrection;

        /// The scripted enemy player of the PvP stage.
        struct OpponentTuning
        {
            int32 LevelSpread = 1;
            uint32 EngageMaxMs = 3000;          // it starts fighting up to this long into the episode
            int32 HealerChance = 20;
            int32 TankChance = 20;
        } Opponent;

        /// Scripted enemy players ambushing the owner (arenas with ambushers). Their class, role and level follow
        /// Opponent.* chances and spread.
        struct AmbushTuning
        {
            uint32 MinMs = 20000;               // beside pulls: they arrive this far into the episode ...
            uint32 MaxMs = 120000;              // ... at the latest
            uint32 EngageMaxMs = 3000;          // they start fighting up to this long after arriving
            float Kill = 3.0f;                  // every seat, per ambusher killed
        } Ambush;

        /// Getting to a place (travel arenas): how far it is, and what arriving pays.
        struct TravelTuning
        {
            float ObjectiveMin = 60.0f;         // ground: yards from the start (by path, reachable on foot)
            float ObjectiveMax = 320.0f;
            float FlyingMin = 350.0f;           // flying arenas: yards from the start
            float FlyingMax = 700.0f;
            float Progress = 1.0f;              // potential shaping: per 100 yd closed (taken back for leaving)
            float Arrive = 3.0f;
            float FastArrive = 3.0f;            // times the fraction of the episode still left
            float DamageTaken = 1.0f;           // fraction of the bot's health (falls, what it rode past)
            float Death = 3.0f;
            float StepCost = 0.0002f;           // per decision
        } Travel;

        /// The flag match (Warsong Gulch's rules between two seats).
        struct FlagTuning
        {
            float BaseMin = 100.0f;             // yards between the bases, by path
            float BaseMax = 180.0f;
            uint32 CapturesToWin = 3;
            uint32 RespawnMs = 15000;           // the dead stand up at their base after this (a graveyard wave)
            uint32 DroppedReturnMs = 10000;     // a dropped flag goes home on its own after this
            float TouchDistance = 4.0f;         // yards to pick up, return or capture
            float Capture = 5.0f;
            float Pickup = 1.0f;
            float Return = 1.0f;
            float CarrierKill = 1.5f;           // killing the one carrying the seat's flag
            float Lost = 3.0f;                  // the other side captured the seat's flag
            float Progress = 0.5f;              // potential shaping toward the seat's current objective, per 100 yd
            float Death = 1.0f;
            float StepCost = 0.0002f;           // per decision
        } Flag;

        /// How the scripted players (owner, PvP opponent, ambushers) play.
        struct ScriptedPlayerTuning
        {
            uint32 SpellMinMs = 2000;           // time between damage spells
            uint32 SpellMaxMs = 4000;
            uint32 HealMinMs = 1500;            // time between heals
            uint32 HealMaxMs = 2500;
            uint32 WanderMinMs = 6000;          // between pulls: time between wander steps
            uint32 WanderMaxMs = 12000;
            float RegenFraction = 0.04f;        // of max health and mana per second, out of combat
            float HealBelow = 0.85f;            // healers heal party members under this health fraction
            float SelfHealBelow = 0.6f;         // PvP healers heal themselves under this
            float HealerRange = 30.0f;          // healers stay this close to the tank
            float TauntRange = 25.0f;
            float RangedMin = 20.0f;            // PvP: a ranged spec backs off inside half this ...
            float RangedMax = 30.0f;            // ... and closes in beyond this
            int32 StealthChance = 50;           // PvP: percent of engagements a rogue (or a feral druid, which
                                                // shifts to Cat Form first) sneaks up in stealth
            int32 TacticsChance = 75;           // PvP: percent of engagements it plays its kit (below)
            uint32 ControlMinMs = 8000;         // ... time between crowd control attempts
            uint32 ControlMaxMs = 15000;
            float DefensiveBelow = 0.35f;       // ... a defensive when its health is under this
            float BreakBelow = 0.6f;            // ... breaks crowd control when its health is under this
        } ScriptedPlayers;

        /// Calls f(key, value) for every value, key relative to the tuning prefix (AnimusForge.Curriculum., ...).
        template <typename T, typename F>
        static void Visit(T& tuning, F&& f)
        {
            f("Characters.HighLevelFirst", tuning.Characters.HighLevelFirst);
            f("Characters.HighLevelChance", tuning.Characters.HighLevelChance);
            f("Characters.LowLevelLast", tuning.Characters.LowLevelLast);
            f("Characters.LowLevelChance", tuning.Characters.LowLevelChance);
            f("Characters.NoisyTalentChance", tuning.Characters.NoisyTalentChance);
            f("Characters.RandomTalentChance", tuning.Characters.RandomTalentChance);
            f("Characters.TalentNoisePoints", tuning.Characters.TalentNoisePoints);
            f("Characters.PetOutChance", tuning.Characters.PetOutChance);

            f("Party.SizeWeight1", tuning.Party.SizeWeight1);
            f("Party.SizeWeight2", tuning.Party.SizeWeight2);
            f("Party.SizeWeight3", tuning.Party.SizeWeight3);
            f("Party.SizeWeight4", tuning.Party.SizeWeight4);
            f("Party.ClassicChance", tuning.Party.ClassicChance);
            f("Party.RoleTankChance", tuning.Party.RoleTankChance);
            f("Party.RoleHealerChance", tuning.Party.RoleHealerChance);
            f("Party.TeammateDamageTakenDps", tuning.Party.TeammateDamageTakenDps);
            f("Party.TeammateDamageTakenProtector", tuning.Party.TeammateDamageTakenProtector);
            f("Party.TeammateHealing", tuning.Party.TeammateHealing);
            f("Party.TankLoseTeammate", tuning.Party.TankLoseTeammate);
            f("Party.TeammateDeath", tuning.Party.TeammateDeath);

            f("Duel.DamageDealt", tuning.Duel.DamageDealt);
            f("Duel.DamageTaken", tuning.Duel.DamageTaken);
            f("Duel.Approach", tuning.Duel.Approach);
            f("Duel.StealthOpener", tuning.Duel.StealthOpener);
            f("Duel.StealthUtility", tuning.Duel.StealthUtility);
            f("Duel.StepCost", tuning.Duel.StepCost);
            f("Duel.Kill", tuning.Duel.Kill);
            f("Duel.FastKill", tuning.Duel.FastKill);
            f("Duel.HealthKept", tuning.Duel.HealthKept);
            f("Duel.Death", tuning.Duel.Death);
            f("Duel.Timeout", tuning.Duel.Timeout);
            f("Duel.Stall", tuning.Duel.Stall);
            f("Duel.StallGraceMs", tuning.Duel.StallGraceMs);
            f("Duel.PreparationRefundMaxMs", tuning.Duel.PreparationRefundMaxMs);
            f("Duel.Spacing", tuning.Duel.Spacing);
            f("Duel.MeleeRange", tuning.Duel.MeleeRange);
            f("Duel.RangedRange", tuning.Duel.RangedRange);

            f("Difficulty.MaxTier", tuning.Difficulty.MaxTier);
            f("Difficulty.EliteTier", tuning.Difficulty.EliteTier);
            f("Difficulty.LevelsPerTier", tuning.Difficulty.LevelsPerTier);
            f("Difficulty.RaiseAbove", tuning.Difficulty.RaiseAbove);
            f("Difficulty.LowerBelow", tuning.Difficulty.LowerBelow);
            f("Difficulty.Window", tuning.Difficulty.Window);
            f("Difficulty.ReviewChance", tuning.Difficulty.ReviewChance);

            f("Casting.TimeWasted", tuning.Casting.TimeWasted);
            f("Casting.TimeCompleted", tuning.Casting.TimeCompleted);
            f("Casting.Cancel", tuning.Casting.Cancel);

            f("Actions.RepeatMs", tuning.Actions.RepeatMs);
            f("Actions.MoveRepeatMs", tuning.Actions.MoveRepeatMs);
            f("Actions.StopCastMinMs", tuning.Actions.StopCastMinMs);
            f("Actions.RecastAfterStopMs", tuning.Actions.RecastAfterStopMs);
            f("Actions.ReverseMoveMs", tuning.Actions.ReverseMoveMs);
            f("Actions.ModeLockMs", tuning.Actions.ModeLockMs);
            f("Actions.Repeat", tuning.Actions.Repeat);
            f("Actions.RepeatWindowMs", tuning.Actions.RepeatWindowMs);
            f("Actions.RepeatFree", tuning.Actions.RepeatFree);

            f("Pulls.LinkedChance", tuning.Pulls.LinkedChance);
            f("Pulls.EliteChance", tuning.Pulls.EliteChance);
            f("Pulls.HigherLevelChance", tuning.Pulls.HigherLevelChance);
            f("Pulls.PartyEliteChance", tuning.Pulls.PartyEliteChance);
            f("Pulls.NextPullMinMs", tuning.Pulls.NextPullMinMs);
            f("Pulls.NextPullMaxMs", tuning.Pulls.NextPullMaxMs);
            f("Pulls.OwnerEngageMinMs", tuning.Pulls.OwnerEngageMinMs);
            f("Pulls.OwnerEngageMaxMs", tuning.Pulls.OwnerEngageMaxMs);
            f("Pulls.PartyOwnerEngageMinMs", tuning.Pulls.PartyOwnerEngageMinMs);
            f("Pulls.PartyOwnerEngageMaxMs", tuning.Pulls.PartyOwnerEngageMaxMs);
            f("Pulls.OwnerPullsMinMs", tuning.Pulls.OwnerPullsMinMs);
            f("Pulls.OwnerPullsMaxMs", tuning.Pulls.OwnerPullsMaxMs);
            f("Pulls.OwnerPullsChance", tuning.Pulls.OwnerPullsChance);
            f("Pulls.RecoverFraction", tuning.Pulls.RecoverFraction);
            f("Pulls.DamageDealt", tuning.Pulls.DamageDealt);
            f("Pulls.DamageTaken", tuning.Pulls.DamageTaken);
            f("Pulls.GauntletDamageTaken", tuning.Pulls.GauntletDamageTaken);
            f("Pulls.Approach", tuning.Pulls.Approach);
            f("Pulls.StealthOpener", tuning.Pulls.StealthOpener);
            f("Pulls.StealthUtility", tuning.Pulls.StealthUtility);
            f("Pulls.Interrupt", tuning.Pulls.Interrupt);
            f("Pulls.Kill", tuning.Pulls.Kill);
            f("Pulls.StepCost", tuning.Pulls.StepCost);
            f("Pulls.Clear", tuning.Pulls.Clear);
            f("Pulls.FastPull", tuning.Pulls.FastPull);
            f("Pulls.HealthKept", tuning.Pulls.HealthKept);
            f("Pulls.GauntletDeath", tuning.Pulls.GauntletDeath);
            f("Pulls.PackClear", tuning.Pulls.PackClear);
            f("Pulls.FastClear", tuning.Pulls.FastClear);
            f("Pulls.PackHealthKept", tuning.Pulls.PackHealthKept);
            f("Pulls.PackDeath", tuning.Pulls.PackDeath);
            f("Pulls.MaxTier", tuning.Pulls.MaxTier);
            f("Pulls.Timeout", tuning.Pulls.Timeout);
            f("Pulls.Overtime", tuning.Pulls.Overtime);
            f("Pulls.OvertimeGraceMs", tuning.Pulls.OvertimeGraceMs);
            f("Pulls.Stall", tuning.Pulls.Stall);
            f("Pulls.StallGraceMs", tuning.Pulls.StallGraceMs);
            f("Pulls.PreparationRefundMaxMs", tuning.Pulls.PreparationRefundMaxMs);
            f("Pulls.Spacing", tuning.Pulls.Spacing);
            f("Pulls.SoloGauntletClear", tuning.Pulls.SoloGauntletClear);
            f("Pulls.SoloGauntletFastPull", tuning.Pulls.SoloGauntletFastPull);
            f("Pulls.SoloGauntletHealthKept", tuning.Pulls.SoloGauntletHealthKept);
            f("Pulls.SoloGauntletDeath", tuning.Pulls.SoloGauntletDeath);
            f("Pulls.SoloGauntletReadiness", tuning.Pulls.SoloGauntletReadiness);
            f("Pulls.SoloGauntletWinPulls", tuning.Pulls.SoloGauntletWinPulls);
            f("Pulls.GauntletSupplies", tuning.Pulls.GauntletSupplies);
            f("Pulls.SoloGauntletControl", tuning.Pulls.SoloGauntletControl);
            f("Pulls.SoloGauntletControlMax", tuning.Pulls.SoloGauntletControlMax);
            f("Pulls.ArriveMinMs", tuning.Pulls.ArriveMinMs);
            f("Pulls.ArriveMaxMs", tuning.Pulls.ArriveMaxMs);
            f("Pulls.ArriveShrinkMs", tuning.Pulls.ArriveShrinkMs);
            f("Pulls.ArriveFloorMs", tuning.Pulls.ArriveFloorMs);
            f("Pulls.NextPullShrinkMs", tuning.Pulls.NextPullShrinkMs);
            f("Pulls.NextPullFloorMs", tuning.Pulls.NextPullFloorMs);
            f("Pulls.OwnerClearScale", tuning.Pulls.OwnerClearScale);

            f("Owner.LevelSpread", tuning.Owner.LevelSpread);
            f("Owner.TankChance", tuning.Owner.TankChance);
            f("Owner.HealerChance", tuning.Owner.HealerChance);
            f("Owner.DamageTakenDps", tuning.Owner.DamageTakenDps);
            f("Owner.DamageTakenProtector", tuning.Owner.DamageTakenProtector);
            f("Owner.TankOwnerDamageShare", tuning.Owner.TankOwnerDamageShare);
            f("Owner.Healing", tuning.Owner.Healing);
            f("Owner.TankDamageRefund", tuning.Owner.TankDamageRefund);
            f("Owner.TankHold", tuning.Owner.TankHold);
            f("Owner.TankLose", tuning.Owner.TankLose);
            f("Owner.PulledThreat", tuning.Owner.PulledThreat);
            f("Owner.SoloFight", tuning.Owner.SoloFight);
            f("Owner.FollowFar", tuning.Owner.FollowFar);
            f("Owner.FollowNear", tuning.Owner.FollowNear);
            f("Owner.FollowFarDistance", tuning.Owner.FollowFarDistance);
            f("Owner.FollowNearDistance", tuning.Owner.FollowNearDistance);
            f("Owner.Death", tuning.Owner.Death);

            f("Resurrection.GraceMs", tuning.Resurrection.GraceMs);
            f("Resurrection.ReviveAlly", tuning.Resurrection.ReviveAlly);

            f("Opponent.LevelSpread", tuning.Opponent.LevelSpread);
            f("Opponent.EngageMaxMs", tuning.Opponent.EngageMaxMs);
            f("Opponent.HealerChance", tuning.Opponent.HealerChance);
            f("Opponent.TankChance", tuning.Opponent.TankChance);

            f("Ambush.MinMs", tuning.Ambush.MinMs);
            f("Ambush.MaxMs", tuning.Ambush.MaxMs);
            f("Ambush.EngageMaxMs", tuning.Ambush.EngageMaxMs);
            f("Ambush.Kill", tuning.Ambush.Kill);

            f("Travel.ObjectiveMin", tuning.Travel.ObjectiveMin);
            f("Travel.ObjectiveMax", tuning.Travel.ObjectiveMax);
            f("Travel.FlyingMin", tuning.Travel.FlyingMin);
            f("Travel.FlyingMax", tuning.Travel.FlyingMax);
            f("Travel.Progress", tuning.Travel.Progress);
            f("Travel.Arrive", tuning.Travel.Arrive);
            f("Travel.FastArrive", tuning.Travel.FastArrive);
            f("Travel.DamageTaken", tuning.Travel.DamageTaken);
            f("Travel.Death", tuning.Travel.Death);
            f("Travel.StepCost", tuning.Travel.StepCost);

            f("Flag.BaseMin", tuning.Flag.BaseMin);
            f("Flag.BaseMax", tuning.Flag.BaseMax);
            f("Flag.CapturesToWin", tuning.Flag.CapturesToWin);
            f("Flag.RespawnMs", tuning.Flag.RespawnMs);
            f("Flag.DroppedReturnMs", tuning.Flag.DroppedReturnMs);
            f("Flag.TouchDistance", tuning.Flag.TouchDistance);
            f("Flag.Capture", tuning.Flag.Capture);
            f("Flag.Pickup", tuning.Flag.Pickup);
            f("Flag.Return", tuning.Flag.Return);
            f("Flag.CarrierKill", tuning.Flag.CarrierKill);
            f("Flag.Lost", tuning.Flag.Lost);
            f("Flag.Progress", tuning.Flag.Progress);
            f("Flag.Death", tuning.Flag.Death);
            f("Flag.StepCost", tuning.Flag.StepCost);

            f("ScriptedPlayers.SpellMinMs", tuning.ScriptedPlayers.SpellMinMs);
            f("ScriptedPlayers.SpellMaxMs", tuning.ScriptedPlayers.SpellMaxMs);
            f("ScriptedPlayers.HealMinMs", tuning.ScriptedPlayers.HealMinMs);
            f("ScriptedPlayers.HealMaxMs", tuning.ScriptedPlayers.HealMaxMs);
            f("ScriptedPlayers.WanderMinMs", tuning.ScriptedPlayers.WanderMinMs);
            f("ScriptedPlayers.WanderMaxMs", tuning.ScriptedPlayers.WanderMaxMs);
            f("ScriptedPlayers.RegenFraction", tuning.ScriptedPlayers.RegenFraction);
            f("ScriptedPlayers.HealBelow", tuning.ScriptedPlayers.HealBelow);
            f("ScriptedPlayers.SelfHealBelow", tuning.ScriptedPlayers.SelfHealBelow);
            f("ScriptedPlayers.HealerRange", tuning.ScriptedPlayers.HealerRange);
            f("ScriptedPlayers.TauntRange", tuning.ScriptedPlayers.TauntRange);
            f("ScriptedPlayers.RangedMin", tuning.ScriptedPlayers.RangedMin);
            f("ScriptedPlayers.RangedMax", tuning.ScriptedPlayers.RangedMax);
            f("ScriptedPlayers.StealthChance", tuning.ScriptedPlayers.StealthChance);
            f("ScriptedPlayers.TacticsChance", tuning.ScriptedPlayers.TacticsChance);
            f("ScriptedPlayers.ControlMinMs", tuning.ScriptedPlayers.ControlMinMs);
            f("ScriptedPlayers.ControlMaxMs", tuning.ScriptedPlayers.ControlMaxMs);
            f("ScriptedPlayers.DefensiveBelow", tuning.ScriptedPlayers.DefensiveBelow);
            f("ScriptedPlayers.BreakBelow", tuning.ScriptedPlayers.BreakBelow);
        }

        /// The values of the config keys <prefix><key>, each defaulting to the value above; min/max pairs are
        /// ordered.
        [[nodiscard]] static CurriculumTuning Load(std::string const& prefix);

        /// Every value as a JSON object, keys as in the config.
        [[nodiscard]] boost::json::object Json() const;
    };
}

#endif

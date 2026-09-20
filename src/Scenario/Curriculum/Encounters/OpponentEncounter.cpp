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

#include "Encounters.h"
#include "IncomingSpell.h"
#include "BotAccounts.h"
#include "CombatReward.h"
#include "Env.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "StringFormat.h"
#include <algorithm>

float Animus::Curriculum::PreventedScale(float heal, float area, float longCast, uint8 prevented)
{
    switch (IncomingSpell::Prevented(prevented))
    {
        case IncomingSpell::Prevented::Heal:    return heal;
        case IncomingSpell::Prevented::Area:    return area;
        case IncomingSpell::Prevented::Long:    return longCast;
        default:                                return 1.0f;
    }
}

using Animus::Curriculum::EnemyPlayers::MakeEnemies;

Animus::Curriculum::OpponentEncounter::OpponentEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::OpponentEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken, RewardTerm::Casting,
        RewardTerm::Approach, RewardTerm::StealthOpener, RewardTerm::StealthUtility, RewardTerm::Kill,
        RewardTerm::HealthKept, RewardTerm::Death, RewardTerm::Interrupt };
}

/// An interrupt counts when the opponent it was cast at had its cast cut short since, and is paid by what it stopped
/// (IncomingSpell::Prevented) at the duel's weights, which CombatReward::OneOnOne already uses here.
///
/// A scripted enemy player casts and heals -- stage14_pvp at 13.7M saw 3.0 interruptible casts an episode -- so
/// stopping one is worth at least what it is worth against a creature. Until now nothing in a PvP stage paid for an
/// interrupt or counted one: the term belonged to the pulls and the duel, so the seats arrived with cast identity,
/// threat and the hold_interrupt option and no reason to use any of them.
void Animus::Curriculum::OpponentEncounter::TrackInterrupt(Env& env, uint32 seat, Unit const* opponent,
    RewardLedger& ledger)
{
    EnvOpponent& state = _envs[env.Index];
    if (seat >= state.PendingInterrupt.size())
        return;

    // Held out of the fight: stunned, feared, rooted away, polymorphed. Counted as the pulls count it, per decision.
    if (opponent && PullsEncounter::Controlled(opponent))
        state.ControlMs[seat] += _scenario.DecisionMs();

    ObjectGuid& pending = state.PendingInterrupt[seat];
    if (pending.IsEmpty())
        return;

    auto const stopped = std::find_if(env.StepInterruptedTargets.begin(), env.StepInterruptedTargets.end(),
        [&pending](Env::InterruptedCast const& cast) { return cast.Caster == pending; });
    if (stopped != env.StepInterruptedTargets.end())
    {
        CurriculumTuning::DuelTuning const& duel = _scenario.Tuning().Duel;
        ledger.Add(RewardTerm::Interrupt, duel.Interrupt
            * PreventedScale(duel.InterruptHeal, duel.InterruptArea, duel.InterruptLong, stopped->Prevented));
        ++state.Interrupts[seat];
    }

    pending.Clear();
}

void Animus::Curriculum::OpponentEncounter::OnSeatAction(Env& env, uint32 seat, SeatActionResult const& result)
{
    EnvOpponent& state = _envs[env.Index];
    if (!result.PendingInterrupt.IsEmpty() && seat < state.PendingInterrupt.size())
        state.PendingInterrupt[seat] = result.PendingInterrupt;
}

void Animus::Curriculum::OpponentEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("interrupts", [this](Env const& env, uint32 seat)
    {
        EnvOpponent const& state = _envs[env.Index];
        return seat < state.Interrupts.size() ? float(state.Interrupts[seat]) : 0.0f;
    });
    table.Add("control_seconds", [this](Env const& env, uint32 seat)
    {
        EnvOpponent const& state = _envs[env.Index];
        return seat < state.ControlMs.size() ? float(state.ControlMs[seat]) / 1000.0f : 0.0f;
    });

    table.Add("won", [this](Env const& env, uint32 seat)
    {
        CombatTally const& tally = _scenario.Data(env).Seats[seat].Combat;
        return _scenario.Uses(env, *this) && tally.Killed && !tally.Died ? 1.0f : 0.0f;
    });

    // Every seat of the stage has a row: in a stage that also has party arenas, seats 2 and 3 of a mirror episode are
    // empty and have no other side.
    table.Add("opponent_class", [this](Env const& env, uint32 seat)
    {
        if (!Mirror(env))
            return float(_envs[env.Index].Class);
        if (seat > 1)
            return 0.0f;
        SeatState const& other = _scenario.Data(env).Seats[1 - seat];
        return other.L ? float(other.L->Profile->Class) : 0.0f;
    });

    table.Add("opponent_role", [this](Env const& env, uint32 seat)
    {
        if (!Mirror(env))
            return float(uint32(_envs[env.Index].PlayRole));
        if (seat > 1)
            return 0.0f;
        SeatState const& other = _scenario.Data(env).Seats[1 - seat];
        return other.L ? float(uint32(other.L->PlayRole())) : 0.0f;
    });
}

uint32 Animus::Curriculum::OpponentEncounter::EnemySeats(Env const& env, uint32 seat,
    std::array<uint32, PACK_SLOTS>& out) const
{
    out.fill(NO_SEAT);

    EnvState const& data = _scenario.Data(env);
    uint32 const side = _scenario.SideOf(env, seat);
    uint32 count = 0;
    for (uint32 other = 0; other < data.ActiveSeats && count < PACK_SLOTS; ++other)
        if (_scenario.SideOf(env, other) != side && _scenario.SeatBot(env, other))
            out[count++] = other;

    return count;
}

Player* Animus::Curriculum::OpponentEncounter::Find(Env const& env, uint32 seat) const
{
    if (Mirror(env))
    {
        // The enemy this seat has selected. One a side leaves a single choice, which is the other seat as it
        // always was; a side of several makes it whichever of them target selection last picked.
        std::array<uint32, PACK_SLOTS> enemies{};
        uint32 const count = EnemySeats(env, seat, enemies);
        if (!count)
            return nullptr;

        EnvState const& data = _scenario.Data(env);
        uint32 const slot = seat < data.Seats.size() ? data.Seats[seat].TargetSlot : 0;
        if (Player* chosen = slot < count ? _scenario.SeatBot(env, enemies[slot]) : nullptr;
            chosen && chosen->IsAlive())
            return chosen;

        // Its choice is down or gone: the first of that side still standing, so the seat keeps an opponent to be
        // scored against rather than dropping out of the fight for the rest of the episode.
        for (uint32 other = 0; other < count; ++other)
            if (Player* bot = _scenario.SeatBot(env, enemies[other]); bot && bot->IsAlive())
                return bot;

        return _scenario.SeatBot(env, enemies[0]);
    }

    Player* opponent = _envs[env.Index].Bot.Active();
    return opponent && opponent->IsInWorld() ? opponent : nullptr;
}

bool Animus::Curriculum::OpponentEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);

    EnvOpponent& state = _envs[env.Index];
    state.PendingInterrupt.fill(ObjectGuid::Empty);
    state.Interrupts.fill(0);
    state.ControlMs.fill(0);

    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        _scenario.PrepareFighter(_scenario.SeatBot(env, seat), data.Seats[seat]);

    if (Mirror(env))
    {
        // A scripted opponent of an earlier episode (a stage mixing both kinds) has no place here.
        _envs[env.Index].Bot.Destroy();

        // Everyone on one side is an enemy of everyone on the other, not only the pair that shares an index:
        // a seat has to be able to hit whichever of them it chooses, or is told to.
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            for (uint32 other = seat + 1; other < data.ActiveSeats; ++other)
                if (_scenario.SideOf(env, seat) != _scenario.SideOf(env, other))
                    if (Player* bot = _scenario.SeatBot(env, seat); bot)
                        if (Player* enemy = _scenario.SeatBot(env, other); enemy)
                            MakeEnemies(bot, enemy);

        // Nobody has chosen yet: the first of the enemy side, which is all there is when it holds one seat.
        for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
            data.Seats[seat].TargetSlot = 0;

        return true;
    }

    if (!RebuildScripted(env, _scenario.SeatBot(env, 0), map))
        return false;

    env.Targets = { Find(env, 0)->GetGUID() };
    return true;
}

bool Animus::Curriculum::OpponentEncounter::RebuildScripted(Env& env, Player* bot, Map* map)
{
    EnvOpponent& opponent = _envs[env.Index];
    CurriculumTuning::OpponentTuning const& tuning = _scenario.Tuning().Opponent;

    uint8 const level = uint8(std::clamp<int32>(int32(_scenario.Data(env).Seats[0].Level)
        + irand(-tuning.LevelSpread, tuning.LevelSpread), 1, DEFAULT_MAX_LEVEL));

    uint32 const index = env.Id;
    EnemyPlayers::Naming const naming{
        [index](uint8 session) { return Acore::StringFormat("Foe{}{}", index, session ? "b" : "a"); },
        [index](uint8 session) { return BotAccounts::Opponent(index, session); },
    };

    EnemyPlayers::Spawned const spawned = EnemyPlayers::Create(opponent.Bot, naming, level, tuning, bot, map,
        _scenario.SpawnMapId(), opponent.Script);
    if (!spawned.Bot)
        return false;

    opponent.Script.EngageMs = env.EpisodeElapsedMs + urand(0, tuning.EngageMaxMs);
    MakeEnemies(bot, spawned.Bot);
    opponent.Class = spawned.Class;
    opponent.PlayRole = spawned.PlayRole;
    return true;
}

void Animus::Curriculum::OpponentEncounter::Update(Env& env)
{
    // Zone updates can drop the PvP flag; the fight needs it on every seat, not only the first.
    if (Mirror(env))
    {
        uint32 const seats = _scenario.Data(env).ActiveSeats;
        for (uint32 seat = 0; seat < seats; ++seat)
        {
            Player* bot = _scenario.SeatBot(env, seat);
            Player* opponent = Find(env, seat);
            if (bot && opponent && (!bot->IsPvP() || !opponent->IsPvP()))
                MakeEnemies(bot, opponent);
        }
        return;
    }

    Player* bot = env.FindBot(0);
    Player* opponent = Find(env, 0);
    if (!bot || !opponent)
        return;

    if (!bot->IsPvP() || !opponent->IsPvP())
        MakeEnemies(bot, opponent);

    ScriptedPlayer::UpdateOpponent(opponent, bot, env.EpisodeElapsedMs, _envs[env.Index].Script,
        _scenario.Tuning().ScriptedPlayers);
}

bool Animus::Curriculum::OpponentEncounter::SelectTarget(Env const& env, uint32 seat, Unit*& target)
{
    target = Find(env, seat);
    return true;
}

void Animus::Curriculum::OpponentEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    view.Opponent = Find(env, seat);
    view.Mirror = Mirror(env);

    if (Mirror(env))
    {
        // The side it fights, as slots it can select between. Without this the seat sees no enemies at all --
        // the env's target list is for spawned packs -- so every target-selection action stays masked and there
        // is nothing an order to focus one of them could ask for.
        std::array<uint32, PACK_SLOTS> enemies{};
        uint32 const count = EnemySeats(env, seat, enemies);

        EnvState const& data = _scenario.Data(env);
        for (uint32 slot = 0; slot < count; ++slot)
            view.Enemies[slot] = _scenario.SeatBot(env, enemies[slot]);
        for (uint32 slot = count; slot < PACK_SLOTS; ++slot)
            view.Enemies[slot] = nullptr;
        view.EnemyCount = count;
        view.TargetSlot = std::min(view.TargetSlot, count ? count - 1 : 0u);

        uint32 const chosen = view.TargetSlot < count ? enemies[view.TargetSlot] : NO_SEAT;
        SeatState const* other = chosen != NO_SEAT ? &data.Seats[chosen] : nullptr;
        view.OpponentClass = other && other->L ? other->L->Profile->Class : 0;
        view.OpponentRole = other && other->L ? other->L->PlayRole() : Role::Dps;
        return;
    }

    view.OpponentClass = _envs[env.Index].Class;
    view.OpponentRole = _envs[env.Index].PlayRole;
}

void Animus::Curriculum::OpponentEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    // A flag match pays for the flags (FlagEncounter), not for a one-on-one's single kill.
    if (Flag(env))
        return;

    // No opponent in the world (a far teleport, a failed rebuild): nothing to score, not even the step cost.
    if (Player* opponent = Find(env, seat); bot && opponent)
    {
        CombatReward::OneOnOne(_scenario, env, seat, bot, opponent, ledger);
        TrackInterrupt(env, seat, opponent, ledger);
    }
}

bool Animus::Curriculum::OpponentEncounter::IsTerminal(Env const& env) const
{
    // In a flag match the dead stand up again at their base; the flags end it.
    if (Flag(env))
        return false;

    EnvState const& data = _scenario.Data(env);
    if (Mirror(env))
    {
        // A side is out when every seat on it is down. One seat a side makes that the first death, as before.
        for (uint32 side = 0; side < TEAM_COUNT; ++side)
        {
            bool held = false, standing = false;
            for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
                if (_scenario.SideOf(env, seat) == side)
                {
                    held = true;
                    standing = standing || !data.Seats[seat].Combat.Died;
                }

            if (held && !standing)
                return true;
        }

        return false;
    }

    return data.Seats[0].Combat.Died || data.Seats[0].Combat.Killed;
}

void Animus::Curriculum::OpponentEncounter::Deactivate(Env& env)
{
    Teardown(env);
    _envs[env.Index].Class = 0;
    _envs[env.Index].PlayRole = Role::Dps;
}

void Animus::Curriculum::OpponentEncounter::Teardown(Env& env)
{
    // Nothing to destroy in self-play: the slot is empty then.
    _envs[env.Index].Bot.Destroy();
    env.Targets.clear();
}

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
        RewardTerm::HealthKept, RewardTerm::Death };
}

void Animus::Curriculum::OpponentEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
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

Player* Animus::Curriculum::OpponentEncounter::Find(Env const& env, uint32 seat) const
{
    if (Mirror(env))
        return env.FindBot(1 - seat);

    Player* opponent = _envs[env.Index].Bot.Active();
    return opponent && opponent->IsInWorld() ? opponent : nullptr;
}

bool Animus::Curriculum::OpponentEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);

    for (uint32 seat = 0; seat < data.ActiveSeats; ++seat)
        _scenario.PrepareFighter(_scenario.SeatBot(env, seat), data.Seats[seat]);

    if (Mirror(env))
    {
        // A scripted opponent of an earlier episode (a stage mixing both kinds) has no place here.
        _envs[env.Index].Bot.Destroy();
        MakeEnemies(_scenario.SeatBot(env, 0), _scenario.SeatBot(env, 1));
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
    Player* bot = env.FindBot(0);
    Player* opponent = Find(env, 0);
    if (!bot || !opponent)
        return;

    // Zone updates can drop the PvP flag; the fight needs it.
    if (!bot->IsPvP() || !opponent->IsPvP())
        MakeEnemies(bot, opponent);

    if (!Mirror(env))
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
        SeatState const& other = _scenario.Data(env).Seats[1 - seat];
        view.OpponentClass = other.L ? other.L->Profile->Class : 0;
        view.OpponentRole = other.L ? other.L->PlayRole() : Role::Dps;
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
        CombatReward::OneOnOne(_scenario, env, seat, bot, opponent, ledger);
}

bool Animus::Curriculum::OpponentEncounter::IsTerminal(Env const& env) const
{
    // In a flag match the dead stand up again at their base; the flags end it.
    if (Flag(env))
        return false;

    EnvState const& data = _scenario.Data(env);
    if (Mirror(env))
        return data.Seats[0].Combat.Died || data.Seats[1].Combat.Died;

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

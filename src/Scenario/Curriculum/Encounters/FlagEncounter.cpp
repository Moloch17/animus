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
#include "BotFactory.h"
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "Map.h"
#include "Player.h"
#include "SeatView.h"

namespace
{
    using State = Animus::Curriculum::SeatView::FlagState;
}

Animus::Curriculum::FlagEncounter::FlagEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::FlagEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::FlagCapture, RewardTerm::FlagPickup, RewardTerm::FlagReturn,
        RewardTerm::CarrierKill, RewardTerm::FlagLost, RewardTerm::Progress, RewardTerm::Death };
}

void Animus::Curriculum::FlagEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    auto const side = [this](Env const& env, uint32 seat) -> Side const&
    {
        return _envs[env.Index].Sides[seat < 2 ? seat : 0];
    };

    table.Add("flag_captures", [side](Env const& env, uint32 seat) { return float(side(env, seat).Captures); });
    table.Add("flag_pickups", [side](Env const& env, uint32 seat) { return float(side(env, seat).Pickups); });
    table.Add("flag_returns", [side](Env const& env, uint32 seat) { return float(side(env, seat).Returns); });
    table.Add("carrier_kills", [side](Env const& env, uint32 seat) { return float(side(env, seat).CarrierKills); });
    table.Add("flag_deaths", [side](Env const& env, uint32 seat) { return float(side(env, seat).Deaths); });
    table.Add("match_won", [this, side](Env const& env, uint32 seat)
    {
        return seat < 2 && side(env, seat).Captures >= _scenario.Tuning().Flag.CapturesToWin ? 1.0f : 0.0f;
    });
}

void Animus::Curriculum::FlagEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index] = EnvFlags();
}

bool Animus::Curriculum::FlagEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    CurriculumTuning::FlagTuning const& tuning = _scenario.Tuning().Flag;
    EnvFlags& flags = _envs[env.Index];
    Player* first = _scenario.SeatBot(env, 0);
    Player* second = _scenario.SeatBot(env, 1);
    if (!first || !second || !map)
        return false;

    // The first seat's base is where it stands; the other's a walk away, where it goes now.
    flags.Sides[0].Base.Relocate(first);
    if (!TravelEncounter::FindPlace(first, map, tuning.BaseMin, tuning.BaseMax, false, flags.Sides[1].Base))
        return false;

    flags.Sides[1].Base.SetOrientation(flags.Sides[1].Base.GetAngle(&flags.Sides[0].Base));
    if (!BotFactory::TeleportWithinMap(second, flags.Sides[1].Base))
        return false;

    flags.Built = true;
    return true;
}

void Animus::Curriculum::FlagEncounter::Update(Env& env)
{
    EnvFlags& flags = _envs[env.Index];
    if (!flags.Built)
        return;

    CurriculumTuning::FlagTuning const& tuning = _scenario.Tuning().Flag;
    uint32 const now = env.EpisodeElapsedMs;

    for (uint32 seat = 0; seat < 2; ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        Side& own = flags.Sides[seat];
        Side& enemy = flags.Sides[1 - seat];
        if (!bot)
            continue;

        bool const carrying = enemy.State == State::Carried;

        // Death: a carrier drops the flag where it fell, and the other side is paid for stopping it.
        if (!bot->IsAlive())
        {
            if (!own.Dead)
            {
                own.Dead = true;
                own.RespawnMs = now + tuning.RespawnMs;
                ++own.Deaths;
                ++own.StepDeaths;
                if (carrying)
                {
                    enemy.State = State::Dropped;
                    enemy.Dropped.Relocate(bot);
                    enemy.DroppedMs = now;
                    ++enemy.CarrierKills;
                    ++enemy.StepCarrierKills;
                }
            }
            else if (now >= own.RespawnMs)
            {
                // A graveyard wave: back at the base, whole.
                bot->ResurrectPlayer(1.0f);
                bot->SpawnCorpseBones();
                BotFactory::TeleportWithinMap(bot, own.Base);
                own.Dead = false;
                own.LastDistance = -1.0f;
            }
            continue;
        }

        // A carrier cannot ride.
        if (carrying && bot->IsMounted())
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);

        auto const touches = [bot, &tuning](Position const& place)
        {
            return bot->GetExactDist2d(&place) <= tuning.TouchDistance;
        };

        // Its own flag lying on the ground: return it.
        if (own.State == State::Dropped && touches(own.Dropped))
        {
            own.State = State::AtBase;
            ++own.Returns;
            ++own.StepReturns;
        }

        // The other side's flag, at its base or dropped: take it.
        if ((enemy.State == State::AtBase && touches(enemy.Base))
            || (enemy.State == State::Dropped && touches(enemy.Dropped)))
        {
            enemy.State = State::Carried;
            ++own.Pickups;
            ++own.StepPickups;
            if (bot->IsMounted())
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        }

        // Home with it while its own flag is there: a capture.
        if (enemy.State == State::Carried && own.State == State::AtBase && touches(own.Base))
        {
            enemy.State = State::AtBase;
            ++own.Captures;
            ++own.StepCaptures;
            ++enemy.StepLost;
        }
    }

    // A dropped flag nobody touched goes home.
    for (Side& side : flags.Sides)
        if (side.State == State::Dropped && now >= side.DroppedMs + tuning.DroppedReturnMs)
            side.State = State::AtBase;
}

Animus::Curriculum::FlagEncounter::Goal Animus::Curriculum::FlagEncounter::CurrentGoal(Env const& env, uint32 seat,
    Position& place) const
{
    EnvFlags const& flags = _envs[env.Index];
    Side const& own = flags.Sides[seat];
    Side const& enemy = flags.Sides[1 - seat];

    if (enemy.State == State::Carried)
    {
        place = own.Base;
        return Goal::CaptureHome;
    }

    if (own.State == State::Dropped)
    {
        place = own.Dropped;
        return Goal::ReturnOwn;
    }

    if (enemy.State == State::AtBase)
    {
        // With its own flag carried off, stopping the carrier comes first.
        if (own.State == State::Carried)
            if (Player* carrier = _scenario.SeatBot(env, 1 - seat); carrier && carrier->IsAlive())
            {
                place.Relocate(carrier);
                return Goal::ChaseCarrier;
            }

        place = enemy.Base;
        return Goal::TakeEnemy;
    }

    place = enemy.Dropped;
    return Goal::PickUpEnemy;
}

void Animus::Curriculum::FlagEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    EnvFlags const& flags = _envs[env.Index];
    if (!flags.Built || seat > 1)
        return;

    Side const& own = flags.Sides[seat];
    Side const& enemy = flags.Sides[1 - seat];

    SeatView::FlagMatch& match = view.Flags;
    match.Active = true;
    match.Own = own.State;
    match.Enemy = enemy.State;
    match.OwnBase = own.Base;
    match.EnemyBase = enemy.Base;
    match.OwnDropped = own.Dropped;
    match.EnemyDropped = enemy.Dropped;
    match.OwnScore = own.Captures;
    match.EnemyScore = enemy.Captures;

    view.HasObjective = CurrentGoal(env, seat, view.Objective) != Goal::None;
}

void Animus::Curriculum::FlagEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::FlagTuning const& tuning = _scenario.Tuning().Flag;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    EnvFlags& flags = _envs[env.Index];
    if (!flags.Built || seat > 1)
        return;

    Side& side = flags.Sides[seat];
    ledger.Add(RewardTerm::FlagCapture, tuning.Capture * float(side.StepCaptures));
    ledger.Add(RewardTerm::FlagPickup, tuning.Pickup * float(side.StepPickups));
    ledger.Add(RewardTerm::FlagReturn, tuning.Return * float(side.StepReturns));
    ledger.Add(RewardTerm::CarrierKill, tuning.CarrierKill * float(side.StepCarrierKills));
    ledger.Add(RewardTerm::FlagLost, -tuning.Lost * float(side.StepLost));
    ledger.Add(RewardTerm::Death, -tuning.Death * float(side.StepDeaths));
    side.StepCaptures = side.StepPickups = side.StepReturns = side.StepCarrierKills = side.StepLost = 0;
    side.StepDeaths = 0;

    // Potential shaping toward the current goal; a new goal starts it over, so a flag changing hands pays nothing.
    Position place;
    Goal const goal = CurrentGoal(env, seat, place);
    if (!bot || !bot->IsAlive() || goal != side.LastGoal)
    {
        side.LastGoal = goal;
        side.LastDistance = bot && bot->IsAlive() ? bot->GetExactDist2d(&place) : -1.0f;
        return;
    }

    float const distance = bot->GetExactDist2d(&place);
    if (side.LastDistance >= 0.0f)
        ledger.Add(RewardTerm::Progress, tuning.Progress * (side.LastDistance - distance) / 100.0f);
    side.LastDistance = distance;
}

bool Animus::Curriculum::FlagEncounter::IsTerminal(Env const& env) const
{
    EnvFlags const& flags = _envs[env.Index];
    uint32 const toWin = _scenario.Tuning().Flag.CapturesToWin;
    return flags.Sides[0].Captures >= toWin || flags.Sides[1].Captures >= toWin;
}

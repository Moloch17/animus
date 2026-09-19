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
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "TravelBlock.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr uint32 OBJECTIVE_ATTEMPTS = 32;
    constexpr float MAX_PATH_DETOUR = 1.8f;         // a path at most this many times the straight distance
    constexpr float HEIGHT_SEARCH = 120.0f;
    constexpr float BODY_HEIGHT = 2.0f;             // for the water check
}

Animus::Curriculum::TravelEncounter::TravelEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::TravelEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::Progress, RewardTerm::Arrive, RewardTerm::DamageTaken,
        RewardTerm::Death };
}

void Animus::Curriculum::TravelEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("arrived", [this](Env const& env, uint32) { return _envs[env.Index].Arrived ? 1.0f : 0.0f; });
    table.Add("travel_seconds", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return float(travel.Arrived ? travel.ArriveMs : env.EpisodeElapsedMs) / 1000.0f;
    });
    table.Add("start_distance", [this](Env const& env, uint32) { return _envs[env.Index].StartDistance; });
    table.Add("walk_distance", [this](Env const& env, uint32) { return _envs[env.Index].WalkDistance; });
    // Why a flying arena does or does not fly, in three steps: does the seat know a flying mount, would the mask
    // have offered it at the start, and did the seat ride one (in the air or not, unlike flying_fraction).
    table.Add("knows_flying_mount", [this](Env const& env, uint32)
    {
        return _envs[env.Index].KnowsFlyer ? 1.0f : 0.0f;
    });
    table.Add("could_mount_flying", [this](Env const& env, uint32)
    {
        return _envs[env.Index].CouldMountFlyer ? 1.0f : 0.0f;
    });
    table.Add("area_before", [this](Env const& env, uint32) { return float(_envs[env.Index].AreaBefore); });
    table.Add("area_after", [this](Env const& env, uint32) { return float(_envs[env.Index].AreaAfter); });
    table.Add("flyer_refusal_before", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].FlyerRefusalBefore);
    });
    table.Add("flyer_refusal_after", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].FlyerRefusalAfter);
    });
    table.Add("flying_mount_fraction", [this](Env const& env, uint32)
    {
        return env.EpisodeElapsedMs ? float(_envs[env.Index].FlyingMountMs) / float(env.EpisodeElapsedMs) : 0.0f;
    });
    // What the trip beat the walk by: 0 walked it, 0.3 arrived in 70% of the time walking would have taken.
    table.Add("saved", [this](Env const& env, uint32) { return Saved(_envs[env.Index]); });
    table.Add("mounted_fraction", [this](Env const& env, uint32)
    {
        return env.EpisodeElapsedMs ? float(_envs[env.Index].MountedMs) / float(env.EpisodeElapsedMs) : 0.0f;
    });
    table.Add("flying_fraction", [this](Env const& env, uint32)
    {
        return env.EpisodeElapsedMs ? float(_envs[env.Index].FlyingMs) / float(env.EpisodeElapsedMs) : 0.0f;
    });
}

float Animus::Curriculum::TravelEncounter::Saved(EnvTravel const& travel)
{
    if (!travel.Arrived || travel.WalkDistance <= 0.0f)
        return 0.0f;

    float const walkSeconds = travel.WalkDistance / TravelBlock::BASE_RUN_SPEED;
    float const tripSeconds = float(travel.ArriveMs) / 1000.0f;
    return std::clamp((walkSeconds - tripSeconds) / walkSeconds, 0.0f, 1.0f);
}

void Animus::Curriculum::TravelEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index] = EnvTravel();
}

bool Animus::Curriculum::TravelEncounter::FindPlace(Player* bot, Map* map, float nearest, float furthest, bool flying,
    Position& place, float* walk)
{
    for (uint32 attempt = 0; attempt < OBJECTIVE_ATTEMPTS; ++attempt)
    {
        // Later attempts settle for shorter trips rather than failing the episode.
        float const reach = attempt < OBJECTIVE_ATTEMPTS / 2 ? furthest : (nearest + furthest) * 0.5f;
        float const distance = frand(nearest, std::max(nearest, reach));
        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const x = bot->GetPositionX() + distance * std::cos(angle);
        float const y = bot->GetPositionY() + distance * std::sin(angle);

        map->LoadGrid(x, y);
        float const z = map->GetHeight(bot->GetPhaseMask(), x, y, bot->GetPositionZ() + HEIGHT_SEARCH * 0.5f, true,
            HEIGHT_SEARCH);
        if (z <= INVALID_HEIGHT || map->IsInWater(bot->GetPhaseMask(), x, y, z, BODY_HEIGHT))
            continue;

        // On the ground it has to be reachable on foot, by a path not much longer than the straight line.
        float walked = distance;
        if (!flying)
        {
            PathGenerator path(bot);
            if (!path.CalculatePath(x, y, z) || !(path.GetPathType() & PATHFIND_NORMAL)
                || path.getPathLength() > distance * MAX_PATH_DETOUR)
                continue;

            walked = path.getPathLength();
        }

        place.Relocate(x, y, z);
        if (walk)
            *walk = walked;
        return true;
    }

    return false;
}

bool Animus::Curriculum::TravelEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);
    Player* bot = _scenario.SeatBot(env, 0);
    EnvTravel& travel = _envs[env.Index];
    if (!bot || !map)
        return false;

    CurriculumTuning::TravelTuning const& tuning = _scenario.Tuning().Travel;
    bool const flying = _scenario.Arena(env).Flying;
    float walk = 0.0f;
    if (!FindPlace(bot, map, flying ? tuning.FlyingMin : tuning.ObjectiveMin,
        flying ? tuning.FlyingMax : tuning.ObjectiveMax, flying, travel.Objective, &walk))
        return false;

    travel.HasObjective = true;
    travel.StartDistance = bot->GetExactDist2d(&travel.Objective);
    travel.WalkDistance = walk > 0.0f ? walk : travel.StartDistance;
    travel.KnowsFlyer = TravelBlock::FlyingMount(bot) != nullptr;
    // Does the seat get judged in the area it is standing in? Read the cached area and the flying mount's refusal,
    // refresh the position data the way a map update would, then read both again.
    travel.AreaBefore = bot->GetAreaId();
    travel.FlyerRefusalBefore = TravelBlock::FlyingMountRefusal(bot);
    bot->UpdatePositionData();
    travel.AreaAfter = bot->GetAreaId();
    travel.FlyerRefusalAfter = TravelBlock::FlyingMountRefusal(bot);
    travel.CouldMountFlyer = TravelBlock::CanSummonFlying(bot);
    _scenario.PrepareFighter(bot, data.Seats[0]);
    return true;
}

bool Animus::Curriculum::TravelEncounter::SelectTarget(Env const& /*env*/, uint32 /*seat*/, Unit*& target)
{
    // Nothing to fight: the seats act without a target (SeatEncoder::ActsWithoutTarget).
    target = nullptr;
    return true;
}

void Animus::Curriculum::TravelEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    EnvTravel const& travel = _envs[env.Index];
    view.HasObjective = travel.HasObjective;
    view.Objective = travel.Objective;
}

void Animus::Curriculum::TravelEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    CurriculumTuning::TravelTuning const& tuning = _scenario.Tuning().Travel;
    ledger.Add(RewardTerm::StepCost, -tuning.StepCost * _scenario.DecisionScale());

    EnvTravel& travel = _envs[env.Index];
    SeatState& seat = _scenario.Data(env).Seats[seatIndex];
    if (!bot || !travel.HasObjective)
        return;

    uint32 const stepMs = env.EpisodeElapsedMs - std::min(env.EpisodeElapsedMs, travel.LastRewardMs);
    travel.LastRewardMs = env.EpisodeElapsedMs;
    if (bot->IsMounted())
        travel.MountedMs += stepMs;
    if (bot->IsMounted() && bot->CanFly())
        travel.FlyingMountMs += stepMs;
    if (bot->IsMounted() && bot->CanFly() && TravelBlock::HeightAboveGround(bot) > 2.0f)
        travel.FlyingMs += stepMs;

    // Potential-based: what is closed pays, what is given back costs, so wandering cannot be farmed.
    float const distance = bot->GetExactDist2d(&travel.Objective);
    if (travel.LastDistance >= 0.0f && !travel.Arrived)
        ledger.Add(RewardTerm::Progress, tuning.Progress * (travel.LastDistance - distance) / 100.0f);
    travel.LastDistance = distance;

    seat.Combat.DamageTaken += env.StepStats[seatIndex].DamageTaken;
    ledger.Add(RewardTerm::DamageTaken, -tuning.DamageTaken * seat.LastStepDamageTaken);

    if (!travel.Arrived && bot->IsAlive() && TravelBlock::AtObjective(bot, travel.Objective))
    {
        travel.Arrived = true;
        travel.ArriveMs = env.EpisodeElapsedMs;
        ledger.Add(RewardTerm::Arrive, tuning.Arrive + tuning.FastArrive * Saved(travel));
    }

    CombatTally& tally = seat.Combat;
    if (!tally.DeathCounted && !bot->IsAlive())
    {
        tally.DeathCounted = true;
        tally.Died = true;
        tally.DeathMs = env.EpisodeElapsedMs;
        ++tally.Deaths;
        ledger.Add(RewardTerm::Death, -tuning.Death);
    }
}

bool Animus::Curriculum::TravelEncounter::IsTerminal(Env const& env) const
{
    return _envs[env.Index].Arrived || _scenario.DeadForGood(env, 0);
}

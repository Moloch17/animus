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
#include "MapDefines.h"
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
    // A water arena wants the detour the others refuse: the way round has to be far enough longer than the way
    // through that swimming is a real choice. Swimming is about 4.7 yd/s against 7 running, so the crossing pays
    // at roughly 1.5x and this leaves a margin on either side of that -- some of these trips are worth swimming
    // and some are not, which is what makes it a decision rather than a reflex.
    constexpr float MIN_DETOUR_ACROSS = 1.35f;
    // Running is 7 yd/s and swimming about 4.7, so the way round has to be this much longer than the way through
    // before swimming it actually saves time. Reported, never required: the arena wants trips on both sides of it.
    constexpr float SWIM_PAYS_ABOVE = 7.0f / 4.7f;
    constexpr uint32 WATER_SAMPLES = 12;            // points along the straight line, looking for water
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
    // How far short the seat finished, in yards. `distance_at_end` is the distance to the *target*, and a
    // movement stage has none by design, so it reads 0 for every episode of the four stages that are only about
    // getting somewhere -- which left no way to tell a seat wedged against geometry forty yards out from one
    // orbiting the objective at eight, never inside the six ARRIVE_YARDS wants. Those are different bugs with
    // different fixes, and this is the column that separates them. 0 on an episode that arrived.
    // What the legs did, against WalkDistance's what-they-were-asked-for. A timeout with distance_travelled near
    // zero is a seat that never moved; one with distance_travelled far above walk_distance is a seat that moved
    // plenty and in the wrong directions. Stuck and lost want opposite fixes.
    table.Add("distance_travelled", [this](Env const& env, uint32) { return _envs[env.Index].Travelled; });
    table.Add("objective_distance_at_end", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        if (!travel.HasObjective || travel.Arrived)
            return 0.0f;

        Player* bot = _scenario.SeatBot(env, 0);
        return bot ? bot->GetExactDist2d(&travel.Objective) : 0.0f;
    });
    // Whether the episode was built as a water crossing at all, and how long the seat spent in the water. The
    // first says what the arena offered, the second what the seat did with it -- and a water arena where
    // swim_seconds stays at zero is either a policy that always goes round or spawn points with no water in
    // reach, which the two columns together tell apart.
    table.Add("crossing", [this](Env const& env, uint32) { return _envs[env.Index].Crossing ? 1.0f : 0.0f; });
    table.Add("swim_seconds", [this](Env const& env, uint32)
    {
        return float(_envs[env.Index].SwimMs) / 1000.0f;
    });
    table.Add("walk_distance", [this](Env const& env, uint32) { return _envs[env.Index].WalkDistance; });
    // Flying against not, split per episode: an update's mean mixes a handful of flights into a hundred rides, so
    // divide each conditional sum by `flew` (or 1 - flew) to read what a trip of that kind actually cost.
    table.Add("flew", [this](Env const& env, uint32) { return _envs[env.Index].Flew ? 1.0f : 0.0f; });
    table.Add("saved_if_flew", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Flew ? Saved(_envs[env.Index]) : 0.0f;
    });
    table.Add("saved_if_ground", [this](Env const& env, uint32)
    {
        return _envs[env.Index].Flew ? 0.0f : Saved(_envs[env.Index]);
    });
    // Does a flying mount actually fly at flight speed, and how high does the seat take it?
    table.Add("flight_speed", [this](Env const& env, uint32) { return _envs[env.Index].FlightSpeedSeen; });
    table.Add("flight_yps", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return travel.FlightMs ? float(travel.FlightDistance / (double(travel.FlightMs) / 1000.0)) : 0.0f;
    });
    table.Add("flight_yps_peak", [this](Env const& env, uint32)
    {
        return _envs[env.Index].FlightPeakYps;
    });
    // Is MOVEMENTFLAG_FLYING actually set while the seat is in the air? 1 = always, 0 = never.
    table.Add("flying_flag_share", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return travel.AloftSteps ? float(travel.AloftFlagged) / float(travel.AloftSteps) : 0.0f;
    });
    table.Add("flight_height", [this](Env const& env, uint32)
    {
        EnvTravel const& travel = _envs[env.Index];
        return travel.HeightSamples ? float(travel.HeightSum / travel.HeightSamples) : 0.0f;
    });
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
    Position& place, float* walk, bool across, float* dry)
{
    // A crossing is a much narrower thing to ask for than a trip -- it wants water on the straight line and a dry
    // way round at least MIN_DETOUR_ACROSS longer -- so it gets more tries before it gives up and the arena falls
    // back to an ordinary trip. At 32 it found one in 0.65 of its episodes; the ones it missed were not bad ground
    // but too few throws at it.
    uint32 const attempts = across ? OBJECTIVE_ATTEMPTS * 4 : OBJECTIVE_ATTEMPTS;
    for (uint32 attempt = 0; attempt < attempts; ++attempt)
    {
        // Later attempts settle for shorter trips rather than failing the episode.
        float const reach = attempt < attempts / 2 ? furthest : (nearest + furthest) * 0.5f;
        float const distance = frand(nearest, std::max(nearest, reach));
        float const angle = frand(0.0f, 2.0f * float(M_PI));
        float const x = bot->GetPositionX() + distance * std::cos(angle);
        float const y = bot->GetPositionY() + distance * std::sin(angle);

        map->LoadGrid(x, y);
        float const z = map->GetHeight(bot->GetPhaseMask(), x, y, bot->GetPositionZ() + HEIGHT_SEARCH * 0.5f, true,
            HEIGHT_SEARCH);
        if (z <= INVALID_HEIGHT)
            continue;

        // The objective itself always stands on dry land -- arriving is standing somewhere, not treading water.
        if (map->IsInWater(bot->GetPhaseMask(), x, y, z, BODY_HEIGHT))
            continue;

        // On the ground it has to be reachable on foot, by a path not much longer than the straight line.
        float walked = distance;
        float dryWalk = 0.0f;
        if (!flying)
        {
            PathGenerator path(bot);
            if (!path.CalculatePath(x, y, z) || !(path.GetPathType() & PATHFIND_NORMAL))
                continue;

            walked = path.getPathLength();

            // A water arena wants the opposite of what every other one wants. Elsewhere a long detour means the
            // straight line was a lie and the objective is rejected; here it is the whole point -- the way round
            // is longer than the way through, and whether the difference is worth swimming for is the lesson. The
            // path is what a runner would cover, so the ratio is the choice the seat is being asked to make.
            if (across)
            {
                // Water is worth getting into only when there is a dry way round and swimming beats it, so the
                // episode has to offer both and know how long each is. The path above is no use for the dry one:
                // PathGenerator::CreateFilter hands a player NAV_GROUND | NAV_WATER | NAV_MAGMA whatever the
                // ground, so the "walk" is allowed to swim and comes back the same length as the straight line
                // (measured over four bodies of water: the longest way round found was 1.15x the way through,
                // even where every straight line crossed water). NAV_GROUND alone is the way round on foot.
                if (!CrossesWater(bot, map, place, x, y))
                    continue;

                PathGenerator dry(bot);
                dry.SetIncludeFlags(NAV_GROUND);
                if (!dry.CalculatePath(x, y, z) || !(dry.GetPathType() & PATHFIND_NORMAL))
                    continue;                      // no dry route: getting in is not a choice, it is the only way

                dryWalk = dry.getPathLength();

                // Measured at the Dustwallow banks: a dry way round of 145 yards against a 75 yard swim
                // (1.94x, well past SWIM_PAYS_ABOVE) next to candidates at 1.02x where walking plainly wins.
                // The floor keeps trips of both kinds, which is what makes the crossing a decision.
                if (dryWalk < distance * MIN_DETOUR_ACROSS)
                    continue;                      // the way round is barely longer: nothing to decide
            }
            else if (walked > distance * MAX_PATH_DETOUR)
                continue;
        }

        place.Relocate(x, y, z);
        if (walk)
            *walk = walked;
        if (dry)
            *dry = dryWalk;
        return true;
    }

    return false;
}

bool Animus::Curriculum::TravelEncounter::CrossesWater(Player const* bot, Map* map, Position const& /*place*/,
    float x, float y)
{
    // Sample the straight line: a detour long enough to be interesting could be a cliff or a canyon as easily as a
    // lake, and only one of those can be swum. Cheap, and only ever asked while an episode is being built.
    float const fromX = bot->GetPositionX();
    float const fromY = bot->GetPositionY();
    float const fromZ = bot->GetPositionZ();
    for (uint32 step = 1; step < WATER_SAMPLES; ++step)
    {
        float const along = float(step) / float(WATER_SAMPLES);
        float const sampleX = fromX + (x - fromX) * along;
        float const sampleY = fromY + (y - fromY) * along;
        map->LoadGrid(sampleX, sampleY);
        float const sampleZ = map->GetHeight(bot->GetPhaseMask(), sampleX, sampleY, fromZ + HEIGHT_SEARCH * 0.5f,
            true, HEIGHT_SEARCH);
        if (sampleZ > INVALID_HEIGHT && map->IsInWater(bot->GetPhaseMask(), sampleX, sampleY, sampleZ, BODY_HEIGHT))
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
    ArenaDefinition const& arena = _scenario.Arena(env);
    bool const flying = arena.Flying;
    float walk = 0.0f;
    // On foot the trip is shorter: the lesson is how well the seat covers ground with what it has, not
    // whether a ride is worth summoning.
    float const least = flying ? tuning.FlyingMin : arena.OnFoot ? tuning.FootMin : tuning.ObjectiveMin;
    float const most = flying ? tuning.FlyingMax : arena.OnFoot ? tuning.FootMax : tuning.ObjectiveMax;
    // A water arena asks for a crossing: an objective whose way round is much longer than the way through, with
    // water in between. Where the ground offers none within reach, fall back to an ordinary trip rather than
    // failing the env -- a scenario that cannot build an episode takes the whole run down with it, and one
    // spawn point without a lake nearby is not a reason to stop training.
    //
    // Falling back silently would be worse than failing, though, because the arena would quietly become a second
    // copy of the open one and still be reported as teaching swimming. So the episode records whether it got a
    // crossing at all (`crossing`), and the stage gates the water arena on the seat actually swimming: a run
    // whose spawn points have no water in reach fails that gate and says so.
    travel.Crossing = false;
    travel.DryDistance = 0.0f;
    travel.Travelled = 0.0f;
    travel.HasLastPos = false;
    travel.MarkMs = 0;
    travel.MarkTravelled = 0.0f;
    travel.MarkDistance = -1.0f;
    travel.MoveRate = 0.0f;
    travel.CloseRate = 0.0f;
    if (arena.Water && FindPlace(bot, map, least, most, flying, travel.Objective, &walk, true, &travel.DryDistance))
        travel.Crossing = true;
    else if (!FindPlace(bot, map, least, most, flying, travel.Objective, &walk))
        return false;

    // What the way round costs on foot, for every arena rather than only the ones built around a crossing:
    // OBS_DETOUR is how a seat learns that the barrier in front of it runs for two hundred yards, and that is
    // as much use on broken ground as it is at a lake. One path at the build, against a reset that already
    // takes several; nothing per decision. Water and magma are excluded, so it is the ground's answer.
    if (travel.DryDistance <= 0.0f && !flying)
    {
        PathGenerator dry(bot);
        dry.SetIncludeFlags(NAV_GROUND);
        if (dry.CalculatePath(travel.Objective.GetPositionX(), travel.Objective.GetPositionY(),
            travel.Objective.GetPositionZ()) && (dry.GetPathType() & PATHFIND_NORMAL))
            travel.DryDistance = dry.getPathLength();
    }

    travel.HasObjective = true;
    travel.StartDistance = bot->GetExactDist2d(&travel.Objective);
    travel.WalkDistance = walk > 0.0f ? walk : travel.StartDistance;
    travel.KnowsFlyer = TravelBlock::FlyingMount(bot) != nullptr;
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
    view.Detour = travel.HasObjective && travel.StartDistance > 0.0f && travel.DryDistance > 0.0f
        ? travel.DryDistance / travel.StartDistance : 0.0f;
    view.MoveRate = travel.MoveRate;
    view.CloseRate = travel.CloseRate;
    view.MountsAllowed = !_scenario.Arena(env).OnFoot;
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
    if (bot->IsInWater())
        travel.SwimMs += stepMs;

    // Ground actually covered. The first decision of an episode only records where the seat is; a jump from
    // wherever the last episode ended is not travel.
    if (travel.HasLastPos)
    {
        float const dx = bot->GetPositionX() - travel.LastX;
        float const dy = bot->GetPositionY() - travel.LastY;
        travel.Travelled += std::sqrt(dx * dx + dy * dy);
    }

    travel.LastX = bot->GetPositionX();
    travel.LastY = bot->GetPositionY();
    travel.HasLastPos = true;

    // Refreshed about once a second rather than every decision: a quarter of a second of walking is 1.75 yards,
    // which is mostly noise, and the question these answer is whether the seat has been getting anywhere.
    if (!travel.MarkMs || env.EpisodeElapsedMs < travel.MarkMs)
    {
        travel.MarkMs = env.EpisodeElapsedMs;
        travel.MarkTravelled = travel.Travelled;
        travel.MarkDistance = travel.LastDistance;
    }
    else if (env.EpisodeElapsedMs - travel.MarkMs >= 1000)
    {
        float const seconds = float(env.EpisodeElapsedMs - travel.MarkMs) / 1000.0f;
        travel.MoveRate = (travel.Travelled - travel.MarkTravelled) / (seconds * TravelBlock::BASE_RUN_SPEED);
        travel.CloseRate = travel.MarkDistance > 0.0f && travel.LastDistance >= 0.0f
            ? (travel.MarkDistance - travel.LastDistance) / (seconds * TravelBlock::BASE_RUN_SPEED) : 0.0f;
        travel.MarkMs = env.EpisodeElapsedMs;
        travel.MarkTravelled = travel.Travelled;
        travel.MarkDistance = travel.LastDistance;
    }
    if (bot->IsMounted())
        travel.MountedMs += stepMs;
    if (bot->IsMounted() && bot->CanFly())
    {
        travel.FlyingMountMs += stepMs;
        travel.Flew = true;
        travel.FlightSpeedSeen = std::max(travel.FlightSpeedSeen, bot->GetSpeed(MOVE_FLIGHT));
        travel.HeightSum += double(TravelBlock::HeightAboveGround(bot));
        ++travel.HeightSamples;
    }

    // Measured between two decisions that were both aloft, so nothing but flight is counted.
    bool const aloft = bot->IsMounted() && bot->CanFly()
        && TravelBlock::HeightAboveGround(bot) > 2.0f;
    if (aloft)
    {
        ++travel.AloftSteps;
        if (bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING))
            ++travel.AloftFlagged;
    }
    if (aloft && travel.LastAloft)
    {
        float const step = bot->GetExactDist2d(&travel.LastPos);
        travel.FlightDistance += double(step);
        travel.FlightMs += stepMs;
        if (stepMs)
            travel.FlightPeakYps = std::max(travel.FlightPeakYps, step / (float(stepMs) / 1000.0f));
    }
    travel.LastPos.Relocate(bot);
    travel.LastAloft = aloft;
    if (bot->IsMounted() && bot->CanFly() && TravelBlock::HeightAboveGround(bot) > 2.0f)
        travel.FlyingMs += stepMs;

    // Potential-based: what is closed pays, what is given back costs, so wandering cannot be farmed. A flying
    // arena shapes on the distance in three dimensions, because there the objective is a point in space and the
    // last part of the trip is downwards: arriving wants AtObjective's AIRBORNE_ABOVE as well as its six yards,
    // and a seat shaped on the ground distance alone flew to directly above the marker and hovered there until
    // the clock ran out. Every episode that flew failed to arrive, so flight paid 0.009 against a ground mount's
    // 0.388, and the policy sensibly stopped flying. The climb costs here and the descent pays it back, which
    // telescopes to nothing over the trip -- the point is that the seat can see the axis it has to close.
    bool const flyingArena = _scenario.Arena(env).Flying;
    float const distance = flyingArena ? bot->GetExactDist(&travel.Objective)
        : bot->GetExactDist2d(&travel.Objective);
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

    // The clock ran out short of the objective. Only the combat encounters used to set this, so on a travel stage
    // `timed_out` was 0 for every episode however it ended -- a trip that ran its full 150 s without arriving
    // reported neither arriving nor timing out, and stage1_move's `timed_out` floor could not fail. Nothing is
    // charged for it here: not arriving already forgoes RewardTerm::Arrive, and a second penalty would be a
    // change to what the stage teaches rather than to what it reports.
    bool const timeIsUp = env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
    if (!travel.Arrived && !tally.TimedOut && timeIsUp)
        tally.TimedOut = true;
}

bool Animus::Curriculum::TravelEncounter::IsTerminal(Env const& env) const
{
    return _envs[env.Index].Arrived || _scenario.DeadForGood(env, 0);
}

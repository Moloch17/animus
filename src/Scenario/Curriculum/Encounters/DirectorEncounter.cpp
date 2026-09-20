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

#include "CombatReward.h"
#include "Encounters.h"
#include "EpisodeInfoTable.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SeatView.h"
#include "StageScenario.h"
#include <algorithm>

namespace
{
    /// How often the scripted director re-reads the fight. Every decision would let the call flicker between two
    /// equally hurt enemies; a seat cannot act on an order that changes under it.
    constexpr uint32 THINK_EVERY = 10;          // decisions, so 2.5 s at the usual 250 ms

    /// Below this the side is told to recover rather than press.
    constexpr float HURT = 0.4f;
}

Animus::Curriculum::DirectorEncounter::DirectorEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

void Animus::Curriculum::DirectorEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    // What the director did, so a directed stage can be read against the undirected one it came from.
    table.Add("order_changes", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Sides[_scenario.SideOf(env, seat)].Changes);
    });
    table.Add("order_posture", [this](Env const& env, uint32 seat)
    {
        return float(uint32(_envs[env.Index].Sides[_scenario.SideOf(env, seat)].Posture));
    });
    table.Add("order_has_focus", [this](Env const& env, uint32 seat)
    {
        return _envs[env.Index].Sides[_scenario.SideOf(env, seat)].Focus ? 1.0f : 0.0f;
    });
    // Whether the side was doing what it was told: the share of its seats on the called target.
    table.Add("order_focus_kept", [this](Env const& env, uint32 seat)
    {
        SideOrder const& side = _envs[env.Index].Sides[_scenario.SideOf(env, seat)];
        Player const* bot = _scenario.SeatBot(env, seat);
        return side.Focus && bot && bot->GetTarget() == side.Focus ? 1.0f : 0.0f;
    });
}

void Animus::Curriculum::DirectorEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index] = EnvDirector();
}

void Animus::Curriculum::DirectorEncounter::Update(Env& env)
{
    EnvDirector& state = _envs[env.Index];
    if (state.Steps++ % THINK_EVERY)
        return;

    for (uint32 side = 0; side < TEAM_COUNT; ++side)
        Command(env, side);
}

void Animus::Curriculum::DirectorEncounter::Command(Env& env, uint32 side)
{
    EnvDirector& state = _envs[env.Index];
    SideOrder& order = state.Sides[side];
    uint32 const seats = _scenario.SeatCount();

    // The side, and the enemy, as they stand.
    std::vector<uint32> mine, theirs;
    float health = 0.0f;
    uint32 alive = 0;
    for (uint32 seat = 0; seat < seats; ++seat)
    {
        Player const* bot = _scenario.SeatBot(env, seat);
        if (!bot || !bot->IsAlive())
            continue;

        if (_scenario.SideOf(env, seat) == side)
        {
            mine.push_back(seat);
            health += CombatReward::HealthLeft(bot);
            ++alive;
        }
        else
            theirs.push_back(seat);
    }

    if (mine.empty())
        return;

    // Focus: the enemy with the least left. The simplest call a director can make, and the one a seat cannot
    // make for the side -- ten seats each choosing their own target is how a team loses a fight it should win.
    ObjectGuid focus;
    float lowest = 2.0f;
    for (uint32 seat : theirs)
        if (Player const* enemy = _scenario.SeatBot(env, seat))
            if (float const left = CombatReward::HealthLeft(enemy); left < lowest)
            {
                lowest = left;
                focus = enemy->GetGUID();
            }

    // Duty: round the side in turn, so the same seat is not always the one asked to interrupt.
    uint32 const duty = mine[(state.Steps / THINK_EVERY) % mine.size()];

    // Posture follows the state of the side: press while it is whole, recover when it is not.
    float const average = alive ? health / float(alive) : 1.0f;
    TeamPosture const posture = average < HURT ? TeamPosture::Recover : TeamPosture::Attack;

    // Rally on the called target while pressing, and on home ground while recovering.
    TeamRally const rally = posture == TeamPosture::Recover ? TeamRally::OwnBase
        : focus ? TeamRally::Focus : TeamRally::None;

    if (order.Focus != focus || order.Posture != posture || order.Rally != rally)
        ++order.Changes;

    order.Focus = focus;
    order.Posture = posture;
    order.Rally = rally;
    order.Duty = duty;
    order.HasPlace = false;
}

void Animus::Curriculum::DirectorEncounter::View(Env const& env, uint32 seat, SeatView& view) const
{
    SideOrder const& order = _envs[env.Index].Sides[_scenario.SideOf(env, seat)];

    view.Order.Active = true;
    view.Order.Posture = order.Posture;
    view.Order.Rally = order.Rally;
    view.Order.HasRallyPlace = order.HasPlace;
    view.Order.RallyPlace = order.Place;
    view.Order.IsDuty = order.Duty == seat;
    view.Order.Focus = order.Focus ? ObjectAccessor::GetUnit(*view.Bot, order.Focus) : nullptr;
}

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
#include "CoreHooks.h"
#include "Env.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Player.h"
#include "SeatView.h"
#include <algorithm>
#include <limits>

Animus::Curriculum::PartyEncounter::PartyEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::PartyEncounter::RewardTerms() const
{
    return { RewardTerm::TeammateDamageTaken, RewardTerm::TeammateHealing, RewardTerm::TeammateThreat,
        RewardTerm::TeammateDeath };
}

void Animus::Curriculum::PartyEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("seat", [](Env const&, uint32 seat) { return float(seat); });
    table.Add("teammates_died", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].TeammatesDied);
    });
    table.Add("teammate_damage_taken", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].TeammateDamageTaken);
    });
    table.Add("teammate_healing", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].TeammateHealing);
    });
    table.Add("threat_on_teammates", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].ThreatOnTeammates);
    });
}

void Animus::Curriculum::PartyEncounter::ResetEpisode(Env& env)
{
    _envs[env.Index].Seats.fill(SeatParty());
}

Player* Animus::Curriculum::PartyEncounter::Tank(Env const& env) const
{
    EnvState const& data = _scenario.Data(env);
    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
        if (data.Seats[seat].L && data.Seats[seat].L->PlayRole() == Role::Tank)
            if (Player* tank = _scenario.SeatBot(env, seat); tank && tank->IsAlive())
                return tank;

    return nullptr;
}

void Animus::Curriculum::PartyEncounter::BeforeRebuild(Env& env)
{
    // The old party goes before its members do.
    Disband(env);
}

bool Animus::Curriculum::PartyEncounter::Build(Env& env, Map* /*map*/, uint8 /*level*/)
{
    EnvState const& data = _scenario.Data(env);
    Player* lead = _scenario.SeatBot(env, 0);

    // Teammates are friends whatever their races.
    for (uint32 seat = 1; seat < data.ActiveSeats; ++seat)
        _scenario.SeatBot(env, seat)->SetFaction(lead->GetFaction());

    // The owner stands in for the player whose party the companions join: it leads.
    Player* owner = _scenario.Owner(env);
    if (!owner)
    {
        // The party stage always has an owner; it has to be built first (see the build order in StageScenario).
        LOG_ERROR("module.animus", "{}: env {} builds its party group before its owner", _scenario.Name(), env.Index);
        return false;
    }

    EnvParty& party = _envs[env.Index];
    if (party.PartyGroup)
        return true;

    Group* group = new Group();
    CoreHooks::MarkSimGroup(group);
    if (!group->Create(owner))
    {
        LOG_ERROR("module.animus", "{}: env {} could not create its party", _scenario.Name(), env.Index);
        delete group;
        return true;
    }

    sGroupMgr->AddGroup(group);
    for (uint32 seat = 0; seat < _scenario.SeatCount(); ++seat)
        if (Player* bot = _scenario.SeatBot(env, seat); bot && !group->AddMember(bot))
            LOG_ERROR("module.animus", "{}: env {} could not add seat {} to its party", _scenario.Name(), env.Index,
                seat);

    party.PartyGroup = group;
    return true;
}

void Animus::Curriculum::PartyEncounter::Disband(Env& env)
{
    EnvParty& party = _envs[env.Index];
    if (!party.PartyGroup)
        return;

    // Disband removes it from the group manager and deletes it.
    party.PartyGroup->Disband(true);
    party.PartyGroup = nullptr;
}

void Animus::Curriculum::PartyEncounter::View(Env const& env, uint32 seatIndex, SeatView& view) const
{
    EnvState const& data = _scenario.Data(env);
    uint32 const seats = _scenario.SeatCount();
    Player* bot = env.FindBot(seatIndex);

    std::array<bool, MAX_SEATS> shown{};
    if (seatIndex < MAX_SEATS)
        shown[seatIndex] = true;

    auto const playing = [&](uint32 seat)
    {
        return seat < seats && seat < MAX_SEATS && !shown[seat] && data.Seats[seat].L && env.FindBot(seat);
    };
    auto const fill = [&](uint32 slot, uint32 seat)
    {
        Layout const& other = *data.Seats[seat].L;
        shown[seat] = true;
        view.Teammates[slot] = { env.FindBot(seat), data.Seats[seat].Goal, other.PlayRole(), other.Profile->Class };
    };

    // The seat's own group fills the first slots: in a party that is everyone, and in a raid it is who the seat
    // heals, assists and guards without being told.
    uint32 slot = 0;
    uint32 const groupFirst = GroupFirstSeat(seatIndex);
    for (uint32 seat = groupFirst; seat < groupFirst + GROUP_SEATS && slot < GROUP_MEMBERS; ++seat)
        if (playing(seat))
            fill(slot++, seat);

    // Then the raiders outside it a seat still has to act on, in the order they matter: the raid's living tank, its
    // most hurt member, and the nearest one. Empty below a raid, where the group is the whole party.
    slot = GROUP_MEMBERS;
    uint32 tank = MAX_SEATS;
    uint32 hurt = MAX_SEATS;
    uint32 closest = MAX_SEATS;
    float lowest = 2.0f;
    float nearest = std::numeric_limits<float>::max();
    for (uint32 seat = 0; seat < seats && seat < MAX_SEATS; ++seat)
    {
        if (!playing(seat))
            continue;

        Player* other = env.FindBot(seat);
        if (!other->IsAlive())
            continue;

        if (tank == MAX_SEATS && data.Seats[seat].L->PlayRole() == Role::Tank)
            tank = seat;

        float const health = other->GetHealthPct();
        if (health < lowest)
        {
            lowest = health;
            hurt = seat;
        }

        if (bot)
        {
            float const distance = bot->GetDistance(other);
            if (distance < nearest)
            {
                nearest = distance;
                closest = seat;
            }
        }
    }

    for (uint32 spotlight : { tank, hurt, closest })
        if (spotlight < MAX_SEATS && !shown[spotlight] && slot < PARTY_MEMBERS)
            fill(slot++, spotlight);

    // The rest of the raid in aggregate: the seat cannot act on them one by one, but how many still stand and how
    // hurt the worst is decides whether it presses on or pulls back.
    uint32 playingSeats = 0;
    uint32 alive = 0;
    uint32 inCombat = 0;
    uint32 groupSeats = 0;
    uint32 groupAlive = 0;
    uint32 tanks = 0;
    uint32 healers = 0;
    float lowestHealth = 1.0f;
    for (uint32 seat = 0; seat < seats && seat < MAX_SEATS; ++seat)
    {
        if (!data.Seats[seat].L)
            continue;

        Player* other = env.FindBot(seat);
        if (!other)
            continue;

        ++playingSeats;
        bool const ownGroup = GroupFirstSeat(seat) == groupFirst;
        groupSeats += ownGroup ? 1 : 0;
        if (!other->IsAlive())
            continue;

        ++alive;
        groupAlive += ownGroup ? 1 : 0;
        inCombat += other->IsInCombat() ? 1 : 0;
        lowestHealth = std::min(lowestHealth, other->GetHealthPct() / 100.0f);
        tanks += data.Seats[seat].L->PlayRole() == Role::Tank ? 1 : 0;
        healers += data.Seats[seat].L->PlayRole() == Role::Heal ? 1 : 0;
    }

    view.Raid.Group = groupFirst / GROUP_SEATS;
    view.Raid.Alive = playingSeats ? float(alive) / float(playingSeats) : 0.0f;
    view.Raid.GroupAlive = groupSeats ? float(groupAlive) / float(groupSeats) : 0.0f;
    view.Raid.InCombat = alive ? float(inCombat) / float(alive) : 0.0f;
    view.Raid.LowestHealth = lowestHealth;
    view.Raid.TanksAlive = std::min(1.0f, float(tanks) / float(RAID_GROUPS));
    view.Raid.HealersAlive = std::min(1.0f, float(healers) / float(RAID_GROUPS));

    view.Tank = Tank(env);
}

void Animus::Curriculum::PartyEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    if (!bot)
        return;

    CurriculumTuning::PartyTuning const& tuning = _scenario.Tuning().Party;
    EnvState const& data = _scenario.Data(env);
    SeatParty& seat = _envs[env.Index].Seats[seatIndex];
    AgentStats const& step = env.StepStats[seatIndex];
    Role const role = data.Seats[seatIndex].L->PlayRole();

    // Every other seat, not only the ones the observation has slots for: a heal lands on whoever needed it, and a
    // raider outside the seat's group is still the party's to keep alive.
    for (uint32 teammateSeat = 0; teammateSeat < _scenario.SeatCount(); ++teammateSeat)
    {
        Player* teammate = teammateSeat == seatIndex ? nullptr : env.FindBot(teammateSeat);
        if (!teammate || !data.Seats[teammateSeat].L)
            continue;

        Role const teammateRole = data.Seats[teammateSeat].L->PlayRole();
        float const health = float(std::max<uint32>(1, teammate->GetMaxHealth()));
        uint64 const taken = env.StepStats[teammateSeat].DamageTaken;
        // Healing, and what the seat's absorbs soaked and its reductions prevented on the teammate, count alike.
        uint64 const healed = step.AgentHealingBy[teammateSeat] + step.AgentProtectionBy[teammateSeat];

        seat.TeammateDamageTaken += taken;
        seat.TeammateHealing += healed;

        // A tank is there to be hit; everyone else being hit is what the party wants to avoid.
        if (teammateRole != Role::Tank)
            ledger.Add(RewardTerm::TeammateDamageTaken,
                -(role == Role::Dps ? tuning.TeammateDamageTakenDps : tuning.TeammateDamageTakenProtector)
                * float(taken) / health);

        if (role == Role::Heal)
            ledger.Add(RewardTerm::TeammateHealing, tuning.TeammateHealing * float(healed) / health);

        if (teammateRole != Role::Tank && teammate->IsAlive())
        {
            uint32 onTeammate = 0;
            for (uint32 enemySlot = 0; enemySlot < env.Targets.size(); ++enemySlot)
                if (Unit* enemy = env.FindTargetUnit(enemySlot);
                    enemy && enemy->IsAlive() && enemy->IsInCombat() && enemy->GetVictim() == teammate)
                    ++onTeammate;

            seat.ThreatOnTeammates += onTeammate;
            if (role == Role::Tank)
                ledger.Add(RewardTerm::TeammateThreat,
                    -tuning.TankLoseTeammate * float(onTeammate) * _scenario.DecisionScale());
        }

        if (teammate->IsAlive())
            seat.TeammateDeathSeen[teammateSeat] = false;
        else if (!seat.TeammateDeathSeen[teammateSeat])
        {
            seat.TeammateDeathSeen[teammateSeat] = true;
            ++seat.TeammatesDied;
            ledger.Add(RewardTerm::TeammateDeath, -tuning.TeammateDeath);
        }
    }
}

void Animus::Curriculum::PartyEncounter::OnRecovered(Env& env, int32 who)
{
    if (who < 0)
        return;

    for (SeatParty& seat : _envs[env.Index].Seats)
        seat.TeammateDeathSeen[who] = false;
}

void Animus::Curriculum::PartyEncounter::Teardown(Env& env)
{
    Disband(env);
}

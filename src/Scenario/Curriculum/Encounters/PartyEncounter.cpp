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
    for (uint32 slot = 0; slot < PARTY_MEMBERS; ++slot)
    {
        uint32 const teammateSeat = TeammateSeat(seatIndex, slot);
        if (teammateSeat >= _scenario.SeatCount() || !data.Seats[teammateSeat].L)
            continue;

        Layout const& other = *data.Seats[teammateSeat].L;
        view.Teammates[slot] = { env.FindBot(teammateSeat), other.PlayRole(), other.Profile->Class };
    }

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

    for (uint32 slot = 0; slot < PARTY_MEMBERS; ++slot)
    {
        uint32 const teammateSeat = TeammateSeat(seatIndex, slot);
        Player* teammate = teammateSeat < _scenario.SeatCount() ? env.FindBot(teammateSeat) : nullptr;
        if (!teammate)
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

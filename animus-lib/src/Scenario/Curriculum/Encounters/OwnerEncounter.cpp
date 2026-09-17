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
#include "BotAccounts.h"
#include "EncoderSupport.h"
#include "Env.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "SeatView.h"
#include "StringFormat.h"
#include <algorithm>

namespace
{
    constexpr float OWNER_START_OFFSET = 3.0f;
}

Animus::Curriculum::OwnerEncounter::OwnerEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

Player* Animus::Curriculum::OwnerEncounter::Find(Env const& env) const
{
    return _envs[env.Index].Bot.Active();
}

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::OwnerEncounter::RewardTerms() const
{
    return { RewardTerm::OwnerDamageTaken, RewardTerm::OwnerHealing, RewardTerm::TankDamageRefund, RewardTerm::Threat,
        RewardTerm::SoloFight, RewardTerm::Follow, RewardTerm::OwnerDeath, RewardTerm::Revive };
}

void Animus::Curriculum::OwnerEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("owner_class", [this](Env const& env, uint32) { return float(_envs[env.Index].Class); });
    table.Add("owner_role", [this](Env const& env, uint32) { return float(uint32(_envs[env.Index].PlayRole)); });
    table.Add("owner_died", [this](Env const& env, uint32) { return _envs[env.Index].Died ? 1.0f : 0.0f; });
    table.Add("owner_deaths", [this](Env const& env, uint32) { return float(_envs[env.Index].Deaths); });
    table.Add("owner_damage_taken", [this](Env const& env, uint32) { return float(_envs[env.Index].DamageTaken); });
    table.Add("owner_healing", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].Healing);
    });
    table.Add("threat_on_bot", [this](Env const& env, uint32 seat)
    {
        return float(_envs[env.Index].Seats[seat].ThreatOnBot);
    });
    table.Add("threat_on_owner", [this](Env const& env, uint32) { return float(_envs[env.Index].ThreatOnOwner); });
    table.Add("revives", [this](Env const& env, uint32 seat) { return float(_scenario.Data(env).Seats[seat].Revives); });
}

void Animus::Curriculum::OwnerEncounter::ResetEpisode(Env& env)
{
    EnvOwner& owner = _envs[env.Index];
    owner.Died = false;
    owner.Deaths = 0;
    owner.DeathCounted = false;
    owner.DamageTaken = 0;
    owner.ThreatOnOwner = 0;
    owner.Seats.fill(SeatOwner());
}

bool Animus::Curriculum::OwnerEncounter::Build(Env& env, Map* map, uint8 level)
{
    EnvOwner& owner = _envs[env.Index];
    CurriculumTuning::OwnerTuning const& tuning = _scenario.Tuning().Owner;
    Player* anchor = _scenario.SeatBot(env, 0);

    uint8 const ownerLevel = uint8(std::clamp<int32>(int32(level) + irand(-tuning.LevelSpread, tuning.LevelSpread), 1,
        DEFAULT_MAX_LEVEL));

    // The owner stands in for a player of any role.
    Role role = RollRole(tuning.TankChance, tuning.HealerChance);
    std::vector<uint8> classes = ClassRoleAssets::ClassesForRole(ownerLevel, role);
    if (classes.empty())
    {
        role = Role::Dps;
        classes = ClassRoleAssets::ClassesForRole(ownerLevel, role);
    }
    if (classes.empty())
        return false;

    uint8 const playerClass = classes[urand(0, uint32(classes.size()) - 1)];
    ClassRoleAssets const& assets = ClassRoleAssets::For(*ClassRoleAssets::FindProfile(playerClass, role));

    owner.Bot.Begin();
    uint8 const session = owner.Bot.NextSession();

    BotFactory::BotSpec spec;
    spec.Name = Acore::StringFormat("Owner{}{}", env.Id, session ? "b" : "a");
    spec.Race = assets.Races[urand(0, uint32(assets.Races.size()) - 1)];
    spec.Class = playerClass;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = ownerLevel;
    spec.AccountId = BotAccounts::Owner(env.Id, session);

    Position start = _scenario.SpawnPoint();
    start.m_positionX += OWNER_START_OFFSET;
    Player* bot = owner.Bot.CreateNext(spec, map, _scenario.SpawnMapId(), start);
    if (!bot)
        return false;

    bot->InitTalentForLevel();
    ScriptedPlayer::Configure(bot, assets, owner.Script, false);

    // Either faction's races can be paired: give the owner the seats' faction so they are friends (heals and buffs
    // land, neither can attack the other).
    bot->SetFaction(anchor->GetFaction());

    owner.Bot.Promote();
    owner.Class = playerClass;
    owner.PlayRole = role;
    env.Allies = { bot->GetGUID() };
    return true;
}

void Animus::Curriculum::OwnerEncounter::Update(Env& env)
{
    Player* owner = Find(env);
    if (!owner)
        return;

    std::vector<Unit*> enemies;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive())
            enemies.push_back(enemy);

    // The owner plays its role as a party member: a tank owner holds the pull and taunts, a healer owner heals the
    // most hurt of it and the seats, a damage dealer fights the tank's target once a tank has one.
    std::vector<Player*> party = { owner };
    for (uint32 seat = 0; seat < _scenario.Data(env).ActiveSeats; ++seat)
        if (Player* bot = _scenario.SeatBot(env, seat))
            party.push_back(bot);

    EnvOwner& state = _envs[env.Index];
    Player* tank = state.PlayRole == Role::Tank ? owner : _scenario.PartyTank(env);
    ScriptedPlayer::UpdateMember(owner, party, tank, enemies, env.EpisodeElapsedMs, _scenario.SpawnPoint(),
        state.Script, _scenario.Tuning().ScriptedPlayers);
}

void Animus::Curriculum::OwnerEncounter::View(Env const& env, uint32 /*seat*/, SeatView& view) const
{
    view.Owner = Find(env);
}

void Animus::Curriculum::OwnerEncounter::BeforeRewards(Env& env)
{
    // The env's owner totals, once per decision before any seat is rewarded (not while rewarding one of the seats).
    EnvOwner& state = _envs[env.Index];
    state.StepEnemiesOnOwner = 0;
    Player* owner = Find(env);
    if (!owner)
        return;

    // The owner is ally 0; every agent's step stats carry its damage taken alike.
    if (!env.StepStats.empty())
        state.DamageTaken += env.StepStats.front().AllyDamageTakenBy[0];

    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->IsInCombat()
            && enemy->GetVictim() == owner)
            ++state.StepEnemiesOnOwner;
    state.ThreatOnOwner += state.StepEnemiesOnOwner;

    // Standing again (resurrected, or recovered after a pull): its next death is counted again.
    if (owner->IsAlive())
        state.DeathCounted = false;
    else if (!state.DeathCounted)
    {
        state.DeathCounted = true;
        state.Died = true;
        ++state.Deaths;
    }
}

void Animus::Curriculum::OwnerEncounter::Reward(Env& env, uint32 seatIndex, Player* bot, RewardLedger& ledger)
{
    EnvOwner& state = _envs[env.Index];
    Player* owner = Find(env);

    // A dead ally (the owner, a teammate) the seat resurrected stood up: revives exist only beside an owner.
    if (SeatState& reviver = _scenario.Data(env).Seats[seatIndex]; reviver.StepRevivedAlly)
    {
        ledger.Add(RewardTerm::Revive, _scenario.Tuning().Resurrection.ReviveAlly);
        reviver.StepRevivedAlly = false;
    }

    if (!bot || !owner)
        return;

    CurriculumTuning::OwnerTuning const& tuning = _scenario.Tuning().Owner;
    float const scale = _scenario.DecisionScale();
    SeatState const& seat = _scenario.Data(env).Seats[seatIndex];
    SeatOwner& seatOwner = state.Seats[seatIndex];
    AgentStats const& step = env.StepStats[seatIndex];
    Role const role = seat.L->PlayRole();
    float const ownerHealth = float(std::max<uint32>(1, owner->GetMaxHealth()));

    seatOwner.Healing += step.AllyHealingBy[0];

    bool const ownerTanks = state.PlayRole == Role::Tank;
    ledger.Add(RewardTerm::OwnerDamageTaken, -(role == Role::Dps ? tuning.DamageTakenDps : tuning.DamageTakenProtector)
        * (ownerTanks ? tuning.TankOwnerDamageShare : 1.0f) * float(step.AllyDamageTakenBy[0]) / ownerHealth);

    if (role == Role::Heal)
        ledger.Add(RewardTerm::OwnerHealing, tuning.Healing * float(step.AllyHealingBy[0]) / ownerHealth);

    // Tanks take hits by design: soften the pulls' damage taken.
    if (role == Role::Tank)
        ledger.Add(RewardTerm::TankDamageRefund, tuning.TankDamageRefund * seat.LastStepDamageTaken);

    // Who the enemies are fighting (those on the owner were counted in BeforeRewards).
    uint32 onBot = 0;
    for (uint32 slot = 0; slot < env.Targets.size(); ++slot)
        if (Unit* enemy = env.FindTargetUnit(slot); enemy && enemy->IsAlive() && enemy->IsInCombat()
            && enemy->GetVictim() == bot)
            ++onBot;

    uint32 const onOwner = state.StepEnemiesOnOwner;
    seatOwner.ThreatOnBot += onBot;

    if (role == Role::Tank)
        ledger.Add(RewardTerm::Threat,
            (tuning.TankHold * float(onBot) - (ownerTanks ? 0.0f : tuning.TankLose * float(onOwner))) * scale);
    else
        ledger.Add(RewardTerm::Threat, -tuning.PulledThreat * float(onBot) * scale);

    if (owner->IsAlive())
    {
        // Standing again (resurrected, or recovered after a pull): its next death is paid for again.
        seatOwner.DeathSeen = false;

        // Fighting on its own: the companion pulled something, or kept fighting after the owner stopped. A tank
        // pulls first by design, in a party and beside a single owner alike, so it is never charged for it.
        if (bot->IsInCombat() && !owner->IsInCombat() && role != Role::Tank)
            ledger.Add(RewardTerm::SoloFight, -tuning.SoloFight * scale);

        // Out of combat, stay with the owner.
        if (bot->IsAlive() && !bot->IsInCombat() && !owner->IsInCombat() && owner->IsInMap(bot))
        {
            float const distance = bot->GetDistance(owner);
            if (distance > tuning.FollowFarDistance)
                ledger.Add(RewardTerm::Follow, -tuning.FollowFar * scale);
            else if (distance < tuning.FollowNearDistance)
                ledger.Add(RewardTerm::Follow, tuning.FollowNear * scale);
        }
    }
    else if (!seatOwner.DeathSeen)
    {
        // Every seat pays for each of the owner's deaths, once (BeforeRewards counted the death for the env).
        seatOwner.DeathSeen = true;
        ledger.Add(RewardTerm::OwnerDeath, -tuning.Death);
    }
}

void Animus::Curriculum::OwnerEncounter::WriteState(Env const& env, float* state) const
{
    Player* owner = Find(env);
    if (!owner || !owner->IsInWorld())
        return;

    Position const& origin = _scenario.SpawnPoint();
    state[StageScenario::STATE_OWNER_PRESENT] = 1.0f;
    state[StageScenario::STATE_OWNER_ALIVE] = owner->IsAlive() ? 1.0f : 0.0f;
    state[StageScenario::STATE_OWNER_HEALTH] = owner->GetHealthPct() / 100.0f;
    if (uint32 const maxMana = owner->GetMaxPower(POWER_MANA))
        state[StageScenario::STATE_OWNER_MANA] = float(owner->GetPower(POWER_MANA)) / float(maxMana);
    state[StageScenario::STATE_OWNER_X] = Encoding::RelativePosition(owner->GetPositionX(), origin.GetPositionX());
    state[StageScenario::STATE_OWNER_Y] = Encoding::RelativePosition(owner->GetPositionY(), origin.GetPositionY());
    state[StageScenario::STATE_OWNER_IN_COMBAT] = owner->IsInCombat() ? 1.0f : 0.0f;
}

void Animus::Curriculum::OwnerEncounter::OnRecovered(Env& env, int32 who)
{
    if (who != RECOVERED_OWNER)
        return;

    EnvOwner& state = _envs[env.Index];
    state.DeathCounted = false;
    for (SeatOwner& seat : state.Seats)
        seat.DeathSeen = false;
}

void Animus::Curriculum::OwnerEncounter::OnPullStarting(Env& env)
{
    Player* owner = Find(env);
    if (!owner)
        return;

    // The owner walks over to a new pull after a moment (in a party, after the tank has had time to pull). A tank
    // owner always starts the pull, and any other owner sometimes does, as a player who pulls without waiting.
    EnvOwner& state = _envs[env.Index];
    CurriculumTuning::PullTuning const& tuning = _scenario.Tuning().Pulls;
    bool const ownerPulls = state.PlayRole == Role::Tank || roll_chance_i(tuning.OwnerPullsChance);
    state.Script.EngageMs = env.EpisodeElapsedMs
        + (ownerPulls ? urand(tuning.OwnerPullsMinMs, tuning.OwnerPullsMaxMs)
        : _scenario.Arena(env).PartyGroup ? urand(tuning.PartyOwnerEngageMinMs, tuning.PartyOwnerEngageMaxMs)
        : urand(tuning.OwnerEngageMinMs, tuning.OwnerEngageMaxMs));
}

void Animus::Curriculum::OwnerEncounter::Deactivate(Env& env)
{
    Teardown(env);
    _envs[env.Index].Class = 0;
    _envs[env.Index].PlayRole = Role::Dps;
}

void Animus::Curriculum::OwnerEncounter::Teardown(Env& env)
{
    _envs[env.Index].Bot.Destroy();
    env.Allies.clear();
}

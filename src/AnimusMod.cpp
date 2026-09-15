/*
 * This file is part of the Animus project, based on AzerothCore.
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

#include "AnimusMod.h"
#include "ClassRoleProfile.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StageDefinition.h"
#include "StringFormat.h"
#include <algorithm>
#include <vector>

Animus::AnimusMod* Animus::AnimusMod::Instance()
{
    static AnimusMod instance;
    return &instance;
}

void Animus::AnimusMod::LoadConfig()
{
    _config.Load();

    // Models load when a companion or a stage seat first needs them, from the (new) model directory.
    _models.Reset(_config.Enable ? _config.ModelDir : "");

    if (!_config.Enable)
        LOG_INFO("module.animus", "Animus is disabled (Animus.Enable = 0)");
}

void Animus::AnimusMod::OnUpdate(uint32 diff)
{
    if (_parties.empty() && _viewers.empty())
        return;

    if (!_config.Enable)
    {
        RemoveAll();
        return;
    }

    CompanionParty::Settings const settings{ _config.CurriculumDecisionMs };
    std::vector<ObjectGuid> gone;
    for (auto const& [owner, party] : _parties)
        if (party->Update(diff, settings, _models) == CompanionParty::Status::Dismiss)
            gone.push_back(owner);

    for (ObjectGuid const& owner : gone)
        RemoveParty(owner);

    std::vector<ObjectGuid> ended;
    for (auto const& [viewer, stage] : _viewers)
        if (stage->Update(diff, _models) == StageViewer::Status::Ended)
            ended.push_back(viewer);

    for (ObjectGuid const& viewer : ended)
        RemoveViewer(viewer);
}

void Animus::AnimusMod::OnShutdown()
{
    RemoveAll();
}

bool Animus::AnimusMod::Dismiss(Player* owner, std::string& message)
{
    auto const party = _parties.find(owner->GetGUID());
    if (party == _parties.end() || !party->second->Size())
    {
        message = "You have no companions.";
        return false;
    }

    std::size_t const count = party->second->Size();
    RemoveParty(owner->GetGUID());
    message = count == 1 ? "Companion dismissed." : Acore::StringFormat("{} companions dismissed.", count);
    return true;
}

bool Animus::AnimusMod::Summon(Player* owner, std::string_view classRole, std::string& message)
{
    if (!_config.Enable)
    {
        message = "Animus is disabled.";
        return false;
    }

    if (owner->GetMap()->Instanceable())
    {
        message = "Companions can only be summoned in the open world.";
        return false;
    }

    if (owner->GetTransport() || owner->IsInFlight())
    {
        message = "Companions cannot be summoned while on a transport or in flight.";
        return false;
    }

    std::vector<Curriculum::ClassRoleProfile> const& profiles = Curriculum::ClassRoleProfiles();
    auto const profile = std::find_if(profiles.begin(), profiles.end(),
        [&](Curriculum::ClassRoleProfile const& candidate) { return candidate.Name == classRole; });
    if (profile == profiles.end())
    {
        message = "Unknown class/role. Choose one of:";
        for (Curriculum::ClassRoleProfile const& candidate : profiles)
            message += " " + candidate.Name;
        return false;
    }

    Curriculum::Layout const& layout = LayoutFor(*profile);

    std::unique_ptr<CompanionParty>& party = _parties[owner->GetGUID()];
    if (!party)
        party = std::make_unique<CompanionParty>(owner->GetGUID());

    if (!party->Add(owner, layout, message))
    {
        if (!party->Size())
            _parties.erase(owner->GetGUID());
        return false;
    }

    for (ObjectGuid const& bot : party->GetBotGUIDs())
        _partyByBot[bot] = party.get();

    std::string error;
    if (!_models.Find(layout, error))
        message += Acore::StringFormat(" Its model is not available ({}), so it only follows you.", error);

    return true;
}

std::vector<std::string> Animus::AnimusMod::List(Player* owner)
{
    auto const itr = _parties.find(owner->GetGUID());
    if (itr == _parties.end())
        return {};

    return itr->second->Describe(_models);
}

std::vector<std::string> Animus::AnimusMod::StageList() const
{
    std::vector<std::string> lines;
    for (Curriculum::StageDefinition const& stage : Curriculum::CurriculumStages())
    {
        std::string arenas;
        for (Curriculum::ArenaDefinition const& arena : stage.Arenas)
            arenas += (arenas.empty() ? "" : ", ") + arena.Name;

        lines.push_back(Acore::StringFormat("{}: {} (arenas: {})", stage.Name, stage.Summary, arenas));
    }

    lines.push_back("Start one with .animus stage start <stage> [model|random|greedy|fight] [arena]: it teleports you "
        "to where the stage happens.");
    return lines;
}

bool Animus::AnimusMod::StageStart(Player* viewer, std::string_view stage, std::string_view policy,
    std::string_view arena, std::string& message)
{
    if (!_config.Enable)
    {
        message = "Animus is disabled.";
        return false;
    }

    if (viewer->IsInFlight())
    {
        message = "Land first: a stage cannot start while you are in flight.";
        return false;
    }

    // A viewer watches one stage at a time: starting another replaces it.
    bool const replacing = _viewers.contains(viewer->GetGUID());
    RemoveViewer(viewer->GetGUID());

    // The lowest free env id: bot accounts and names of viewers running side by side must differ.
    uint32 envId = 0;
    while (std::any_of(_viewers.begin(), _viewers.end(),
        [envId](auto const& entry) { return entry.second->GetEnvId() == envId; }))
        ++envId;

    if (envId >= _config.StageMaxViewers)
    {
        message = Acore::StringFormat("{} stages are already running (Animus.Stage.MaxViewers).", _viewers.size());
        return false;
    }

    auto stageViewer = std::make_unique<StageViewer>(viewer->GetGUID(), envId, _config.ViewerSettings(envId));
    std::string const chosenPolicy = policy.empty() ? _config.StagePolicy : std::string(policy);
    if (!stageViewer->Begin(viewer, std::string(stage), chosenPolicy, std::string(arena), message))
        return false;

    if (replacing)
        message = "Your previous stage stopped. " + message;

    _viewers[viewer->GetGUID()] = std::move(stageViewer);
    return true;
}

bool Animus::AnimusMod::StageStop(Player* viewer, std::string& message)
{
    if (!_viewers.contains(viewer->GetGUID()))
    {
        message = "You are not watching a stage.";
        return false;
    }

    RemoveViewer(viewer->GetGUID());
    message = "Stage stopped.";
    return true;
}

bool Animus::AnimusMod::StageReset(Player* viewer, std::string& message)
{
    auto const itr = _viewers.find(viewer->GetGUID());
    if (itr == _viewers.end())
    {
        message = "You are not watching a stage.";
        return false;
    }

    itr->second->RequestReset();
    message = "A new episode starts at the next decision.";
    return true;
}

std::vector<std::string> Animus::AnimusMod::StageStatus(Player* viewer)
{
    auto const itr = _viewers.find(viewer->GetGUID());
    if (itr == _viewers.end())
        return {};

    return itr->second->Describe(_models);
}

void Animus::AnimusMod::OnPlayerLogout(Player* player)
{
    RemoveParty(player->GetGUID());
    RemoveViewer(player->GetGUID());
}

void Animus::AnimusMod::RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type)
{
    if (_partyByBot.empty() || !attacker || !victim || !damage
        || (type != DIRECT_DAMAGE && type != SPELL_DIRECT_DAMAGE && type != DOT))
        return;

    // A companion's damage dealt and taken (the forge's step damage and damage taken).
    auto const dealer = _partyByBot.find(attacker->GetGUID());
    auto const taker = _partyByBot.find(victim->GetGUID());
    if (dealer != _partyByBot.end())
        dealer->second->RecordDamage(attacker->GetGUID(), victim->GetGUID(), damage,
            taker != _partyByBot.end() && taker->second == dealer->second ? damage : 0);
    if (taker != _partyByBot.end() && (dealer == _partyByBot.end() || taker->second != dealer->second))
        taker->second->RecordDamage(attacker->GetGUID(), victim->GetGUID(), 0, damage);
}

void Animus::AnimusMod::RemoveParty(ObjectGuid owner)
{
    auto const itr = _parties.find(owner);
    if (itr == _parties.end())
        return;

    // Unlink before destroying: logging the bots out re-enters the module through OnPlayerLogout.
    std::unique_ptr<CompanionParty> const party = std::move(itr->second);
    _parties.erase(itr);
    std::erase_if(_partyByBot, [&](auto const& entry) { return entry.second == party.get(); });

    party->DestroyAll();
}

void Animus::AnimusMod::RemoveViewer(ObjectGuid viewer)
{
    auto const itr = _viewers.find(viewer);
    if (itr == _viewers.end())
        return;

    // Unlink before stopping: logging the stage's bots out re-enters the module through OnPlayerLogout.
    std::unique_ptr<StageViewer> const stage = std::move(itr->second);
    _viewers.erase(itr);
    stage->Stop();
}

void Animus::AnimusMod::RemoveAll()
{
    while (!_parties.empty())
        RemoveParty(_parties.begin()->first);

    while (!_viewers.empty())
        RemoveViewer(_viewers.begin()->first);
}

Animus::Curriculum::Layout const& Animus::AnimusMod::LayoutFor(Curriculum::ClassRoleProfile const& profile)
{
    Curriculum::StageDefinition const& stage = *Curriculum::FindStage(_config.CurriculumStage);
    std::string const name = profile.Name + stage.Suffix;
    auto itr = _layouts.find(name);
    if (itr == _layouts.end())
    {
        // Builds the class/role's assets on first use (trainer data and item pools: a few seconds).
        itr = _layouts.emplace(name, Curriculum::Layout::Build(profile, stage)).first;
        LOG_INFO("module.animus", "Animus built the {} layout (obs {}, actions {})", name, itr->second.ObsDim,
            itr->second.NumActions);
    }

    return itr->second;
}

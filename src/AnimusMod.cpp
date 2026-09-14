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
#include "BotFactory.h"
#include "ClassRoleProfile.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <filesystem>
#include <vector>

namespace
{
    enum AnimusSpells : uint32
    {
        SPELL_BATTLE_STANCE         = 2457,
    };

    /// The only targets the warrior_dummy policy was trained on.
    constexpr char const* TRAINING_DUMMY_SCRIPT = "npc_training_dummy";

    /// How far from the bot `.animus attack` accepts a dummy.
    constexpr float MAX_ATTACK_DISTANCE = 100.0f;
}

Animus::AnimusMod* Animus::AnimusMod::Instance()
{
    static AnimusMod instance;
    return &instance;
}

void Animus::AnimusMod::LoadConfig()
{
    _config.Load();

    if (!_config.Enable)
    {
        _policy.Unload();
        _models.Reset("");
        _modelError = "the module is disabled";
        LOG_INFO("module.animus", "Animus is disabled (Animus.Enable = 0)");
        return;
    }

    LoadModel();
}

void Animus::AnimusMod::LoadModel()
{
    // A relative ModelDir lives in the data directory, where the build installs the models.
    std::filesystem::path dir(_config.ModelDir);
    if (dir.is_relative())
        dir = std::filesystem::path(sWorld->GetDataPath()) / dir;

    // Class/role models load when a companion first needs them.
    _models.Reset(dir.lexically_normal().string());

    std::string const path = (dir / Acore::StringFormat("{}.amdl", WarriorDummy::SCENARIO_NAME)).lexically_normal()
        .string();

    MlpPolicy policy;
    std::string error;
    if (!policy.Load(path, WarriorDummy::SCENARIO_NAME, WarriorDummy::OBS_COUNT, WarriorDummy::ACTION_COUNT, error))
    {
        _modelError = error;
        LOG_ERROR("module.animus", "Animus model not loaded: {}", error);

        // A failed reload keeps the model already in use.
        if (_policy.IsLoaded())
            LOG_ERROR("module.animus", "Keeping the previously loaded {} model", WarriorDummy::SCENARIO_NAME);

        return;
    }

    _policy = std::move(policy);
    _modelError.clear();
    LOG_INFO("module.animus", "Animus loaded {} model from {} ({})", WarriorDummy::SCENARIO_NAME, path,
        _policy.Describe());
}

void Animus::AnimusMod::OnUpdate(uint32 diff)
{
    if (_companions.empty() && _parties.empty())
        return;

    if (!_config.Enable)
    {
        RemoveAll();
        return;
    }

    std::vector<ObjectGuid> dismissed;
    for (auto const& [owner, companion] : _companions)
    {
        // Without a model the bot still follows and fights; it just never uses abilities.
        if (companion->Update(diff, _config.DecisionMs, _tree, _policy) == WarriorCompanion::Status::Dismiss)
            dismissed.push_back(owner);
    }

    for (ObjectGuid const& owner : dismissed)
        Remove(owner);

    ClassRoleParty::Settings const settings{ _config.ClassRoleDecisionMs };
    std::vector<ObjectGuid> gone;
    for (auto const& [owner, party] : _parties)
        if (party->Update(diff, settings, _models) == ClassRoleParty::Status::Dismiss)
            gone.push_back(owner);

    for (ObjectGuid const& owner : gone)
        RemoveParty(owner);
}

void Animus::AnimusMod::OnShutdown()
{
    RemoveAll();
}

bool Animus::AnimusMod::Spawn(Player* owner, std::string& message)
{
    if (!_config.Enable)
    {
        message = "Animus is disabled.";
        return false;
    }

    if (_companions.contains(owner->GetGUID()))
    {
        message = "You already have a companion. Use .animus dismiss first.";
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

    BotFactory::BotSpec spec;
    spec.Race = RACE_HUMAN;
    spec.Class = CLASS_WARRIOR;
    spec.Gender = GENDER_MALE;
    spec.Level = 1;

    Player* bot = BotFactory::Create(spec);
    if (!bot || !BotFactory::PlaceNear(bot, owner))
    {
        message = "The companion could not be created; see the server log.";
        return false;
    }

    // A first login casts the class's start spells (playercreateinfo_cast_spell); Create does not.
    bot->CastSpell(bot, SPELL_BATTLE_STANCE, true);

    auto companion = std::make_unique<WarriorCompanion>(owner->GetGUID(), bot->GetGUID(),
        WarriorDummy::DamageScale(bot));
    _byBot[bot->GetGUID()] = companion.get();
    _companions[owner->GetGUID()] = std::move(companion);

    LOG_INFO("module.animus", "{} summoned companion {} ({})", owner->GetName(), bot->GetName(),
        bot->GetGUID().ToString());

    message = Acore::StringFormat("{} is at your side.", bot->GetName());
    if (!_policy.IsLoaded())
        message += Acore::StringFormat(" No model is loaded ({}), so it cannot attack yet.", _modelError);

    return true;
}

bool Animus::AnimusMod::Attack(Player* owner, Unit* target, std::string& message)
{
    auto const itr = _companions.find(owner->GetGUID());
    if (itr == _companions.end())
    {
        message = "You have no companion. Use .animus spawn first.";
        return false;
    }

    if (!_policy.IsLoaded())
    {
        message = Acore::StringFormat("No model is loaded: {}", _modelError);
        return false;
    }

    Creature* dummy = target ? target->ToCreature() : nullptr;
    if (!dummy || dummy->GetScriptName() != TRAINING_DUMMY_SCRIPT)
    {
        message = "Target a training dummy first.";
        return false;
    }

    Player* bot = ObjectAccessor::FindPlayer(itr->second->GetBotGUID());
    if (!bot)
    {
        message = "Your companion is not in the world.";
        return false;
    }

    if (!dummy->IsAlive() || !bot->IsWithinDistInMap(dummy, MAX_ATTACK_DISTANCE))
    {
        message = Acore::StringFormat("{} is too far from that dummy.", bot->GetName());
        return false;
    }

    if (!bot->IsValidAttackTarget(dummy))
    {
        message = Acore::StringFormat("{} cannot attack that dummy.", bot->GetName());
        return false;
    }

    itr->second->SetTarget(dummy->GetGUID());
    message = Acore::StringFormat("{} attacks {}.", bot->GetName(), dummy->GetName());
    return true;
}

bool Animus::AnimusMod::Dismiss(Player* owner, std::string& message)
{
    auto const party = _parties.find(owner->GetGUID());
    std::size_t const count = (_companions.contains(owner->GetGUID()) ? 1 : 0)
        + (party != _parties.end() ? party->second->Size() : 0);
    if (!count)
    {
        message = "You have no companions.";
        return false;
    }

    Remove(owner->GetGUID());
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

    std::vector<ClassRole::ClassRoleProfile> const& profiles = ClassRole::ClassRoleProfiles();
    auto const profile = std::find_if(profiles.begin(), profiles.end(),
        [&](ClassRole::ClassRoleProfile const& candidate) { return candidate.ScenarioName == classRole; });
    if (profile == profiles.end())
    {
        message = "Unknown class/role. Choose one of:";
        for (ClassRole::ClassRoleProfile const& candidate : profiles)
            message += " " + candidate.ScenarioName;
        return false;
    }

    ClassRole::Layout const& layout = LayoutFor(*profile);

    std::unique_ptr<ClassRoleParty>& party = _parties[owner->GetGUID()];
    if (!party)
        party = std::make_unique<ClassRoleParty>(owner->GetGUID());

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

void Animus::AnimusMod::OnPlayerLogout(Player* player)
{
    Remove(player->GetGUID());
    RemoveParty(player->GetGUID());
}

void Animus::AnimusMod::RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type)
{
    if ((_byBot.empty() && _partyByBot.empty()) || !attacker || !victim || !damage
        || (type != DIRECT_DAMAGE && type != SPELL_DIRECT_DAMAGE && type != DOT))
        return;

    auto const itr = _byBot.find(attacker->GetGUID());
    if (itr != _byBot.end())
        itr->second->RecordDamage(victim, damage);

    // A companion's damage dealt and taken (the forge's step damage and damage taken).
    auto const dealer = _partyByBot.find(attacker->GetGUID());
    auto const taker = _partyByBot.find(victim->GetGUID());
    if (dealer != _partyByBot.end())
        dealer->second->RecordDamage(attacker->GetGUID(), victim->GetGUID(), damage,
            taker != _partyByBot.end() && taker->second == dealer->second ? damage : 0);
    if (taker != _partyByBot.end() && (dealer == _partyByBot.end() || taker->second != dealer->second))
        taker->second->RecordDamage(attacker->GetGUID(), victim->GetGUID(), 0, damage);
}

void Animus::AnimusMod::Remove(ObjectGuid owner)
{
    auto const itr = _companions.find(owner);
    if (itr == _companions.end())
        return;

    // Unlink before destroying: logging the bot out re-enters the module through OnPlayerLogout.
    std::unique_ptr<WarriorCompanion> const companion = std::move(itr->second);
    _companions.erase(itr);
    _byBot.erase(companion->GetBotGUID());

    if (Player* bot = ObjectAccessor::FindPlayer(companion->GetBotGUID()))
    {
        LOG_INFO("module.animus", "Removing companion {} ({})", bot->GetName(), bot->GetGUID().ToString());
        BotFactory::Destroy(bot);
    }
}

void Animus::AnimusMod::RemoveParty(ObjectGuid owner)
{
    auto const itr = _parties.find(owner);
    if (itr == _parties.end())
        return;

    // Unlink before destroying: logging the bots out re-enters the module through OnPlayerLogout.
    std::unique_ptr<ClassRoleParty> const party = std::move(itr->second);
    _parties.erase(itr);
    std::erase_if(_partyByBot, [&](auto const& entry) { return entry.second == party.get(); });

    party->DestroyAll();
}

void Animus::AnimusMod::RemoveAll()
{
    while (!_companions.empty())
        Remove(_companions.begin()->first);

    while (!_parties.empty())
        RemoveParty(_parties.begin()->first);
}

Animus::ClassRole::Layout const& Animus::AnimusMod::LayoutFor(ClassRole::ClassRoleProfile const& profile)
{
    std::string const name = profile.ScenarioName + ClassRole::StageSuffix(_config.ClassRoleStage);
    auto itr = _layouts.find(name);
    if (itr == _layouts.end())
    {
        // Builds the class/role's assets on first use (trainer data and item pools: a few seconds).
        itr = _layouts.emplace(name, ClassRole::Layout::Build(profile, _config.ClassRoleStage)).first;
        LOG_INFO("module.animus", "Animus built the {} layout (obs {}, actions {})", name, itr->second.ObsDim,
            itr->second.NumActions);
    }

    return itr->second;
}

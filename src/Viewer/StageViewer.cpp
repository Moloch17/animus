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

#include "StageViewer.h"
#include "CellImpl.h"
#include "Chat.h"
#include "DBCStores.h"
#include "EnvPool.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "MlpPolicy.h"
#include "ModelLibrary.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PoolRegistry.h"
#include "StageDefinition.h"
#include "StageScenario.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <list>
#include <optional>
#include <string_view>

namespace
{
    using namespace Animus::Curriculum;

    /// How long the viewer has to arrive at the spawn point (a far teleport includes a loading screen).
    constexpr uint32 TRAVEL_TIMEOUT_MS = 60000;

    constexpr char const* POLICY_MODEL = "model";
    constexpr char const* POLICY_RANDOM = "random";

    /// The local policies a viewer can pick besides the models: random actions and the scripted baselines.
    constexpr std::array<char const*, 3> LOCAL_POLICIES = { POLICY_RANDOM, "greedy", "fight" };

    /// What `.freeze` puts on a player or creature: stunned, and nothing it does can move or act.
    constexpr uint32 SPELL_GM_FREEZE = 9454;
    /// How far around the viewer and each seat a freeze reaches: a stage's creatures, pets and allies are in it.
    constexpr float FREEZE_RADIUS = 150.0f;
    /// While frozen, how often units that turned up since (a pet summoned on arrival) are frozen too.
    constexpr uint32 FREEZE_SWEEP_MS = 500;

    constexpr std::string_view ANY = "any";

    /// A whole non-negative number, or nothing.
    std::optional<uint32> ParseNumber(std::string_view text)
    {
        uint32 value = 0;
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc() || end != text.data() + text.size())
            return std::nullopt;
        return value;
    }

    /// "warrior_tank" for a class and role, "class 1" when no class/role has them.
    std::string ClassRoleName(uint8 playerClass, uint32 role)
    {
        for (ClassRoleProfile const& profile : ClassRoleProfiles())
            if (profile.Class == playerClass && uint32(profile.PlayRole) == role)
                return profile.Name;

        return Acore::StringFormat("class {}", playerClass);
    }
}

Animus::StageViewer::StageViewer(ObjectGuid viewer, uint32 envId, StageSettings settings)
    : _viewer(viewer), _envId(envId), _settings(std::move(settings)), _arena(NO_ARENA), _tier(NO_TIER),
    _layout(NO_LAYOUT)
{
    _settings.FirstEnvId = envId;
}

Animus::StageViewer::~StageViewer()
{
    Stop();
}

bool Animus::StageViewer::Begin(Player* viewer, std::string const& stage, std::string const& policy,
    std::string const& arena, std::string& message)
{
    _stage = FindStage(stage);
    if (!_stage)
    {
        message = Acore::StringFormat("There is no stage {}; `.animus stage list` shows them.", stage);
        return false;
    }

    bool const localPolicy = std::any_of(LOCAL_POLICIES.begin(), LOCAL_POLICIES.end(),
        [&policy](char const* name) { return policy == name; });
    if (policy != POLICY_MODEL && !localPolicy)
    {
        message = Acore::StringFormat("There is no policy {}: use model, random, greedy or fight.", policy);
        return false;
    }
    _policy = policy;

    if (!arena.empty())
    {
        auto const itr = std::find_if(_stage->Arenas.begin(), _stage->Arenas.end(),
            [&arena](ArenaDefinition const& definition) { return definition.Name == arena; });
        if (itr == _stage->Arenas.end())
        {
            message = Acore::StringFormat("{} has no arena {}. Its arenas:", _stage->Name, arena);
            for (ArenaDefinition const& definition : _stage->Arenas)
                message += " " + definition.Name;
            return false;
        }

        _arena = uint32(std::distance(_stage->Arenas.begin(), itr));
    }

    MapEntry const* map = sMapStore.LookupEntry(_settings.SpawnMapId);
    if (!map || !map->Instanceable())
    {
        message = Acore::StringFormat("Animus.Stage.SpawnPoint.MapId {} is not an instanceable map: every stage runs "
            "in its own instance.", _settings.SpawnMapId);
        return false;
    }

    // GM mode, as `.gm on`: the stage's creatures and enemy players ignore the viewer, and the teleport is a GM's.
    bool const gmModeTurnedOn = !viewer->IsGameMaster();
    if (gmModeTurnedOn)
    {
        viewer->SetGameMaster(true);
        viewer->UpdateTriggerVisibility();
    }

    Position const& spawn = _settings.SpawnPosition;
    if (!viewer->TeleportTo(_settings.SpawnMapId, spawn.GetPositionX(), spawn.GetPositionY(), spawn.GetPositionZ(),
        spawn.GetOrientation(), TELE_TO_GM_MODE))
    {
        message = Acore::StringFormat("You could not be teleported to {}'s spawn point (map {}, {:.1f} {:.1f} {:.1f}).",
            _stage->Name, _settings.SpawnMapId, spawn.GetPositionX(), spawn.GetPositionY(), spawn.GetPositionZ());
        return false;
    }

    message = Acore::StringFormat("{}Teleporting you to {}'s spawn point (map {}); the stage is built when you arrive.",
        gmModeTurnedOn ? "GM mode is on. " : "", _stage->Name, _settings.SpawnMapId);
    return true;
}

Animus::StageViewer::Status Animus::StageViewer::Update(uint32 diff, ModelLibrary& models)
{
    if (_phase == Phase::Ended)
        return Status::Ended;

    Player* viewer = ObjectAccessor::FindConnectedPlayer(_viewer);
    if (!viewer)
    {
        End("its viewer logged out");
        return Status::Ended;
    }

    if (_phase == Phase::Travelling)
    {
        if (!viewer->IsInWorld() || viewer->IsBeingTeleported())
        {
            _travelMs += diff;
            if (_travelMs >= TRAVEL_TIMEOUT_MS)
                End("you did not arrive at its spawn point");
            return _phase == Phase::Ended ? Status::Ended : Status::Active;
        }

        if (viewer->GetMapId() != _settings.SpawnMapId || !viewer->GetMap()->Instanceable())
        {
            End(Acore::StringFormat("you are not in an instance of its map {} (did the teleport fail?)",
                _settings.SpawnMapId));
            return Status::Ended;
        }

        return Build(viewer, models) ? Status::Active : Status::Ended;
    }

    Env const& env = _pool->GetEnv(0);
    if (!viewer->IsInWorld() || viewer->GetMapId() != env.MapId || viewer->GetInstanceId() != env.InstanceId)
    {
        End("you left its instance");
        return Status::Ended;
    }

    if (_frozen)
    {
        // Nothing decides and the episode's clock stands still; what turned up since the last sweep is frozen too.
        _sinceFreezeMs += diff;
        if (_sinceFreezeMs >= FREEZE_SWEEP_MS)
        {
            _sinceFreezeMs = 0;
            FreezeUnits();
        }
        return Status::Active;
    }

    _pool->AdvanceClock(diff);

    // One decision per DecisionMs of game time; a long world update does not queue up decisions.
    _sinceDecisionMs += diff;
    if (_sinceDecisionMs < _settings.DecisionMs)
        return Status::Active;

    _sinceDecisionMs = std::min(_sinceDecisionMs - _settings.DecisionMs, _settings.DecisionMs);
    Decide(models);
    return _phase == Phase::Ended ? Status::Ended : Status::Active;
}

bool Animus::StageViewer::Build(Player* viewer, ModelLibrary& models)
{
    Map* map = viewer->GetMap();
    Tell(Acore::StringFormat("Building {} (the first build of a class/role's assets takes a few seconds)...",
        _stage->Name));

    _scenario = std::make_unique<StageScenario>(_settings, *_stage);
    if (_scenario->Layouts().empty())
    {
        End("no class/role can play it (check Animus.Stage.ClassRoles)");
        return false;
    }

    if (_policy != POLICY_MODEL && _policy != POLICY_RANDOM)
    {
        // ScriptedAction answers whether the stage has the baseline; a blank row is enough to ask.
        ScenarioSpec const spec = _scenario->Spec();
        std::vector<float> obs(spec.ObsDim, 0.0f);
        std::vector<uint8> mask(spec.NumActions, 0);
        int32 action = 0;
        if (!_scenario->ScriptedAction(_policy, obs.data(), mask.data(), 0, action))
        {
            End(Acore::StringFormat("it has no {} baseline", _policy));
            return false;
        }
    }

    _scenario->ForceArena(_arena);

    _pool = std::make_unique<EnvPool>(*_scenario, _settings);
    _pool->PlaceEnv(0, map->GetId(), map->GetInstanceId());
    if (!_pool->Setup())
    {
        End("its env could not be built; see the server log");
        return false;
    }

    _pool->ResetAll();
    PoolRegistry::Register(_pool.get());
    _phase = Phase::Running;
    _sinceDecisionMs = 0;
    _frozen = true;
    FreezeUnits();

    LOG_INFO("module.animus", "{} started stage {} in map {} instance {} with policy {}", viewer->GetName(),
        _stage->Name, map->GetId(), map->GetInstanceId(), _policy);

    for (std::string const& line : Describe(models))
        Tell(line);
    Tell("Everything is frozen: `.animus stage spawn [tier] [class_role] [level]` spawns another episode, "
        "`.animus stage start` lets it play.");
    return true;
}

void Animus::StageViewer::Decide(ModelLibrary& models)
{
    // Collect resets an env whose episode ended: note how long it ran first.
    uint32 const elapsedMs = _pool->GetEnv(0).EpisodeElapsedMs;
    _pool->Collect();

    if (_pool->Done[0])
    {
        ++_episodes;
        ReportEpisode(elapsedMs, _pool->Terminated[0] != 0);
    }

    if (_policy == POLICY_MODEL)
        ChooseModelActions(models);
    else if (!_pool->ChooseLocalActions(_policy))
    {
        End(Acore::StringFormat("its seats could not choose actions with policy {}", _policy));
        return;
    }

    _pool->ApplyActions();
}

bool Animus::StageViewer::Spawn(std::string_view tier, std::string_view classRole, std::string_view level,
    ModelLibrary& models, std::string& message)
{
    if (_phase != Phase::Running)
    {
        message = "The stage is not built yet: it is built, and its first episode spawned, when you arrive.";
        return false;
    }

    // Every choice is checked before anything is removed.
    uint32 chosenTier = NO_TIER;
    if (!tier.empty() && tier != ANY)
    {
        // The creature duel's tiers and the single pack's ladder rungs.
        auto const fightsCreature = [](ArenaDefinition const& arena) { return arena.Against == Opposition::Creature; };
        auto const singlePack = [](ArenaDefinition const& arena)
        {
            return arena.Against == Opposition::Pulls && arena.Schedule == PullSchedule::SinglePack && !arena.Owner;
        };
        auto const anyArena = [this](auto const& predicate)
        {
            return _arena < _stage->Arenas.size() ? predicate(_stage->Arenas[_arena]) : _stage->AnyArena(predicate);
        };
        bool const duel = anyArena(fightsCreature);
        bool const pack = anyArena(singlePack);
        CurriculumTuning const& tuning = _scenario->Tuning();
        uint32 const maxTier = std::max(duel ? tuning.Difficulty.MaxTier : 0, pack ? tuning.Pulls.MaxTier : 0);
        std::optional<uint32> const number = ParseNumber(tier);
        if (!duel && !pack)
        {
            message = Acore::StringFormat("{} has no difficulty tiers (only a stage that fights one creature or one "
                "pack has): use any.", _stage->Name);
            return false;
        }
        if (!number || *number > maxTier)
        {
            message = duel
                ? Acore::StringFormat("The tier is 0 to {} or any: below {} a normal creature that many levels above "
                    "the character, from it an elite.", maxTier, tuning.Difficulty.EliteTier)
                : Acore::StringFormat("The tier is 0 to {} or any: the pack ladder's rung (2, 3 and 4 creatures, two "
                    "casters, then an elite, then a level more).", maxTier);
            return false;
        }
        chosenTier = *number;
    }

    uint32 chosenLayout = NO_LAYOUT;
    if (!classRole.empty() && classRole != ANY)
    {
        std::vector<Layout> const& layouts = _scenario->Layouts();
        auto const itr = std::find_if(layouts.begin(), layouts.end(),
            [classRole](Layout const& layout) { return layout.Profile->Name == classRole; });
        if (itr == layouts.end())
        {
            message = Acore::StringFormat("{} is not a class/role this stage plays. Its class/roles:", classRole);
            for (Layout const& layout : layouts)
                message += " " + layout.Profile->Name;
            return false;
        }
        chosenLayout = itr->Index;
    }

    uint32 chosenLevel = 0;
    if (!level.empty() && level != ANY)
    {
        std::optional<uint32> const number = ParseNumber(level);
        if (!number || !*number || *number > DEFAULT_MAX_LEVEL)
        {
            message = Acore::StringFormat("The level is 1 to {} or any.", DEFAULT_MAX_LEVEL);
            return false;
        }
        chosenLevel = *number;
    }

    _tier = chosenTier;
    _layout = chosenLayout;
    _level = chosenLevel;
    _scenario->ForceTier(_tier);
    _scenario->ForceLayout(_layout);
    _scenario->ForceLevel(_level);

    // The old episode's units are removed with it; any other unit the freeze held is let go first.
    ThawUnits();
    _pool->ResetAll();
    _sinceDecisionMs = 0;
    _frozen = true;
    FreezeUnits();

    for (std::string const& line : Describe(models))
        Tell(line);
    message = "Spawned, frozen. `.animus stage start` lets it play.";
    return true;
}

bool Animus::StageViewer::Run(std::string& message)
{
    if (_phase != Phase::Running)
    {
        message = "The stage is not built yet: it is built when you arrive.";
        return false;
    }

    if (!_frozen)
    {
        message = "The stage is already playing; `.animus stage stop` freezes it.";
        return false;
    }

    ThawUnits();
    _frozen = false;
    _sinceDecisionMs = 0;
    message = Acore::StringFormat("{} is playing with policy {}: episodes follow one another until "
        "`.animus stage stop`.", _stage->Name, _policy);
    return true;
}

bool Animus::StageViewer::Freeze(std::string& message)
{
    if (_phase != Phase::Running)
    {
        message = "The stage is not built yet: it is built when you arrive.";
        return false;
    }

    if (_frozen)
    {
        message = "The stage is already frozen; `.animus stage start` lets it play.";
        return false;
    }

    _frozen = true;
    _sinceFreezeMs = 0;
    FreezeUnits();
    message = "Frozen where it stood. `.animus stage start` lets it play on.";
    return true;
}

void Animus::StageViewer::FreezeUnits()
{
    Player* viewer = ObjectAccessor::FindConnectedPlayer(_viewer);
    SpellInfo const* freeze = sSpellMgr->GetSpellInfo(SPELL_GM_FREEZE);
    if (!viewer || !viewer->IsInWorld() || !freeze || !_pool)
        return;

    Env const& env = _pool->GetEnv(0);
    std::vector<WorldObject*> centres = { viewer };
    for (uint32 seat = 0; seat < _pool->Spec().AgentsPerEnv; ++seat)
        if (Player* bot = env.FindBot(seat); bot && bot->IsInWorld() && bot->GetMap() == viewer->GetMap())
            centres.push_back(bot);

    std::list<Unit*> units;
    for (WorldObject* centre : centres)
    {
        Acore::AnyUnitInObjectRangeCheck check(centre, FREEZE_RADIUS);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(centre, units, check);
        Cell::VisitObjects(centre, searcher, FREEZE_RADIUS);
    }

    for (Unit* unit : units)
    {
        // Players other than the stage's own (its seats and scripted allies) are never frozen: the viewer, another GM.
        if (Player* player = unit->ToPlayer())
        {
            bool const stagePlayer = std::find(env.Bots.begin(), env.Bots.end(), player->GetGUID()) != env.Bots.end()
                || std::find(env.Allies.begin(), env.Allies.end(), player->GetGUID()) != env.Allies.end();
            if (!stagePlayer)
                continue;
        }

        if (unit->HasAura(SPELL_GM_FREEZE))
            continue;

        Aura::TryRefreshStackOrCreate(freeze, MAX_EFFECT_MASK, unit, unit);
        _frozenUnits.insert(unit->GetGUID());
    }
}

void Animus::StageViewer::ThawUnits()
{
    Player* viewer = ObjectAccessor::FindConnectedPlayer(_viewer);
    if (viewer && viewer->IsInWorld())
        for (ObjectGuid const& guid : _frozenUnits)
            if (Unit* unit = ObjectAccessor::GetUnit(*viewer, guid))
                unit->RemoveAurasDueToSpell(SPELL_GM_FREEZE);

    _frozenUnits.clear();
}

void Animus::StageViewer::ChooseModelActions(ModelLibrary& models)
{
    ScenarioSpec const& spec = _pool->Spec();
    for (uint32 agent = 0; agent < spec.AgentsPerEnv; ++agent)
    {
        _pool->Actions[agent] = 0;
        if (!_pool->Present[agent] || _pool->Layout[agent] >= _scenario->Layouts().size())
            continue;

        // A seat whose model is missing or refused only has the no-op; the viewer is told once per model.
        Layout const& layout = _scenario->Layouts()[_pool->Layout[agent]];
        std::string error;
        MlpPolicy* policy = models.Find(layout, error);
        if (!policy)
        {
            if (_modelErrorsTold.insert(layout.ModelName()).second)
                Tell(Acore::StringFormat("{} has no model, so its seats do nothing: {}", layout.Profile->Name, error));
            continue;
        }

        // The layout fills the first ObsDim features and NumActions mask entries of the seat's padded row.
        _pool->Actions[agent] = policy->Decide(&_pool->Obs[agent * spec.ObsDim], &_pool->Mask[agent * spec.NumActions]);
    }
}

void Animus::StageViewer::ReportEpisode(uint32 elapsedMs, bool terminal)
{
    ScenarioSpec const& spec = _pool->Spec();
    std::vector<std::string> const names = _scenario->EpisodeInfoNames();

    auto const column = [&names](std::string_view name) -> int32
    {
        auto const itr = std::find(names.begin(), names.end(), name);
        return itr == names.end() ? -1 : int32(std::distance(names.begin(), itr));
    };

    // EpisodeInfo still holds the episode that ended (Present and Layout already describe the next one).
    auto const value = [&](uint32 agent, int32 index)
    {
        return index < 0 ? 0.0f : _pool->EpisodeInfo[agent * spec.EpisodeInfoDim + uint32(index)];
    };

    uint32 const arenaIndex = uint32(value(0, column("arena")));
    std::string const arena = arenaIndex < _stage->Arenas.size() ? _stage->Arenas[arenaIndex].Name : "?";
    Tell(Acore::StringFormat("{} episode {} ({}): {:.1f} s, {}", _stage->Name, _episodes, arena,
        float(elapsedMs) / 1000.0f, terminal ? "ended" : "time limit"));

    for (uint32 agent = 0; agent < spec.AgentsPerEnv; ++agent)
    {
        if (value(agent, column("present")) == 0.0f)
            continue;

        float reward = 0.0f;
        for (std::size_t index = 0; index < names.size(); ++index)
            if (names[index].starts_with("reward_"))
                reward += value(agent, int32(index));

        Tell(Acore::StringFormat("  seat {}: level {} {}, damage {:.0f} ({:.0f} dps), taken {:.0f}, kill {}, died {}, "
            "health {:.0f}%, reward {:.2f}", agent + 1, uint32(value(agent, column("level"))),
            ClassRoleName(uint8(value(agent, column("class"))), uint32(value(agent, column("role")))),
            value(agent, column("damage")), value(agent, column("dps")), value(agent, column("damage_taken")),
            value(agent, column("killed")) != 0.0f ? "yes" : "no", value(agent, column("died")) != 0.0f ? "yes" : "no",
            value(agent, column("health_left")) * 100.0f, reward));
    }
}

std::vector<std::string> Animus::StageViewer::Describe(ModelLibrary& models) const
{
    std::vector<std::string> lines;
    std::string const name = _stage ? _stage->Name : "?";
    std::string const forced = _stage && _arena < _stage->Arenas.size()
        ? Acore::StringFormat(", arena {} only", _stage->Arenas[_arena].Name) : "";

    if (_phase != Phase::Running)
    {
        lines.push_back(Acore::StringFormat("{} (policy {}{}): {}", name, _policy, forced,
            _phase == Phase::Travelling ? "waiting for you to arrive" : "stopped"));
        return lines;
    }

    Env const& env = _pool->GetEnv(0);
    lines.push_back(Acore::StringFormat("{} (policy {}{}): {}, episode {}, arena {}, {:.0f} of {:.0f} s", name, _policy,
        forced, _frozen ? "frozen" : "playing", _episodes + 1, _scenario->Arena(env).Name,
        float(env.EpisodeElapsedMs) / 1000.0f, float(env.EpisodeLengthMs) / 1000.0f));
    lines.push_back(Acore::StringFormat("  spawning: tier {}, class/role {}, level {}",
        _tier == NO_TIER ? "any (the class/role's training tier)" : std::to_string(_tier),
        _layout < _scenario->Layouts().size() ? _scenario->Layouts()[_layout].Profile->Name : "any",
        _level ? std::to_string(_level) : "any"));

    EnvState const& data = _scenario->Data(env);
    for (uint32 seat = 0; seat < _pool->Spec().AgentsPerEnv; ++seat)
    {
        SeatState const& state = data.Seats[seat];
        if (!state.L)
        {
            lines.push_back(Acore::StringFormat("  seat {}: empty this episode", seat + 1));
            continue;
        }

        Player* bot = env.FindBot(seat);
        std::string const health = !bot ? "not in the world"
            : bot->IsAlive() ? Acore::StringFormat("{:.0f}% health", bot->GetHealthPct()) : "dead";

        std::string model;
        if (_policy == POLICY_MODEL)
        {
            std::string error;
            model = models.Find(*state.L, error) ? ", model " + state.L->ModelName() : ", no model: " + error;
        }

        lines.push_back(Acore::StringFormat("  seat {}: {} level {} {} ({}), {}{}", seat + 1,
            bot ? bot->GetName() : "?", state.Level, state.L->Profile->Name, state.L->Profile->Specs[state.Spec].Name,
            health, model));
    }

    return lines;
}

void Animus::StageViewer::End(std::string const& reason)
{
    if (_phase == Phase::Ended)
        return;

    LOG_INFO("module.animus", "Stage viewer of {} stopped {}: {}", _viewer.ToString(), _stage ? _stage->Name : "?",
        reason);
    Tell(Acore::StringFormat("{} stopped: {}.", _stage ? _stage->Name : "The stage", reason));
    Stop();
}

void Animus::StageViewer::Stop()
{
    _phase = Phase::Ended;
    ThawUnits();

    if (_pool)
    {
        // The library's hooks stop feeding the pool before its bots and creatures go.
        PoolRegistry::Unregister(_pool.get());
        _pool->Teardown();
        _pool.reset();
    }

    _scenario.reset();
}

void Animus::StageViewer::Tell(std::string const& text) const
{
    if (Player* viewer = ObjectAccessor::FindConnectedPlayer(_viewer))
        ChatHandler(viewer->GetSession()).SendSysMessage(text);
}

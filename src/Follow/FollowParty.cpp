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

#include "FollowParty.h"
#include "ActionCatalog.h"
#include "BotFactory.h"
#include "Capture.h"
#include "CharacterCache.h"
#include "ClassAssets.h"
#include "ClassKit.h"
#include "ClassProfile.h"
#include "EncoderSupport.h"
#include "GoalBlock.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PartyFramesBlock.h"
#include "Player.h"
#include "Random.h"
#include "SeatCharacter.h"
#include "SeatEncoder.h"
#include "StringFormat.h"
#include "World.h"
#include <algorithm>
#include <cmath>

// Line references are to forge-dev 7ebf4bda9: StageScenario.cpp, StageState.h (SeatState::ResetEpisode) and
// EntranceRespawn.cpp under src/server/game/Animus/Scenario/Curriculum (live-follow.PIPELINE.md traces them).

namespace
{
    using namespace Animus::Curriculum;

    /// A breath, as the core times it (StageScenario.cpp:124, Player::HandleDrowning).
    uint32 BreathMs()
    {
        return std::max<uint32>(1000, sWorld->getIntConfig(CONFIG_WATER_BREATH_TIMER));
    }

    /// SeatView::CombatTime's scale (StageScenario.cpp:107).
    constexpr float MAX_COMBAT_TIME_MS = 60000.0f;

    Player* FindBot(ObjectGuid guid)
    {
        // Connected rather than in world: a bot in the middle of a far teleport is still the party's.
        return ObjectAccessor::FindConnectedPlayer(guid);
    }
}

Animus::Follow::FollowParty::FollowParty(ObjectGuid leader) : _leader(leader)
{
}

Animus::Follow::FollowParty::~FollowParty() = default;

bool Animus::Follow::FollowParty::Add(Player* leader, Layout const& layout, std::unique_ptr<FollowPolicy> policy,
    uint8 race, std::string const& name, uint32 account, std::string& message)
{
    if (_seats.size() >= MAX_FOLLOWERS)
    {
        message = Acore::StringFormat("You already have {} followers.", MAX_FOLLOWERS);
        return false;
    }
    if (!policy)
    {
        message = Acore::StringFormat("There is no {} model to play.", layout.ModelName());
        return false;
    }

    // The leader's group, as the old companion party joined one (attic/Companion/CompanionParty.cpp, Join): made with
    // the leader leading it when there is none.
    Group* group = leader->GetGroup();
    if (group && (!group->IsLeader(leader->GetGUID()) || group->isRaidGroup() || group->isBGGroup()))
    {
        message = "Followers join a party you lead (not a raid or battleground group).";
        return false;
    }
    if (group && group->IsFull())
    {
        message = "Your group is full.";
        return false;
    }

    ClassProfile const& profile = *layout.Profile;
    BotFactory::BotSpec spec;
    spec.Name = name;
    spec.Race = race;
    spec.Class = profile.Class;
    spec.Gender = uint8(urand(GENDER_MALE, GENDER_FEMALE));
    spec.Level = uint8(leader->GetLevel());
    spec.AccountId = account;

    Player* bot = BotFactory::Create(spec);
    if (!bot || !BotFactory::PlaceNear(bot, leader))
    {
        if (bot)
            BotFactory::DestroyUnplaced(bot);
        message = "A follower could not be created; see the server log.";
        return false;
    }

    // Built as a forge seat is (StageScenario.cpp:2655-2672): a spec of the class, its standard talents, trainer spells
    // and gear; then every catalog action's highest known rank and what the character can do. The bot is on the
    // leader's map now, which death knights' talent points depend on.
    uint8 const specIndex = uint8(urand(0, uint32(profile.Specs.size()) - 1));
    bot->InitTalentForLevel();
    SeatCharacter::Built const built = SeatCharacter::Configure(bot, layout, specIndex);

    auto seat = std::make_unique<FollowSeat>();
    seat->Bot = bot->GetGUID();
    seat->Name = bot->GetName();
    seat->L = &layout;
    seat->Policy = std::move(policy);
    seat->Level = uint8(bot->GetLevel());
    seat->Race = bot->getRace();
    seat->Spec = specIndex;
    seat->Build = built.Build;
    std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
    seat->KnownRanks.assign(actions.size(), nullptr);
    for (ActionCatalog::Action const& action : actions)
        if (action.Type == ActionCatalog::Kind::Spell)
            seat->KnownRanks[action.Index] = ActionCatalog::KnownRank(bot, action.FirstRank);
    seat->Apt = Aptitude::Of(*layout.Assets, seat->Build, bot);
    // No XP, a warrior's stance (the forge's PrepareFighter); the hunter's stable offer it returns is not used here.
    SeatCharacter::PrepareFighter(bot, layout, seat->Apt);

    seat->Obs.assign(layout.ObsDim, 0.0f);
    seat->Mask.assign(layout.NumActions, 0);
    if (layout.Has(BlockId::Vision))
        seat->Image.assign(Vision::ImageBytes(Vision::Current()), 0);
    if (layout.Has(BlockId::Map))
        seat->MapRow.assign(Vision::CROP_BYTES, 0);
    seat->Memory.Reset(layout.NumActions);
    seat->LastPower = bot->GetPower(bot->getPowerType());

    bool const newGroup = !group;
    if (newGroup)
    {
        group = new Group();
        if (!group->Create(leader))
        {
            delete group;
            BotFactory::Destroy(bot, false, true);
            message = "Could not create a group for your followers; see the server log.";
            return false;
        }
        sGroupMgr->AddGroup(group);
    }
    if (bot->GetGroup() != group && !group->AddMember(bot))
    {
        if (newGroup)
            group->Disband();
        BotFactory::Destroy(bot, false, true);
        message = "A follower could not join your group; see the server log.";
        return false;
    }

    // Its game client, from here on the only way it moves (CompanionClient), synchronised with the server's clock as
    // a client entering a map is.
    seat->Client = std::make_shared<Client::CompanionClient>(bot->GetGUID());
    seat->Session = bot->GetSession();
    Client::Clients::Register(seat->Session, seat->Client);
    seat->Client->RequestTimeSync(bot);

    LOG_INFO("module.animus", "{} summoned follower {} ({} {}, level {}, model {})", leader->GetName(), bot->GetName(),
        profile.Specs[specIndex].Name, profile.Name, bot->GetLevel(), layout.ModelName());
    _seats.push_back(std::move(seat));

    // Every follower starts a new episode together, the newcomer with the rest (a summon is a new episode).
    _episodePending = true;
    _episodeKeepMap = false;
    message = Acore::StringFormat("{} ({} {}) follows you.", bot->GetName(), profile.Specs[specIndex].Name,
        profile.Name);
    return true;
}

Animus::Follow::FollowParty::Status Animus::Follow::FollowParty::Update(uint32 diff, FollowSettings const& settings)
{
    Player* leader = ObjectAccessor::FindConnectedPlayer(_leader);
    if (!leader)
        return Status::Dismiss;

    // Followers only leave through Destroy; anything else took one away. It leaves the group too, which would
    // otherwise keep it as an offline member.
    std::erase_if(_seats, [leader](std::unique_ptr<FollowSeat> const& seat)
    {
        if (FindBot(seat->Bot))
            return false;
        LOG_INFO("module.animus", "Follower {} ({}) is gone", seat->Name, seat->Bot.ToString());
        Client::Clients::Unregister(seat->Session);
        if (Group* group = leader->GetGroup(); group && group->IsMember(seat->Bot))
            group->RemoveMember(seat->Bot);
        return true;
    });
    if (_seats.empty())
        return Status::Dismiss;

    // A teleport a bot started itself completes as its client would acknowledge it. A parked bot's waits.
    for (std::unique_ptr<FollowSeat> const& seat : _seats)
        if (!seat->Parked)
            BotFactory::CompleteTeleport(FindBot(seat->Bot));

    if (!leader->IsInWorld() || leader->IsBeingTeleported())
        return Status::Active;

    // A flight path or a vehicle: no follower can come along, so they leave the world and come back beside the leader
    // once it is off, wherever that is (as the old companions did).
    if (BotFactory::IsAway(leader))
    {
        for (std::unique_ptr<FollowSeat> const& seat : _seats)
            if (!seat->Parked && BotFactory::Park(FindBot(seat->Bot)))
                seat->Parked = true;
        return Status::Active;
    }
    for (std::unique_ptr<FollowSeat> const& seat : _seats)
        if (seat->Parked && BotFactory::CanJoin(leader) && BotFactory::TeleportNear(FindBot(seat->Bot), leader))
        {
            seat->Parked = false;
            seat->Client->Restart(nullptr);
        }

    // Through the leader's loading screens: a follower not on the leader's map (or not on its transport) is brought
    // to it. The controller never sends an area trigger, so a bot cannot enter an instance portal on its own; on the
    // leader's own map nothing leashes or teleports it -- following there is the model's.
    bool present = true;
    for (std::unique_ptr<FollowSeat> const& seat : _seats)
    {
        Player* bot = FindBot(seat->Bot);
        if (seat->Parked || !bot || !bot->IsInWorld() || bot->IsBeingTeleported())
        {
            present = false;
            continue;
        }
        if (bot->GetMap() != leader->GetMap() || bot->GetTransport() != leader->GetTransport())
        {
            present = false;
            if (BotFactory::CanJoin(leader) && BotFactory::TeleportNear(bot, leader))
                seat->Client->Restart(nullptr);
        }
    }

    // A new map or instance is a new episode with a cleared mental map (decision 4), taken once every follower is
    // there; until then the clock waits and nothing is decided.
    Map* map = leader->GetMap();
    if (!_episodeStarted || map->GetId() != _mapId || map->GetInstanceId() != _instanceId)
    {
        if (_episodeStarted)
            LOG_INFO("module.animus", "{}'s followers: a new episode on map {} instance {} (mental maps cleared)",
                leader->GetName(), map->GetId(), map->GetInstanceId());
        _episodePending = true;
        _episodeKeepMap = false;
        _mapId = map->GetId();
        _instanceId = map->GetInstanceId();
        _episodeStarted = true;
    }
    if (!present)
        return Status::Active;

    // Death (the forge's RespawnClock): Respawn.DelayMs after it went down, a follower stands up again, whole.
    for (std::unique_ptr<FollowSeat> const& seat : _seats)
    {
        Player* bot = FindBot(seat->Bot);
        if (bot->IsAlive())
        {
            seat->DeadMs = 0;
            continue;
        }
        seat->DeadMs += diff;
        if (seat->DeadMs >= settings.RespawnDelayMs)
        {
            Rise(*seat, bot, leader);
            seat->DeadMs = 0;
        }
    }

    // The episode clock: real milliseconds, a decision every DecisionMs. A summon, a new map and every EpisodeMs (the
    // roll-over, which keeps the mental map) start an episode, whose first decision is taken at once at NowMs 0.
    bool decide = false;
    if (_episodePending)
    {
        StartEpisode(_episodeKeepMap);
        decide = true;
    }
    else
    {
        _nowMs += diff;
        _sinceDecisionMs += diff;
        if (_nowMs >= settings.EpisodeMs)
        {
            StartEpisode(true);
            decide = true;
        }
        else if (_sinceDecisionMs >= settings.DecisionMs)
        {
            // A long world tick never queues decisions up: one a tick at most, the remainder carried.
            _sinceDecisionMs = std::min(_sinceDecisionMs - settings.DecisionMs, settings.DecisionMs - 1);
            decide = true;
        }
    }
    if (!decide)
        return Status::Active;

    for (std::unique_ptr<FollowSeat> const& seat : _seats)
        Decide(*seat, FindBot(seat->Bot), settings);
    return Status::Active;
}

void Animus::Follow::FollowParty::StartEpisode(bool keepMap)
{
    _episodePending = false;
    _episodeKeepMap = false;
    _nowMs = 0;
    _sinceDecisionMs = 0;
    for (std::unique_ptr<FollowSeat> const& seat : _seats)
        ResetSeat(*seat, FindBot(seat->Bot), keepMap);
}

void Animus::Follow::FollowParty::ResetSeat(FollowSeat& seat, Player* bot, bool keepMap)
{
    // SeatState::ResetEpisode (StageState.h:563-785), the parts a follower has. The held keys must not outlive their
    // episode; the camera starts at the conf's pitch and zoom, at the one render size the realm casts at
    // (Animus.Vision.RenderSize: Vision::Current() carries that one size, which DrawRenderSize returns without a roll).
    seat.Client->Controls.Clear();
    Vision::FreeLook::Reset(seat.Look, Vision::Current(), Vision::DrawRenderSize(Vision::Current(),
        [](float) { return 0.0f; }));
    seat.Facing = 0.0f;
    seat.StuckRunMs = 0;
    seat.Trail.Clear();
    seat.MotionHasLast = false;
    seat.MotionTravelled = 0.0f;
    seat.MotionMarkMs = 0;
    seat.MotionMarkTravelled = 0.0f;
    seat.MotionMarkRange = -1.0f;
    seat.MoveRate = 0.0f;
    seat.Option = SeatOptionSet();
    seat.InCombat = false;
    seat.CombatStartMs = 0;
    seat.TargetSlot = 0;
    seat.RankTier = 0;
    seat.Holds = {};
    seat.Event = false;
    seat.Achieved = NO_GOAL;
    seat.EventLow = false;
    seat.EventEnemies = 0;
    seat.BelowRecover = false;
    seat.Memory.Reset(0);
    seat.BreathSpentMs = 0;
    seat.NearestHazard = Hazard();
    seat.HazardSearchMs = 0;
    seat.LastStepDamage = 0.0f;
    seat.LastStepPowerDelta = 0.0f;
    seat.LastStepDamageTaken = 0.0f;
    seat.StepDamageTaken.store(0, std::memory_order_relaxed);
    seat.Crop = CropPose();
    seat.CoverageSummary = Coverage::Summary();

    // The mental map at the episode's first look (StageScenario.cpp:2061-2071, applied in Observe): a roll-over keeps
    // it, a second older -- training keeps it half the time, by a random age, so this is within what it saw; a new map
    // or instance, or a summon, starts it afresh. PartyRecall is not reset: it clears itself when the clock runs back.
    seat.MapPending = true;
    seat.MapKept = false;
    seat.MapKeep = keepMap;
    seat.MapAgeOffset = keepMap ? 1.0f : 0.0f;

    // The model's memory and goal state (the learner's episode start), and nothing deferred from the last episode:
    // the forge applies no action after an episode's last observation.
    if (seat.Policy)
        seat.Policy->Reset();
    seat.EpisodeStart = true;
    seat.PendingSees.clear();
    seat.PendingChanged = false;
    seat.PendingPress = 0;

    // The body and facing start as the world has them (StageScenario.cpp:2518-2523, StartMover).
    seat.Client->Restart(bot);
    if (seat.Client->Mover().Started())
        seat.Facing = seat.Client->Mover().Body.Yaw;
    else if (bot)
        seat.Facing = bot->GetOrientation();
}

void Animus::Follow::FollowParty::Rise(FollowSeat& seat, Player* bot, Player* leader)
{
    // RiseAtEntrance (EntranceRespawn.cpp:25-58): what it saw before it died is forgotten, it stands up whole and out
    // of any fight, holding no keys (the face turn already applied is kept for the next observation), and its client
    // takes its body from where the server has it. Training rises at the episode's start point; the realm rises beside
    // the leader (the owner's call: a person leads, and there is no start point to walk back from).
    seat.Seen = Vision::SeenList();
    if (!bot->IsAlive())
        bot->ResurrectPlayer(1.0f, false);
    bot->SetFullHealth();
    for (Powers power : { POWER_MANA, POWER_ENERGY })
        if (bot->GetMaxPower(power))
            bot->SetPower(power, bot->GetMaxPower(power));
    bot->CombatStopWithPets(true);

    float const faceTurn = seat.Client->Controls.Held.FaceTurnApplied;
    seat.Client->Controls.Held = Movement::ControlState();
    seat.Client->Controls.Held.FaceTurnApplied = faceTurn;

    if (bot->GetMap() == leader->GetMap())
        BotFactory::TeleportWithinMap(bot, leader->GetPosition());
    seat.Client->Restart(bot);
    LOG_INFO("module.animus", "Follower {} rises beside {}", bot->GetName(), leader->GetName());
}

void Animus::Follow::FollowParty::Decide(FollowSeat& seat, Player* bot, FollowSettings const& settings)
{
    if (!bot || !seat.L || !seat.Policy)
        return;
    Layout const& layout = *seat.L;

    // What it plays, for the capture's MoverState.
    Capture::CompanionModel(bot, layout.ModelName(), uint8(layout.Has(BlockId::Move)
        ? GetBlock(BlockId::Move).Revision() : 0));

    CommitDeferred(seat, bot, settings);
    TrackController(seat, bot);
    TrackStep(seat, bot);
    Observe(seat, bot, settings);

    FollowPolicyInput input;
    input.Obs = seat.Obs.data();
    input.Mask = seat.Mask.data();
    input.Image = seat.Image.empty() ? nullptr : seat.Image.data();
    input.MapCrop = seat.MapRow.empty() ? nullptr : seat.MapRow.data();
    input.ObsDim = layout.ObsDim;
    input.NumActions = layout.NumActions;
    input.ImageBytes = uint32(seat.Image.size());
    input.MapBytes = uint32(seat.MapRow.size());
    input.EpisodeStart = seat.EpisodeStart;
    seat.EpisodeStart = false;

    FollowDecision decision;
    if (!seat.Policy->Decide(input, decision))
    {
        // Training never fails a decision: nothing is changed (no goal, look or key), and it is said once a minute.
        if (seat.FailedDecisions++ % 240 == 0)
            LOG_ERROR("module.animus", "Follower {}: its model {} could not decide ({} times)", seat.Name,
                layout.ModelName(), seat.FailedDecisions);
        return;
    }
    int32 const action = decision.Action >= 0 && uint32(decision.Action) < layout.NumActions ? decision.Action : 0;

    // The forge's order at the next tick (EnvPool::ApplyActionsForMap): the goals, then the free look, then the
    // action (StageScenario::ApplyGoals, ApplyLook, ApplySeatAction).
    ApplyGoals(seat, decision.Goals);
    Capture::CompanionDecision(bot, _leader, layout.ModelName(), seat.Obs.data(), seat.Obs.size(), action,
        seat.Holds[0].Goal, seat.Holds[1].Goal);
    if (layout.Has(BlockId::Vision))
    {
        // StageScenario::ApplyLook (StageScenario.cpp:3008-3015): a turn to camera goes to the controller as a
        // one-shot, applied at the next tick's first step; VisionBlock::Observe takes what it really turned off the
        // yaw offset at the next observation.
        float const turn = Vision::FreeLook::Apply(seat.Look, decision.Look.data(), Vision::Current());
        if (turn != 0.0f)
            seat.Client->Controls.Held.FaceTurn = turn;
    }
    ApplyAction(seat, bot, action, settings);
}

void Animus::Follow::FollowParty::CommitDeferred(FollowSeat& seat, Player* bot, FollowSettings const& settings)
{
    // The forge applies decision k's action at the start of tick k+1, where its clock already reads T(k+1) while the
    // world is still where obs k saw it (the pipeline's clock trap, 2.9). The driver applied at once; what the forge
    // stamps with that clock is stamped here, with this decision's real NowMs (never a predicted one).
    if (seat.PendingChanged)
        seat.Client->Controls.ChangedMs = _nowMs;
    if (seat.PendingPress)
        seat.Memory.Press(*seat.L, seat.PendingPress, _nowMs, settings.Actions, bot, &seat.KnownRanks);
    // The double See (2.5): the apply-side ViewSeat runs the party frames again at T(k+1) with the positions obs k
    // saw, recording a sighting of every member that was live then. Replayed now, before this observation's own.
    for (FollowSeat::SeeReplay const& seen : seat.PendingSees)
        seat.Party.See(seen.Guid, seen.X, seen.Y, _nowMs);
    seat.PendingSees.clear();
    seat.PendingChanged = false;
    seat.PendingPress = 0;
}

void Animus::Follow::FollowParty::TrackController(FollowSeat& seat, Player* bot)
{
    // StageScenario::TrackController (StageScenario.cpp:3305-3319), which training evaluates once a 250 ms tick at
    // TicksPerDecision 1: here once a decision over the map ticks since the last (the client's window). Held keys
    // getting nowhere -- a wall, a slope, a ledge's lip -- feed SeatView::PinnedMs.
    Client::CompanionClient::Window const window = seat.Client->TakeWindow();
    Movement::ControlState const& held = seat.Client->Controls.Held;
    bool const keys = held.Forward || held.Strafe || held.Vertical;
    if (!bot->IsAlive())
        seat.StuckRunMs = 0;
    else
    {
        bool const stuck = keys && window.Commanded > 0.01f && window.Moved < 0.1f * window.Commanded;
        seat.StuckRunMs = stuck ? seat.StuckRunMs + window.Ms : 0;
    }
    // The facing is the body's (SubTick sets it after every controller tick).
    if (seat.Client->Mover().Started())
        seat.Facing = seat.Client->Mover().Body.Yaw;
}

void Animus::Follow::FollowParty::TrackStep(FollowSeat& seat, Player* bot)
{
    // TrackSeatStep (StageScenario.cpp:5237-5243): damage dealt counts on the env's targets only, and a follow env
    // has none, so training's LastStepDamage is always 0 -- and so it is here, whatever the follower hits.
    seat.LastStepDamage = 0.0f;
    seat.LastStepDamageTaken = float(seat.StepDamageTaken.exchange(0, std::memory_order_relaxed))
        / float(std::max<uint32>(1, bot->GetMaxHealth()));

    // SeatReward (StageScenario.cpp:5610-5616).
    Powers const power = bot->getPowerType();
    uint32 const current = bot->GetPower(power);
    seat.LastStepPowerDelta = (float(current) - float(seat.LastPower))
        / float(std::max<uint32>(1, bot->GetMaxPower(power)));
    seat.LastPower = current;

    // TrackHazards (StageScenario.cpp:5251-5258), alive only, from the controller's body.
    if (bot->IsAlive())
    {
        Movement::Client const& mover = seat.Client->Mover();
        Position const self = mover.Started() ? Position(mover.Body.X, mover.Body.Y, mover.Body.Z)
            : bot->GetPosition();
        Encoding::TrackNearestHazard(bot, self, seat.Facing, _nowMs, seat.NearestHazard, seat.HazardSearchMs);
    }
}

void Animus::Follow::FollowParty::TrackMotion(FollowSeat& seat, Player* bot)
{
    // StageScenario::TrackMotion (StageScenario.cpp:5265-5314) with no target: the closing rate stays 0.
    constexpr uint64 MARK_MS = 1000;
    constexpr float RUN_SPEED = 7.0f;

    if (!bot->IsAlive())
    {
        seat.MotionHasLast = false;
        seat.MoveRate = 0.0f;
        return;
    }

    Movement::Client const& mover = seat.Client->Mover();
    Position const self = mover.Started() ? Position(mover.Body.X, mover.Body.Y, mover.Body.Z) : bot->GetPosition();
    float const x = self.GetPositionX();
    float const y = self.GetPositionY();
    if (seat.MotionHasLast)
    {
        float const dx = x - seat.MotionLastX;
        float const dy = y - seat.MotionLastY;
        seat.MotionTravelled += std::sqrt(dx * dx + dy * dy);
    }
    seat.MotionLastX = x;
    seat.MotionLastY = y;
    seat.MotionHasLast = true;

    if (!seat.MotionMarkMs || _nowMs < seat.MotionMarkMs)
    {
        seat.MotionMarkMs = std::max<uint64>(1, _nowMs);
        seat.MotionMarkTravelled = seat.MotionTravelled;
        seat.MotionMarkRange = -1.0f;
        return;
    }
    if (_nowMs - seat.MotionMarkMs < MARK_MS)
        return;

    float const seconds = float(_nowMs - seat.MotionMarkMs) / 1000.0f;
    seat.MoveRate = (seat.MotionTravelled - seat.MotionMarkTravelled) / (seconds * RUN_SPEED);
    seat.MotionMarkMs = _nowMs;
    seat.MotionMarkTravelled = seat.MotionTravelled;
    seat.MotionMarkRange = -1.0f;
}

Animus::Curriculum::SeatView Animus::Follow::FollowParty::View(FollowSeat& seat, Player* bot,
    FollowSettings const& settings, bool recall)
{
    // StageScenario::ViewSeat (StageScenario.cpp:3563-3706) for a move4_follow follower (the pipeline's 2.7): no
    // target, no enemies, no objective; Teammates, Tank, Focus, Recall (entity memory), SightGuids and World empty
    // (filling Teammates would open Protect goals and change the core block).
    SeatView view;
    view.L = seat.L;
    view.Bot = bot;
    view.Target = nullptr;
    view.Level = seat.Level;
    view.Race = seat.Race;
    view.Spec = seat.Spec;
    view.Apt = seat.Apt;
    view.Goal = seat.Holds[0].Goal;
    view.Goal2 = seat.Holds[1].Goal;
    view.GoalEnded = seat.Holds[0].Ended;
    view.Controls = &seat.Client->Controls;
    bool const vision = seat.L->Has(BlockId::Vision);
    view.Look = vision ? &seat.Look : nullptr;
    view.Seen = vision ? &seat.Seen : nullptr;
    view.Party = recall && seat.L->Has(BlockId::PartyFrames) ? &seat.Party : nullptr;
    Movement::Client& mover = seat.Client->Mover();
    view.Body = mover.Started() ? &mover.Body : nullptr;
    view.PinnedMs = seat.StuckRunMs;
    view.Facing = seat.Facing;
    view.Trail = &seat.Trail;
    view.MoveRate = seat.MoveRate;
    view.CloseRate = 0.0f;
    view.BreathSpent = float(seat.BreathSpentMs) / float(std::max<uint32>(1, BreathMs()));
    view.Build = &seat.Build;
    view.KnownRanks = &seat.KnownRanks;
    view.Memory = &seat.Memory;
    view.Options = settings.Options;
    view.NowMs = _nowMs;
    view.DecisionMs = settings.DecisionMs;
    view.LastStepDamage = seat.LastStepDamage;
    view.LastStepPowerDelta = seat.LastStepPowerDelta;
    view.LastStepDamageTaken = seat.LastStepDamageTaken;
    // The "since summoned" clock (decision 4): the synthetic episode's time, as training's episodes showed it.
    view.EpisodeTime = std::min(1.0f, float(_nowMs) / EPISODE_TIME_SCALE_MS);
    view.CombatTime = seat.InCombat
        ? std::min(1.0f, float(_nowMs - std::min(_nowMs, seat.CombatStartMs)) / MAX_COMBAT_TIME_MS) : 0.0f;
    view.SelfResurrectAllowed = true;
    view.EnemyCount = 0;
    view.TargetSlot = seat.TargetSlot;
    view.RankTier = seat.RankTier;

    // The party frames from the real group, its leader rotated to the first slot (PartyFramesBlock::FillFromGroup).
    if (seat.L->Has(BlockId::PartyFrames))
    {
        view.MinimapYards = settings.MinimapYards;
        PartyFramesBlock::FillFromGroup(view);
    }
    view.HasObjective = false;
    view.ObjectivePlaceKnown = GoalBlock::ObjectivePlaceKnown(false, view.CompassWithheld);
    return view;
}

void Animus::Follow::FollowParty::Observe(FollowSeat& seat, Player* bot, FollowSettings const& settings)
{
    // StageScenario::ObserveSeat (StageScenario.cpp:3992-4267) without the sight, cell, room and reward lines.
    Layout const& layout = *seat.L;
    std::fill(seat.Obs.begin(), seat.Obs.end(), 0.0f);
    if (!seat.Image.empty())
        Vision::FillNoFrame(seat.Image.data(), uint32(seat.Image.size()));
    std::fill(seat.MapRow.begin(), seat.MapRow.end(), uint8(0));
    std::fill(seat.Mask.begin(), seat.Mask.end(), uint8(0));
    seat.Mask[0] = 1;

    bool const inCombat = bot->IsAlive() && bot->IsInCombat();
    if (inCombat && !seat.InCombat)
        seat.CombatStartMs = _nowMs;
    seat.InCombat = inCombat;

    TrackMotion(seat, bot);
    if (seat.Memory.Actions() != layout.NumActions)
        seat.Memory.Reset(layout.NumActions);
    seat.Memory.Observe(bot, nullptr, _nowMs);

    // The mental map at the episode's first look (StageScenario.cpp:4030-4054).
    if (layout.Has(BlockId::Map) && seat.MapPending)
    {
        seat.MapPending = false;
        bool const same = seat.MapMapId == bot->GetMapId() && seat.MapInstanceId == bot->GetInstanceId();
        seat.MapKept = seat.MapKeep && same && seat.Map.Tiles() > 0;
        if (seat.MapKept)
            seat.Map.Advance(std::max(seat.MapAgeOffset, 1.0f));
        else
            seat.Map.Clear();
        seat.Map.BeginEpisode();
        seat.Map.Configure(Vision::MapCurrent().Caps);
        seat.MapMapId = bot->GetMapId();
        seat.MapInstanceId = bot->GetInstanceId();
    }

    SeatView view = View(seat, bot, settings, true);
    view.NearestHazard = seat.NearestHazard;
    if (layout.Has(BlockId::Map))
    {
        view.Hits = &seat.Hits;
        view.Map = &seat.Map;
        view.MapRow = seat.MapRow.data();
        view.MapKept = seat.MapKept;
        view.Crop = &seat.Crop;
        view.CoverageOut = &seat.CoverageSummary;
    }
    view.Option = &seat.Option;

    // Each goal held, read off the world as it now is: reached or no longer possible, either way ended
    // (StageScenario.cpp:4092-4220, the reward and cell lines left out).
    view.Goal2Ended = false;
    for (uint32 slot = 0; slot < GOAL_SLOTS; ++slot)
    {
        GoalHold& hold = seat.Holds[slot];
        if (hold.Goal == NO_GOAL || hold.Ended)
            continue;
        bool reached = false;
        bool possible = false;
        GoalBlock::Status(view, hold.Goal, reached, possible, &view.HeldCell[slot]);
        reached = GoalBlock::Earned(reached, hold.Fresh, hold.SatisfiedAtChoice);
        hold.Ended = reached || !possible;
        hold.WasReached = reached;
        if (slot && hold.Ended)
            view.Goal2Ended = true;
    }
    view.GoalEnded = seat.Holds[0].Ended;
    view.GoalReached = seat.Holds[0].Ended && seat.Holds[0].WasReached;
    if (seat.Holds[1].Ended)
        seat.Holds[1] = GoalHold();

    ObserveGoalSignals(seat, bot);
    view.GoalEvent = seat.Event;
    view.Achieved = seat.Achieved;
    view.Goal2 = seat.Holds[1].Goal;
    view.Image = seat.Image.empty() ? nullptr : seat.Image.data();
    SeatEncoder::Observe(view, seat.Obs.data(), seat.Mask.data());

    for (uint32 action = 1; action < layout.NumActions; ++action)
        if (seat.Mask[action] && seat.Memory.Paced(layout, action, _nowMs, settings.Actions))
            seat.Mask[action] = 0;

    // The members this observation saw live (their dot on the minimap, or in the camera's frame), where they stood:
    // the forge's apply sees them again at the next decision's clock (CommitDeferred).
    seat.PendingSees.clear();
    for (SeatView::PartyFrame const& frame : view.Frames)
        if (frame.Present && frame.RecallLive)
            if (Player const* member = ObjectAccessor::FindConnectedPlayer(frame.Guid))
                seat.PendingSees.push_back({ frame.Guid.GetRawValue(), member->GetPositionX(),
                    member->GetPositionY() });
}

void Animus::Follow::FollowParty::ObserveGoalSignals(FollowSeat& seat, Player* bot)
{
    // StageScenario::ObserveGoalSignals (StageScenario.cpp:4453-4500) with no targets (a follow env has none): nothing
    // is achieved by a kill and no fight grows, so what is left is Recover achieved -- health and mana back past 0.8
    // out of combat -- and the event of health newly under 35%.
    seat.Event = false;
    seat.Achieved = NO_GOAL;
    if (!bot->IsAlive())
        return;

    uint32 const maxMana = bot->GetMaxPower(POWER_MANA);
    float const resource = std::min(bot->GetHealthPct() / 100.0f,
        maxMana ? float(bot->GetPower(POWER_MANA)) / float(maxMana) : 1.0f);
    bool const below = resource < 0.8f;
    if (seat.BelowRecover && !below && !bot->IsInCombat())
        seat.Achieved = MakeGoal(SeatGoal::Recover, GOAL_TARGET_NONE);
    seat.BelowRecover = below;

    constexpr float EVENT_HEALTH_PCT = 35.0f;
    uint32 const enemies = 0;
    bool const low = bot->GetHealthPct() < EVENT_HEALTH_PCT;
    seat.Event = (low && !seat.EventLow) || enemies > seat.EventEnemies;
    seat.EventLow = low;
    seat.EventEnemies = enemies;
}

void Animus::Follow::FollowParty::ApplyGoals(FollowSeat& seat, std::array<int32, GOAL_SLOTS> const& goals)
{
    // StageScenario::ApplyGoals (StageScenario.cpp:2841-2947) without cells: a joint outside the goal space is none,
    // the secondary is none when it repeats the primary, and a goal that is not the one held is held afresh.
    auto const valid = [](int32 goal) { return goal >= 0 && goal < int32(GOAL_JOINT_COUNT) ? goal : NO_GOAL; };
    std::array<int32, GOAL_SLOTS> next = { valid(goals[0]), valid(goals[1]) };
    if (next[1] == next[0])
        next[1] = NO_GOAL;
    for (uint32 slot = 0; slot < GOAL_SLOTS; ++slot)
    {
        GoalHold& hold = seat.Holds[slot];
        if (next[slot] == hold.Goal)
            continue;
        hold = GoalHold();
        hold.Goal = next[slot];
        hold.Fresh = true;
    }
}

void Animus::Follow::FollowParty::ApplyAction(FollowSeat& seat, Player* bot, int32 action,
    FollowSettings const& settings)
{
    // StageScenario::ApplySeatAction (StageScenario.cpp:3738-3807, 3828-3830). A paced action is masked, so the model
    // never picks one; applied at once its pacing is what the mask already said.
    Layout const& layout = *seat.L;
    if (!SeatEncoder::ActsWithoutTarget(layout))
        return;

    SeatView view = View(seat, bot, settings, false);
    view.NearestHazard = seat.NearestHazard;
    view.Option = &seat.Option;
    SeatActionResult result;
    SeatEncoder::Apply(view, action, result);
    seat.Facing = view.Facing;

    // Breath, as the core keeps it: spent under water, back ten times as fast above it, none under a water-breathing
    // aura (StageScenario.cpp:3761-3797).
    if (bot->IsAlive())
    {
        if (bot->IsUnderWater())
        {
            if (bot->HasWaterBreathingAura())
                seat.BreathSpentMs = 0;
            else
                seat.BreathSpentMs += settings.DecisionMs;
        }
        else
            seat.BreathSpentMs -= std::min(seat.BreathSpentMs, 10 * settings.DecisionMs);
    }

    // A held control pressed again is the key kept down: no press (StageScenario.cpp:3805-3807). Stamped at the next
    // decision's clock (CommitDeferred), as are the held keys' ChangedMs.
    seat.PendingChanged = result.ControlChanged;
    seat.PendingPress = action > 0 && !result.KeyStillHeld && layout.BlockOfAction(uint32(action)) ? uint32(action) : 0;
    seat.TargetSlot = view.TargetSlot;
    seat.RankTier = view.RankTier;
}

void Animus::Follow::FollowParty::RecordDamageTaken(ObjectGuid bot, uint32 damage)
{
    for (std::unique_ptr<FollowSeat> const& seat : _seats)
        if (seat->Bot == bot)
            seat->StepDamageTaken.fetch_add(damage, std::memory_order_relaxed);
}

bool Animus::Follow::FollowParty::HasBot(ObjectGuid bot) const
{
    return std::any_of(_seats.begin(), _seats.end(),
        [bot](std::unique_ptr<FollowSeat> const& seat) { return seat->Bot == bot; });
}

std::vector<ObjectGuid> Animus::Follow::FollowParty::GetBotGUIDs() const
{
    std::vector<ObjectGuid> bots;
    for (std::unique_ptr<FollowSeat> const& seat : _seats)
        bots.push_back(seat->Bot);
    return bots;
}

std::vector<std::string> Animus::Follow::FollowParty::Describe() const
{
    std::vector<std::string> lines;
    for (std::unique_ptr<FollowSeat> const& seat : _seats)
        lines.push_back(Acore::StringFormat("{}: level {} {} ({}), model {}, episode {:.0f} s{}", seat->Name,
            seat->Level, seat->L->Profile->Name, seat->L->Profile->Specs[seat->Spec].Name, seat->L->ModelName(),
            float(_nowMs) / 1000.0f, seat->Parked ? " (waiting for you to land)" : ""));
    return lines;
}

void Animus::Follow::FollowParty::DestroyAll()
{
    std::vector<std::unique_ptr<FollowSeat>> seats = std::move(_seats);
    _seats.clear();
    for (std::unique_ptr<FollowSeat> const& seat : seats)
        Destroy(*seat);
}

void Animus::Follow::FollowParty::Destroy(FollowSeat& seat)
{
    // Its client goes first: the session it is registered under goes with the bot.
    Client::Clients::Unregister(seat.Session);
    seat.Session = nullptr;

    Player* bot = FindBot(seat.Bot);
    if (!bot)
        return;
    LOG_INFO("module.animus", "Removing follower {} ({})", bot->GetName(), bot->GetGUID().ToString());
    // Out of the group first: logging out would leave it an offline member. A group left with one player disbands.
    if (Group* group = bot->GetGroup())
        group->RemoveMember(bot->GetGUID());
    BotFactory::Destroy(bot, false, true);
}

std::string Animus::Follow::FollowParty::NewName()
{
    // Two or three syllables of a fantasy name, capitalized, that no character has and the core accepts.
    static constexpr std::array<char const*, 24> FIRST = { "Ael", "Bra", "Cor", "Dra", "Eld", "Fen", "Gal", "Hal",
        "Isa", "Jor", "Kel", "Lor", "Mar", "Nor", "Ora", "Pel", "Quin", "Ros", "Syl", "Tor", "Ul", "Val", "Wyn",
        "Zar" };
    static constexpr std::array<char const*, 20> MIDDLE = { "a", "e", "i", "o", "an", "en", "ar", "or", "is", "el",
        "ith", "on", "ra", "ri", "ul", "ae", "ia", "ed", "in", "ae" };
    static constexpr std::array<char const*, 20> LAST = { "dor", "wyn", "ric", "las", "mir", "nor", "thas", "vin",
        "ren", "dan", "sa", "ra", "lin", "gar", "mon", "thil", "ven", "rah", "dis", "mus" };
    for (uint32 attempt = 0; attempt < 100; ++attempt)
    {
        std::string name = std::string(FIRST[urand(0, FIRST.size() - 1)]) + (urand(0, 1) ? MIDDLE[urand(0,
            MIDDLE.size() - 1)] : "") + LAST[urand(0, LAST.size() - 1)];
        if (name.size() > MAX_PLAYER_NAME)
            continue;
        if (ObjectMgr::CheckPlayerName(name) == CHAR_NAME_SUCCESS && !sObjectMgr->IsReservedName(name)
            && !sCharacterCache->GetCharacterGuidByName(name))
            return name;
    }
    return {};
}

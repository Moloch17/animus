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

#ifndef ANIMUS_FOLLOW_PARTY_H
#define ANIMUS_FOLLOW_PARTY_H

#include "ActionTuning.h"
#include "Aptitude.h"
#include "CellGrid.h"
#include "CompanionClient.h"
#include "Coverage.h"
#include "FollowPolicy.h"
#include "FreeLook.h"
#include "Identity.h"
#include "Layout.h"
#include "MentalMap.h"
#include "ObjectGuid.h"
#include "SeatMemory.h"
#include "SeatView.h"
#include "TalentBuilder.h"
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class Player;
class SpellInfo;
class WorldSession;

/// **The realm's follow driver** (live-follow plan; the pipeline it reproduces is live-follow.PIPELINE.md): up to four
/// bots in the caller's group, each played by its class's move4_follow model exactly as a forge seat of that stage is
/// played -- camera, mental map, party frames with recall, goal and look heads, moving through the shared player
/// controller (CompanionClient). Everything here runs on the world thread (WorldScript::OnUpdate, after the maps have
/// updated: the observation reads the world after the movement, and the keys act at the next map update, the forge's
/// order at TicksPerDecision 1), apart from RecordDamageTaken (map threads; an atomic).
namespace Animus::Follow
{
    /// What the driver plays with, from the module's config: the decision clock and the tuning the stage trained with
    /// (CurriculumTuning's Actions, Options, PartyFollow.MinimapYards and Respawn.DelayMs, as stage.json records them).
    struct FollowSettings
    {
        uint32 DecisionMs = 250;
        Curriculum::ActionTuning Actions;
        Curriculum::OptionTuning Options;
        float MinimapYards = 60.0f;
        uint32 RespawnDelayMs = 10000;
        /// The synthetic episode (decision 4): EpisodeTime ramps over it, and every EpisodeMs the episode rolls over.
        uint32 EpisodeMs = 300000;
    };

    /// A goal held (the forge's GoalHold, StageState.h, reduced to what a follower without cell goals uses).
    struct GoalHold
    {
        int32 Goal = Curriculum::NO_GOAL;
        bool Ended = false;
        bool WasReached = false;
        bool Fresh = false;
        bool SatisfiedAtChoice = false;
    };

    /// One follower: its character, its model and everything a forge seat carries between decisions (the parts of
    /// SeatState a move4_follow seat reads).
    struct FollowSeat
    {
        ObjectGuid Bot;
        std::string Name;
        Curriculum::Layout const* L = nullptr;
        std::unique_ptr<FollowPolicy> Policy;

        // The character, as built (StageScenario::Configure).
        uint8 Level = 1;
        uint8 Race = 0;
        uint8 Spec = 0;
        Curriculum::Aptitude Apt;
        Curriculum::TalentBuilder::Build Build;
        std::vector<SpellInfo const*> KnownRanks;

        // Its game client: the keys it holds and the body they move.
        std::shared_ptr<Client::CompanionClient> Client;
        WorldSession* Session = nullptr;

        // The decision's rows.
        std::vector<float> Obs;
        std::vector<uint8> Mask;
        std::vector<uint8> Image;
        std::vector<uint8> MapRow;

        // Camera, mental map and recall.
        Vision::FreeLook::State Look;
        Vision::SeenList Seen;
        Vision::FrameHits Hits;
        Vision::MentalMap Map;
        Curriculum::CropPose Crop;
        Curriculum::Coverage::Summary CoverageSummary;
        bool MapPending = true;
        bool MapKeep = false;
        bool MapKept = false;
        float MapAgeOffset = 0.0f;
        uint32 MapMapId = 0;
        uint32 MapInstanceId = 0;
        Curriculum::PartyRecall Party;

        /// The forge applies an action at the next tick, where its clock already reads the next decision's time
        /// (the pipeline's clock trap): the driver applies at once and stamps these at the next decision's NowMs.
        struct SeeReplay
        {
            uint64 Guid = 0;
            float X = 0.0f;
            float Y = 0.0f;
        };
        std::vector<SeeReplay> PendingSees;     // members whose frame was live at the last observation (double See)
        bool PendingChanged = false;            // the last apply changed a held control (Controls.ChangedMs)
        uint32 PendingPress = 0;                // the last apply's press, for SeatMemory::Press
        bool EpisodeStart = true;               // the next decision is its episode's first

        // What it has been doing.
        Curriculum::SeatMemory Memory;
        Curriculum::SeatOptionSet Option;
        Curriculum::MovementTrail Trail;

        // The controller's bookkeeping (TrackController) and the motion rates (TrackMotion).
        float Facing = 0.0f;
        uint32 StuckRunMs = 0;
        bool MotionHasLast = false;
        float MotionLastX = 0.0f;
        float MotionLastY = 0.0f;
        float MotionTravelled = 0.0f;
        uint64 MotionMarkMs = 0;
        float MotionMarkTravelled = 0.0f;
        float MotionMarkRange = -1.0f;
        float MoveRate = 0.0f;

        // Water, hazards, combat.
        uint32 BreathSpentMs = 0;
        Curriculum::Hazard NearestHazard;
        uint64 HazardSearchMs = 0;
        bool InCombat = false;
        uint64 CombatStartMs = 0;

        // The step bookkeeping (TrackSeatStep, SeatReward).
        uint32 LastPower = 0;
        float LastStepDamage = 0.0f;
        float LastStepPowerDelta = 0.0f;
        float LastStepDamageTaken = 0.0f;
        std::atomic<uint64> StepDamageTaken{ 0 };

        // Goals and their signals.
        std::array<GoalHold, GOAL_SLOTS> Holds{};
        bool Event = false;
        int32 Achieved = Curriculum::NO_GOAL;
        bool EventLow = false;
        uint32 EventEnemies = 0;
        bool BelowRecover = false;

        uint32 TargetSlot = 0;
        uint32 RankTier = 0;

        /// Dead this long (real milliseconds): Respawn.DelayMs stands it up again.
        uint32 DeadMs = 0;
        bool Parked = false;                    // out of the world while the leader flies or rides a vehicle
        uint32 FailedDecisions = 0;
    };

    class FollowParty
    {
    public:
        enum class Status : uint8
        {
            Active,
            Dismiss,    // the leader is gone or none of the followers is left: the caller destroys the party
        };

        static constexpr uint32 MAX_FOLLOWERS = 4;

        explicit FollowParty(ObjectGuid leader);
        ~FollowParty();

        FollowParty(FollowParty const&) = delete;
        FollowParty& operator=(FollowParty const&) = delete;

        /// A new follower of `layout`'s class and `race`, at the leader's level, in a spec of the class drawn at
        /// random, joined to the leader's group (made, with the leader leading it, when there is none). Never saved.
        /// False with `message` set when it could not be made or placed.
        bool Add(Player* leader, Curriculum::Layout const& layout, std::unique_ptr<FollowPolicy> policy, uint8 race,
            std::string const& name, uint32 account, std::string& message);

        /// One world tick, after the maps updated.
        Status Update(uint32 diff, FollowSettings const& settings);

        /// Every follower out of the world, unsaved and forgotten.
        void DestroyAll();

        /// Damage a follower took (UnitScript::DealDamage, map threads).
        void RecordDamageTaken(ObjectGuid bot, uint32 damage);

        [[nodiscard]] ObjectGuid Leader() const { return _leader; }
        [[nodiscard]] bool HasBot(ObjectGuid bot) const;
        [[nodiscard]] std::vector<ObjectGuid> GetBotGUIDs() const;
        [[nodiscard]] std::size_t Size() const { return _seats.size(); }
        [[nodiscard]] std::vector<std::string> Describe() const;

        /// A name no character has and the core accepts, for a follower; empty when none was found.
        [[nodiscard]] static std::string NewName();

    private:
        /// A new episode for every follower: the clock back to 0 and each seat reset as SeatState::ResetEpisode
        /// does; the mental map kept (a roll-over, aged a second) or cleared (a new map or instance, a summon).
        void StartEpisode(bool keepMap);
        void ResetSeat(FollowSeat& seat, Player* bot, bool keepMap);

        /// One decision of one follower: the deferred stamps, the step bookkeeping, the observation, the model, the
        /// goal holds, the free look and the action.
        void Decide(FollowSeat& seat, Player* bot, FollowSettings const& settings);
        void CommitDeferred(FollowSeat& seat, Player* bot, FollowSettings const& settings);
        void TrackController(FollowSeat& seat, Player* bot);
        void TrackStep(FollowSeat& seat, Player* bot);
        void TrackMotion(FollowSeat& seat, Player* bot);
        void Observe(FollowSeat& seat, Player* bot, FollowSettings const& settings);
        void ObserveGoalSignals(FollowSeat& seat, Player* bot);
        void ApplyGoals(FollowSeat& seat, std::array<int32, GOAL_SLOTS> const& goals);
        void ApplyAction(FollowSeat& seat, Player* bot, int32 action, FollowSettings const& settings);
        /// The seat's view, as StageScenario::ViewSeat builds a move4_follow follower's. `recall`: the party frames
        /// read and write the seat's PartyRecall (the observation); the apply's view leaves it alone, its See being
        /// replayed at the next decision instead (CommitDeferred).
        [[nodiscard]] Curriculum::SeatView View(FollowSeat& seat, Player* bot, FollowSettings const& settings,
            bool recall);

        /// Stand a dead follower up beside the leader, whole (the forge's RiseAtEntrance, at the leader rather than
        /// the episode's start point: the realm has no start point to walk back from).
        void Rise(FollowSeat& seat, Player* bot, Player* leader);
        void Destroy(FollowSeat& seat);

        ObjectGuid _leader;
        std::vector<std::unique_ptr<FollowSeat>> _seats;

        /// The episode clock (SeatView::NowMs): real milliseconds since the episode began.
        uint64 _nowMs = 0;
        uint32 _sinceDecisionMs = 0;
        bool _episodePending = true;            // the next decision starts an episode ...
        bool _episodeKeepMap = false;           // ... keeping the mental map (a roll-over)
        bool _episodeStarted = false;
        uint32 _mapId = 0;                      // the leader's map and instance the episode is on
        uint32 _instanceId = 0;
    };
}

#endif

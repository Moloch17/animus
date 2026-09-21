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

#ifndef ANIMUS_STAGE_VIEWER_H
#define ANIMUS_STAGE_VIEWER_H

#include "MlpPolicy.h"
#include "ObjectGuid.h"
#include "StageSettings.h"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class Player;

namespace Animus
{
    class EnvPool;
    class ModelLibrary;

    namespace Curriculum
    {
        class StageScenario;
        struct StageDefinition;
    }

    /// A curriculum stage as mod-animus-forge runs it, without the learner, for a game master to watch: the same
    /// scenario code (animus-lib's StageScenario and EnvPool) with one env, built in the game master's own instance at
    /// the stage's spawn point. The seats play their exported models (ModelLibrary), a scripted baseline or random
    /// actions.
    ///
    /// An episode is spawned frozen, for the game master to inspect: its seats, pets and creatures are held with the
    /// GM freeze aura and nobody decides. Spawn builds a new one in its place, with a chosen difficulty tier,
    /// class and level if asked; Run lets it play, and episodes then follow one another, each reported in chat
    /// as it ends, until Freeze holds everything where it is again.
    ///
    /// World thread only. Holds its viewer by GUID.
    class StageViewer
    {
    public:
        enum class Status : uint8
        {
            Active,
            Ended,      // stopped by itself (the viewer left, the build failed); the caller destroys it
        };

        StageViewer(ObjectGuid viewer, uint32 envId, StageSettings settings);
        ~StageViewer();

        StageViewer(StageViewer const&) = delete;
        StageViewer& operator=(StageViewer const&) = delete;

        /// Check `stage`, `policy` ("model", "random" or a baseline) and `arena` (empty = drawn by weight), then send
        /// the viewer to the stage's spawn point, where its first episode is spawned frozen. False with `message` set
        /// when refused.
        bool Begin(Player* viewer, std::string const& stage, std::string const& policy, std::string const& arena,
            std::string& message);

        /// Every world update: build the stage once the viewer has arrived, then decide every DecisionMs unless frozen.
        Status Update(uint32 diff, ModelLibrary& models);

        /// Remove the current episode and spawn a new one, frozen. `tier` (a difficulty tier of a stage that fights a
        /// creature, a ladder rung of one that fights a pack), `classRole` (warlock_dps, ...) and `level` choose that
        /// part of it, and of every episode after it; empty or "any" leaves it to the curriculum. False with `message`
        /// set when refused.
        bool Spawn(std::string_view tier, std::string_view classRole, std::string_view level, ModelLibrary& models,
            std::string& message);

        /// Let the episode play, and episodes follow one another, until Freeze. False with `message` set when refused.
        bool Run(std::string& message);

        /// Hold everything where it is: the seats stop deciding, and they, their pets and the creatures are frozen.
        bool Freeze(std::string& message);

        /// Remove everything the stage built. Safe to call more than once.
        void Stop();

        [[nodiscard]] ObjectGuid GetViewerGUID() const { return _viewer; }
        [[nodiscard]] uint32 GetEnvId() const { return _envId; }

        /// Status lines: the stage, its episode and its seats.
        [[nodiscard]] std::vector<std::string> Describe(ModelLibrary& models) const;

    private:
        enum class Phase : uint8
        {
            Travelling,     // the viewer is on the way to the spawn point
            Running,
            Ended,
        };

        bool Build(Player* viewer, ModelLibrary& models);
        /// Freeze every living unit of the stage around the viewer and the seats (not the viewer), once each.
        void FreezeUnits();
        /// Lift the freeze from every unit FreezeUnits froze that is still there.
        void ThawUnits();
        void Decide(ModelLibrary& models);
        void ChooseModelActions(ModelLibrary& models);
        void ReportEpisode(uint32 elapsedMs, bool terminal);
        void End(std::string const& reason);
        void Tell(std::string const& text) const;

        ObjectGuid _viewer;
        uint32 _envId;
        StageSettings _settings;

        Curriculum::StageDefinition const* _stage = nullptr;
        std::string _policy;
        uint32 _arena;                              // forced arena index, or NO_ARENA
        uint32 _tier;                               // Spawn's choices: NO_TIER, NO_LAYOUT and 0 = the curriculum's
        uint32 _layout;
        uint32 _level = 0;
        std::unique_ptr<Curriculum::StageScenario> _scenario;
        std::unique_ptr<EnvPool> _pool;

        Phase _phase = Phase::Travelling;
        uint32 _travelMs = 0;
        uint32 _sinceDecisionMs = 0;
        uint32 _episodes = 0;
        bool _frozen = true;
        uint32 _sinceFreezeMs = 0;
        std::unordered_set<ObjectGuid> _frozenUnits;
        std::unordered_set<std::string> _modelErrorsTold;   // model names whose failure the viewer was told
        std::vector<MlpPolicy::State> _policyState;        // per seat: what its model carries between decisions
    };
}

#endif

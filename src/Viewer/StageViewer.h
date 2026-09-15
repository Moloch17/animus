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

#include "ObjectGuid.h"
#include "StageSettings.h"
#include <memory>
#include <string>
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
    /// actions; episodes follow one another, each reported in chat as it ends.
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
        /// the viewer to the stage's spawn point. False with `message` set when refused.
        bool Begin(Player* viewer, std::string const& stage, std::string const& policy, std::string const& arena,
            std::string& message);

        /// Every world update: build the stage once the viewer has arrived, then decide every DecisionMs.
        Status Update(uint32 diff, ModelLibrary& models);

        /// End the current episode at the next decision and start a new one.
        void RequestReset() { _resetRequested = true; }

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
        std::unique_ptr<Curriculum::StageScenario> _scenario;
        std::unique_ptr<EnvPool> _pool;

        Phase _phase = Phase::Travelling;
        uint32 _travelMs = 0;
        uint32 _sinceDecisionMs = 0;
        uint32 _episodes = 0;
        bool _resetRequested = false;
        std::unordered_set<std::string> _modelErrorsTold;   // model names whose failure the viewer was told
    };
}

#endif

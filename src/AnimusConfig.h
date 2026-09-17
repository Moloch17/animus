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

#ifndef ANIMUS_CONFIG_H
#define ANIMUS_CONFIG_H

#include "Define.h"
#include "Position.h"
#include "CurriculumTuning.h"
#include "StageSettings.h"
#include <string>
#include <vector>

namespace Animus
{
    /// Module settings, read at startup and on config reload (mod_animus.conf.dist documents every key).
    struct AnimusConfig
    {
        bool Enable = true;

        /// Animus.ModelDir; a relative one is looked for where the build installs models (see Load).
        std::string ModelDir;

        /// Animus.Curriculum.Stage and DecisionMs: the stage whose models `.animus summon` companions play, and their
        /// decision interval.
        std::string CurriculumStage = "stage1_duel";
        uint32 CurriculumDecisionMs = 250;
        /// Action pacing for companions (Animus.Curriculum.Actions.*), as the forge paces its seats.
        Curriculum::CurriculumTuning::ActionTuning CurriculumActions;
        /// Animus.Curriculum.Options.*: how long a companion's durative actions (rest, hold an interrupt, keep range)
        /// may run, as the forge's seats have them.
        Curriculum::CurriculumTuning::OptionTuning CurriculumOptions;

        /// Animus.Stage.*: the stage viewer (`.animus stage open`).
        uint32 StageDecisionMs = 250;
        uint32 StageEpisodeSeconds = 60;
        std::string StagePolicy = "model";
        std::vector<std::string> StageClassRoles;
        uint32 StageLevel = 0;
        uint32 StageMaxViewers = 4;
        uint32 StageSpawnMapId = 560;
        Position StageSpawnPosition{ 2741.9f, 1315.2f, 14.0f, 2.96f };

        /// A viewer's scenario settings: one env, placed by the viewer, with env id `envId` (bot accounts and names).
        [[nodiscard]] StageSettings ViewerSettings(uint32 envId) const;

        void Load();
    };
}

#endif

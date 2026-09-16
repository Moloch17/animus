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

#include "AnimusConfig.h"
#include "Config.h"
#include "DBCEnums.h"
#include "Log.h"
#include "StageDefinition.h"
#include "Tokenize.h"
#include "World.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace
{
    /// A comma-separated config list, whitespace removed, empty entries dropped.
    std::vector<std::string> GetList(std::string const& key)
    {
        std::vector<std::string> entries;
        std::string const value = sConfigMgr->GetOption<std::string>(key, "");
        for (std::string_view name : Acore::Tokenize(value, ',', false))
        {
            std::string entry(name);
            entry.erase(std::remove_if(entry.begin(), entry.end(), [](unsigned char c) { return std::isspace(c); }),
                entry.end());

            if (!entry.empty())
                entries.push_back(std::move(entry));
        }

        return entries;
    }
}

void Animus::AnimusConfig::Load()
{
    Enable = sConfigMgr->GetOption<bool>("Animus.Enable", true);

    // A relative ModelDir lives in the data directory, where the build installs the models.
    std::filesystem::path dir(sConfigMgr->GetOption<std::string>("Animus.ModelDir", "animus"));
    if (dir.is_relative())
        dir = std::filesystem::path(sWorld->GetDataPath()) / dir;
    ModelDir = dir.lexically_normal().string();

    CurriculumStage = sConfigMgr->GetOption<std::string>("Animus.Curriculum.Stage", "stage5_party");
    if (!Curriculum::FindStage(CurriculumStage))
    {
        LOG_ERROR("module.animus", "Animus.Curriculum.Stage \"{}\" is not a curriculum stage; using \"stage5_party\"",
            CurriculumStage);
        CurriculumStage = "stage5_party";
    }
    CurriculumDecisionMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Animus.Curriculum.DecisionMs", 250));
    CurriculumActions = Curriculum::CurriculumTuning::Load("Animus.Curriculum.").Actions;

    StageDecisionMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Animus.Stage.DecisionMs", 250));
    StageEpisodeSeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Animus.Stage.EpisodeSeconds", 60));
    StagePolicy = sConfigMgr->GetOption<std::string>("Animus.Stage.Policy", "model");
    StageClassRoles = GetList("Animus.Stage.ClassRoles");
    StageLevel = std::min<uint32>(DEFAULT_MAX_LEVEL, sConfigMgr->GetOption<uint32>("Animus.Stage.Level", 0));
    StageMaxViewers = sConfigMgr->GetOption<uint32>("Animus.Stage.MaxViewers", 4);

    StageSpawnMapId = sConfigMgr->GetOption<uint32>("Animus.Stage.SpawnPoint.MapId", 560);
    StageSpawnPosition.Relocate(
        sConfigMgr->GetOption<float>("Animus.Stage.SpawnPoint.X", 2741.9f),
        sConfigMgr->GetOption<float>("Animus.Stage.SpawnPoint.Y", 1315.2f),
        sConfigMgr->GetOption<float>("Animus.Stage.SpawnPoint.Z", 14.0f),
        sConfigMgr->GetOption<float>("Animus.Stage.SpawnPoint.O", 2.96f));
}

Animus::StageSettings Animus::AnimusConfig::ViewerSettings(uint32 envId) const
{
    StageSettings settings;
    settings.Envs = 1;
    settings.FirstEnvId = envId;
    settings.DecisionMs = StageDecisionMs;
    settings.EpisodeSeconds = StageEpisodeSeconds;
    settings.ReportEpisodes = 1;
    settings.ClassRoles = StageClassRoles;
    settings.SpawnMapId = StageSpawnMapId;
    settings.SpawnPosition = StageSpawnPosition;
    settings.Level = StageLevel;
    settings.TuningPrefix = "Animus.Curriculum.";
    return settings;
}

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
#include <vector>

namespace
{
    /// Whether `dir` holds at least one exported model.
    bool HasModels(std::filesystem::path const& dir)
    {
        std::error_code error;
        if (!std::filesystem::is_directory(dir, error))
            return false;

        for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(dir, error))
            if (entry.path().extension() == ".amdl")
                return true;

        return false;
    }

    /// Animus.ModelDir as a directory. An absolute path is used as is. A relative one is looked for, in order, under
    /// DataDir (where models placed by hand go), under the image's reference config directory (Docker: the build
    /// installs models with the module's configs, and only bin/ and etc/ reach the runtime image, so this copy always
    /// matches the build), and under the config directory's modules/ (the build's install location; Docker copies the
    /// reference configs there once and never refreshes them). The first holding a model wins; with none, the config
    /// directory's, which errors name.
    std::string ResolveModelDir(std::string const& setting)
    {
        std::filesystem::path const dir(setting);
        if (dir.is_absolute())
            return dir.lexically_normal().string();

        std::filesystem::path configDir = std::filesystem::path(sConfigMgr->GetConfigPath()).lexically_normal();
        if (configDir.filename().empty())
            configDir = configDir.parent_path();

        std::vector<std::filesystem::path> const candidates =
        {
            std::filesystem::path(sWorld->GetDataPath()) / dir,
            configDir.parent_path().parent_path() / "ref" / "etc" / "modules" / dir,
            configDir / "modules" / dir,
        };

        for (std::filesystem::path const& candidate : candidates)
        {
            if (HasModels(candidate))
            {
                LOG_INFO("module.animus", "Animus models: {}", candidate.lexically_normal().string());
                return candidate.lexically_normal().string();
            }
        }

        LOG_WARN("module.animus", "Animus found no models under {}, {} or {}",
            candidates[0].lexically_normal().string(), candidates[1].lexically_normal().string(),
            candidates[2].lexically_normal().string());
        return candidates.back().lexically_normal().string();
    }

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

    ModelDir = ResolveModelDir(sConfigMgr->GetOption<std::string>("Animus.ModelDir", "animus"));

    CurriculumStage = sConfigMgr->GetOption<std::string>("Animus.Curriculum.Stage", "stage1_duel");
    if (!Curriculum::FindStage(CurriculumStage))
    {
        LOG_ERROR("module.animus", "Animus.Curriculum.Stage \"{}\" is not a curriculum stage; using \"stage1_duel\"",
            CurriculumStage);
        CurriculumStage = "stage1_duel";
    }
    CurriculumDecisionMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Animus.Curriculum.DecisionMs", 250));
    Curriculum::CurriculumTuning const curriculum = Curriculum::CurriculumTuning::Load("Animus.Curriculum.");
    CurriculumActions = curriculum.Actions;
    CurriculumOptions = curriculum.Options;

}

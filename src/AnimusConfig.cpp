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
#include "Log.h"
#include "StageDefinition.h"
#include "Tokenize.h"
#include "World.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string_view>
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

    /// The directory the server's config file is in.
    std::filesystem::path ConfigDir()
    {
        std::filesystem::path configDir = std::filesystem::path(sConfigMgr->GetConfigPath()).lexically_normal();
        if (configDir.filename().empty())
            configDir = configDir.parent_path();
        return configDir;
    }

    /// A float key clamped into its range, said when it was not in it (as the forge reads AnimusForge.Vision.*).
    float Ranged(char const* key, float fallback, float low, float high)
    {
        float const value = sConfigMgr->GetOption<float>(key, fallback);
        float const kept = std::clamp(std::isfinite(value) ? value : fallback, low, high);
        if (kept != value)
            LOG_ERROR("module.animus", "{} = {} is outside {} to {}: using {}", key, value, low, high, kept);
        return kept;
    }

    /// A comma-separated config list, whitespace removed, empty entries dropped.
    std::vector<std::string> GetList(std::string const& key, std::string const& fallback)
    {
        std::vector<std::string> entries;
        std::string const value = sConfigMgr->GetOption<std::string>(key, fallback);
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

    // The stage whose models the followers play (models/<class>_follow.amdl). A name that is not a stage falls back to
    // it, and it to the first stage, since the layouts are built from whatever is named here.
    constexpr std::string_view DEFAULT_STAGE = "move4_follow";
    CurriculumStage = sConfigMgr->GetOption<std::string>("Animus.Curriculum.Stage", std::string(DEFAULT_STAGE));
    if (!Curriculum::FindStage(CurriculumStage))
    {
        std::string const fallback = Curriculum::FindStage(DEFAULT_STAGE) || Curriculum::CurriculumStages().empty()
            ? std::string(DEFAULT_STAGE) : Curriculum::CurriculumStages().front().Name;
        LOG_ERROR("module.animus", "Animus.Curriculum.Stage \"{}\" is not a curriculum stage; using \"{}\"",
            CurriculumStage, fallback);
        CurriculumStage = fallback;
    }

    // Where the module writes (the camera scenes): <config dir>/modules/animus by default, beside the installed models.
    std::filesystem::path dataDir = sConfigMgr->GetOption<std::string>("Animus.DataDir", "");
    if (dataDir.empty())
        dataDir = ConfigDir() / "modules" / "animus";
    else if (dataDir.is_relative())
        dataDir = ConfigDir() / dataDir;
    DataDir = dataDir.lexically_normal().string();

    VisionMaps.clear();
    for (std::string const& entry : GetList("Animus.Vision.Maps", "389,36,43,34"))
    {
        char* end = nullptr;
        unsigned long const id = std::strtoul(entry.c_str(), &end, 10);
        if (end && *end == '\0' && id < 10000)
            VisionMaps.push_back(uint32(id));
        else
            LOG_ERROR("module.animus", "Animus.Vision.Maps: \"{}\" is not a map id; left out", entry);
    }

    // The camera, as the forge reads AnimusForge.Vision.* (ForgeConfig.cpp), defaulting to what move4_follow trained
    // with (its stage.json blocks[vision]; Zoom and ZoomMin locked at 12 in the machines' confs).
    Camera = Vision::Settings();
    Camera.Width = uint32(Ranged("Animus.Vision.Width", 128.0f, 8.0f, 256.0f));
    Camera.Height = uint32(Ranged("Animus.Vision.Height", 64.0f, 8.0f, 256.0f));
    Camera.FovH = Ranged("Animus.Vision.FovH", 120.0f, 30.0f, 170.0f);
    Camera.FovV = Ranged("Animus.Vision.FovV", 60.0f, 20.0f, 120.0f);
    Camera.Range = Ranged("Animus.Vision.Range", 100.0f, 10.0f, 500.0f);
    Camera.Zoom = Ranged("Animus.Vision.Zoom", 12.0f, 0.0f, 50.0f);
    Camera.ZoomMin = std::min(Camera.Zoom, Ranged("Animus.Vision.ZoomMin", 12.0f, 0.0f, 50.0f));
    Camera.Pitch = Ranged("Animus.Vision.Pitch", -15.0f, -80.0f, 80.0f);
    // The render sizes the models were trained with (Animus.Vision.RenderSizes, the forge's AnimusForge.Vision.
    // RenderSizes): a layout's manifest records them, so a model is refused unless they match. Every follower casts
    // at one of them, Animus.Vision.RenderSize (decision 3), whichever the realm can afford.
    std::vector<std::string> sizeErrors;
    Camera.RenderSizes = Vision::ParseRenderSizes(sConfigMgr->GetOption<std::string>("Animus.Vision.RenderSizes",
        Vision::DEFAULT_RENDER_SIZES), Camera, Camera.RenderWeights, sizeErrors);
    for (std::string const& error : sizeErrors)
        LOG_ERROR("module.animus", "Animus.Vision.RenderSizes: {}", error);
    if (Camera.RenderSizes.empty())
    {
        Camera.RenderSizes = { Vision::Resolution{ Camera.Width, Camera.Height } };
        Camera.RenderWeights = { 1.0f };
    }
    sizeErrors.clear();
    std::vector<float> weights;
    std::vector<Vision::Resolution> const chosen = Vision::ParseRenderSizes(sConfigMgr->GetOption<std::string>(
        "Animus.Vision.RenderSize", "64x32"), Camera, weights, sizeErrors);
    for (std::string const& error : sizeErrors)
        LOG_ERROR("module.animus", "Animus.Vision.RenderSize: {}", error);
    if (chosen.size() > 1)
        LOG_ERROR("module.animus", "Animus.Vision.RenderSize names {} sizes; every follower casts at the first, {}x{}",
            chosen.size(), chosen.front().Width, chosen.front().Height);
    Render = chosen.empty() ? Camera.RenderSizes.front() : chosen.front();
    if (std::find_if(Camera.RenderSizes.begin(), Camera.RenderSizes.end(), [this](Vision::Resolution const& size)
        { return size.Width == Render.Width && size.Height == Render.Height; }) == Camera.RenderSizes.end())
        LOG_ERROR("module.animus", "Animus.Vision.RenderSize {}x{} is not one of Animus.Vision.RenderSizes: the models "
            "never saw frames cast at it", Render.Width, Render.Height);

    // The mental map: its caps, and no keep roll (the follow driver keeps it across a roll-over and nowhere else).
    MapRun = Vision::MapRunSettings();
    MapRun.Caps.MaxTiles = uint32(Ranged("Animus.Map.MaxTiles", 4096.0f, 1.0f, 65536.0f));
    MapRun.Caps.CoarseTiles = uint32(Ranged("Animus.Map.CoarseTiles", 0.0f, 0.0f, 65536.0f));
    MapRun.KeepShare = 0.0f;
    MapRun.AgeOffsetSeconds = 0.0f;
    ClusterMinCells = uint32(Ranged("Animus.Map.ClusterMinCells", 2.0f, 1.0f, 1000.0f));
    PocketMinCells = uint32(Ranged("Animus.Map.PocketMinCells", 6.0f, 1.0f, 1000.0f));

    // The follow driver: the decision clock and the tuning move4_follow trained with (stage.json "tuning"). The
    // runtime's ActionTuning defaults are the forge's, apart from Actions.Repeat (0.02 in training; a reward weight,
    // which nothing here reads, set for the record).
    FollowTuning = Follow::FollowSettings();
    Follow::FollowSettings& tuning = FollowTuning;
    tuning.DecisionMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Animus.Curriculum.DecisionMs", 250));
    tuning.Actions.Repeat = 0.02f;
    tuning.Actions.RepeatMs = sConfigMgr->GetOption<uint32>("Animus.Curriculum.Actions.RepeatMs", 1000);
    tuning.Actions.MoveRepeatMs = sConfigMgr->GetOption<uint32>("Animus.Curriculum.Actions.MoveRepeatMs", 300);
    tuning.Actions.StopCastMinMs = sConfigMgr->GetOption<uint32>("Animus.Curriculum.Actions.StopCastMinMs", 500);
    tuning.Actions.RecastAfterStopMs =
        sConfigMgr->GetOption<uint32>("Animus.Curriculum.Actions.RecastAfterStopMs", 2000);
    tuning.Actions.ModeLockMs = sConfigMgr->GetOption<uint32>("Animus.Curriculum.Actions.ModeLockMs", 5000);
    tuning.Options.RestMaxMs = sConfigMgr->GetOption<uint32>("Animus.Curriculum.Options.RestMaxMs", 30000);
    tuning.Options.HoldInterruptMs = sConfigMgr->GetOption<uint32>("Animus.Curriculum.Options.HoldInterruptMs", 10000);
    tuning.Options.JitterDecayMs = sConfigMgr->GetOption<uint32>("Animus.Curriculum.Options.JitterDecayMs", 2500);
    tuning.MinimapYards = Ranged("Animus.Curriculum.PartyFollow.MinimapYards", 60.0f, 1.0f, 500.0f);
    tuning.RespawnDelayMs = sConfigMgr->GetOption<uint32>("Animus.Curriculum.Respawn.DelayMs", 10000);
    tuning.EpisodeMs = uint32(Ranged("Animus.Follow.EpisodeSeconds", 300.0f, 10.0f, 86400.0f) * 1000.0f);
    tuning.Render = Render;
}

std::string Animus::AnimusConfig::SceneDir() const
{
    return (std::filesystem::path(DataDir) / "scenes").lexically_normal().string();
}

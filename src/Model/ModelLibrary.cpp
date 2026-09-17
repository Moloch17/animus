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

#include "ModelLibrary.h"
#include "Layout.h"
#include "Log.h"
#include "StringFormat.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    std::string TrimEnd(std::string text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
            text.pop_back();
        return text;
    }
}

void Animus::ModelLibrary::Reset(std::string dir)
{
    _dir = std::move(dir);
    _models.clear();
}

Animus::MlpPolicy* Animus::ModelLibrary::Find(Curriculum::Layout const& layout, std::string& error)
{
    std::string const name = layout.ModelName();
    auto [itr, inserted] = _models.try_emplace(name);
    Entry& entry = itr->second;

    if (!inserted)
    {
        error = entry.Error;
        return entry.Policy.IsLoaded() ? &entry.Policy : nullptr;
    }

    std::filesystem::path const base = std::filesystem::path(_dir) / name;
    std::string const modelPath = base.string() + ".amdl";
    std::string const manifestPath = base.string() + ".json";

    std::ifstream manifestFile(manifestPath);
    if (!manifestFile)
        entry.Error = Acore::StringFormat("no layout manifest {} beside the model", manifestPath);
    else
    {
        std::ostringstream manifest;
        manifest << manifestFile.rdbuf();
        if (TrimEnd(manifest.str()) != TrimEnd(layout.Manifest()))
            entry.Error = Acore::StringFormat("{} was trained on a different {} layout than this server builds "
                "(its manifest differs; export a model trained with this build)", modelPath, name);
    }

    std::string loadError;
    if (entry.Error.empty() && !entry.Policy.Load(modelPath, name, layout.ObsDim, layout.NumActions, loadError))
        entry.Error = loadError;

    if (!entry.Error.empty())
    {
        LOG_ERROR("module.animus", "Animus model {} not loaded: {}", name, entry.Error);
        error = entry.Error;
        return nullptr;
    }

    LOG_INFO("module.animus", "Animus loaded model {} from {} ({})", name, modelPath, entry.Policy.Describe());
    return &entry.Policy;
}

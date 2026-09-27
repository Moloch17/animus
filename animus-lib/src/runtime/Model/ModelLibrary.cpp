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
#include <boost/json.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    /// Where two manifests first part: the path to the first value that differs ("blocks[2].features.ground_probe
    /// .source: \"live\" here, \"geometry\" in the model's"), or what could not be read. What a refused model needs
    /// to be fixed is which setting or revision differs, not only that something does.
    std::string FirstDifference(boost::json::value const& server, boost::json::value const& model, std::string path)
    {
        if (server.kind() != model.kind())
            return Acore::StringFormat("{}: {} here, {} in the model's", path.empty() ? "(root)" : path,
                boost::json::serialize(server), boost::json::serialize(model));
        if (server.is_object())
        {
            boost::json::object const& a = server.get_object();
            boost::json::object const& b = model.get_object();
            for (auto const& [key, value] : a)
            {
                std::string const at = path.empty() ? std::string(key) : path + "." + std::string(key);
                auto const other = b.find(key);
                if (other == b.end())
                    return at + ": only this server has it";
                if (std::string const found = FirstDifference(value, other->value(), at); !found.empty())
                    return found;
            }
            for (auto const& [key, value] : b)
                if (!a.contains(key))
                    return (path.empty() ? std::string(key) : path + "." + std::string(key)) + ": only the model has it";
            return {};
        }
        if (server.is_array())
        {
            boost::json::array const& a = server.get_array();
            boost::json::array const& b = model.get_array();
            for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
                if (std::string const found = FirstDifference(a[i], b[i], Acore::StringFormat("{}[{}]", path, i));
                    !found.empty())
                    return found;
            if (a.size() != b.size())
                return Acore::StringFormat("{}: {} entries here, {} in the model's", path, a.size(), b.size());
            return {};
        }
        if (server != model)
            return Acore::StringFormat("{}: {} here, {} in the model's", path, boost::json::serialize(server),
                boost::json::serialize(model));
        return {};
    }

    std::string ManifestDifference(std::string const& server, std::string const& model)
    {
        boost::system::error_code error;
        boost::json::value const ours = boost::json::parse(server, error);
        if (error)
            return "this server's manifest does not parse";
        boost::json::value const theirs = boost::json::parse(model, error);
        if (error)
            return "the model's manifest does not parse: " + error.message();
        std::string found = FirstDifference(ours, theirs, "");
        return found.empty() ? "they differ only in formatting" : found;
    }

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
                "(its manifest differs at {}; export a model trained with this build)", modelPath, name,
                ManifestDifference(layout.Manifest(), manifest.str()));
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

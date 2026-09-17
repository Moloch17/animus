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

#include "MlpPolicy.h"
#include "StringFormat.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace
{
    constexpr char AMDL_MAGIC[4] = { 'A', 'M', 'D', 'L' };
    constexpr uint32 AMDL_VERSION = 1;

    /// Guards against a corrupt header asking for gigabytes.
    constexpr uint32 MAX_LAYER_WIDTH = 1 << 16;
    constexpr uint32 MAX_LAYERS = 64;

    /// Little-endian reader over a whole file; every read fails once the data runs out.
    class Reader
    {
    public:
        explicit Reader(std::vector<char> const& data) : _data(data) { }

        bool Read(char* out, std::size_t size)
        {
            if (_data.size() - _offset < size)
                return false;

            std::memcpy(out, _data.data() + _offset, size);
            _offset += size;
            return true;
        }

        template <typename T>
        bool Read(T& value)
        {
            return Read(reinterpret_cast<char*>(&value), sizeof(T));
        }

        bool ReadFloats(std::vector<float>& out, std::size_t count)
        {
            out.resize(count);
            return Read(reinterpret_cast<char*>(out.data()), count * sizeof(float));
        }

        [[nodiscard]] bool AtEnd() const { return _offset == _data.size(); }

    private:
        std::vector<char> const& _data;
        std::size_t _offset = 0;
    };
}

bool Animus::MlpPolicy::Load(std::string const& path, std::string const& scenario, uint32 obsDim, uint32 numActions,
    std::string& error)
{
    Unload();

    // The file stores little-endian values and is read by memcpy.
    if (std::endian::native != std::endian::little)
    {
        error = "big-endian hosts are not supported";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = Acore::StringFormat("cannot open {}", path);
        return false;
    }

    std::vector<char> const data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Reader reader(data);

    char magic[4];
    uint32 version = 0;
    uint16 nameLength = 0;
    if (!reader.Read(magic, sizeof(magic)) || std::memcmp(magic, AMDL_MAGIC, sizeof(magic)) != 0)
    {
        error = Acore::StringFormat("{} is not an .amdl model", path);
        return false;
    }

    if (!reader.Read(version) || version != AMDL_VERSION)
    {
        error = Acore::StringFormat("{} has model version {}, expected {}", path, version, AMDL_VERSION);
        return false;
    }

    std::string name;
    if (!reader.Read(nameLength))
    {
        error = Acore::StringFormat("{} is truncated", path);
        return false;
    }

    name.resize(nameLength);
    uint32 fileObsDim = 0;
    uint32 numAgents = 0;
    uint32 fileNumActions = 0;
    uint32 layerCount = 0;
    if (!reader.Read(name.data(), nameLength) || !reader.Read(fileObsDim) || !reader.Read(numAgents)
        || !reader.Read(fileNumActions) || !reader.Read(layerCount))
    {
        error = Acore::StringFormat("{} is truncated", path);
        return false;
    }

    if (name != scenario || fileObsDim != obsDim || fileNumActions != numActions)
    {
        error = Acore::StringFormat("{} was trained for {} (obs {}, actions {}); this build expects {} "
            "(obs {}, actions {})", path, name, fileObsDim, fileNumActions, scenario, obsDim, numActions);
        return false;
    }

    if (numAgents == 0 || numAgents > MAX_LAYER_WIDTH || layerCount == 0 || layerCount > MAX_LAYERS)
    {
        error = Acore::StringFormat("{} has an invalid header ({} agents, {} layers)", path, numAgents, layerCount);
        return false;
    }

    std::vector<Layer> layers(layerCount);
    uint32 expectedIn = obsDim + numAgents;
    uint32 widest = expectedIn;

    for (uint32 index = 0; index < layerCount; ++index)
    {
        Layer& layer = layers[index];
        if (!reader.Read(layer.In) || !reader.Read(layer.Out))
        {
            error = Acore::StringFormat("{} is truncated in layer {}", path, index);
            return false;
        }

        if (layer.In != expectedIn || layer.Out == 0 || layer.Out > MAX_LAYER_WIDTH)
        {
            error = Acore::StringFormat("{} layer {} is {} -> {}, expected {} inputs", path, index, layer.In,
                layer.Out, expectedIn);
            return false;
        }

        if (!reader.ReadFloats(layer.Weight, std::size_t(layer.Out) * layer.In)
            || !reader.ReadFloats(layer.Bias, layer.Out))
        {
            error = Acore::StringFormat("{} is truncated in layer {}", path, index);
            return false;
        }

        expectedIn = layer.Out;
        widest = std::max(widest, layer.Out);
    }

    if (expectedIn != numActions)
    {
        error = Acore::StringFormat("{} outputs {} logits, expected {}", path, expectedIn, numActions);
        return false;
    }

    if (!reader.AtEnd())
    {
        error = Acore::StringFormat("{} has trailing data", path);
        return false;
    }

    _obsDim = obsDim;
    _numAgents = numAgents;
    _numActions = numActions;
    _layers = std::move(layers);
    _scratchA.assign(widest, 0.0f);
    _scratchB.assign(widest, 0.0f);
    return true;
}

void Animus::MlpPolicy::Unload()
{
    _layers.clear();
    _scratchA.clear();
    _scratchB.clear();
    _obsDim = 0;
    _numAgents = 0;
    _numActions = 0;
}

std::string Animus::MlpPolicy::Describe() const
{
    if (_layers.empty())
        return "not loaded";

    std::string shape = Acore::StringFormat("{}+{}", _obsDim, _numAgents);
    for (Layer const& layer : _layers)
        shape += Acore::StringFormat(" -> {}", layer.Out);

    return shape;
}

int32 Animus::MlpPolicy::Decide(float const* obs, uint8 const* mask)
{
    if (_layers.empty())
        return 0;

    // Input: observation, then the one-hot id of agent 0.
    float* in = _scratchA.data();
    float* out = _scratchB.data();
    std::copy(obs, obs + _obsDim, in);
    std::fill(in + _obsDim, in + _obsDim + _numAgents, 0.0f);
    in[_obsDim] = 1.0f;

    for (std::size_t index = 0; index < _layers.size(); ++index)
    {
        Layer const& layer = _layers[index];
        bool const hidden = index + 1 < _layers.size();

        for (uint32 row = 0; row < layer.Out; ++row)
        {
            float const* weights = layer.Weight.data() + std::size_t(row) * layer.In;
            float sum = layer.Bias[row];
            for (uint32 col = 0; col < layer.In; ++col)
                sum += weights[col] * in[col];

            out[row] = hidden ? std::tanh(sum) : sum;
        }

        std::swap(in, out);
    }

    // `in` now holds the logits.
    int32 best = 0;
    float bestLogit = -std::numeric_limits<float>::infinity();
    bool anyAllowed = false;

    for (uint32 action = 0; action < _numActions; ++action)
    {
        if (!mask[action])
            continue;

        if (!anyAllowed || in[action] > bestLogit)
        {
            best = int32(action);
            bestLogit = in[action];
            anyAllowed = true;
        }
    }

    return anyAllowed ? best : 0;
}

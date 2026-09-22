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
    constexpr uint32 AMDL_VERSION = 2;

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

        // Every layer's inputs are the layer before's outputs, except the action head's: with a memory it
        // reads the GRU's state rather than the trunk's output, and the memory's size is only known further
        // down the file. The head is checked there instead, against the memory or the trunk as it applies.
        bool const head = layerCount > 1 && index + 1 == layerCount;
        if ((!head && layer.In != expectedIn) || layer.In == 0 || layer.In > MAX_LAYER_WIDTH
            || layer.Out == 0 || layer.Out > MAX_LAYER_WIDTH)
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

    // The memory (a GRU between the trunk and the action head) and the goals, either of which a model may not have.
    uint32 recurrentSize = 0;
    std::vector<float> memoryWeightIn;
    std::vector<float> memoryWeightHidden;
    std::vector<float> memoryBiasIn;
    std::vector<float> memoryBiasHidden;
    if (!reader.Read(recurrentSize) || recurrentSize > MAX_LAYER_WIDTH)
    {
        error = Acore::StringFormat("{} is truncated before its memory", path);
        return false;
    }

    uint32 const features = layers.size() > 1 ? layers[layers.size() - 2].Out : 0;
    if (recurrentSize)
    {
        if (layers.back().In != recurrentSize)
        {
            error = Acore::StringFormat("{} has a memory of {} but its action head takes {} inputs", path,
                recurrentSize, layers.back().In);
            return false;
        }

        if (!reader.ReadFloats(memoryWeightIn, std::size_t(3) * recurrentSize * features)
            || !reader.ReadFloats(memoryWeightHidden, std::size_t(3) * recurrentSize * recurrentSize)
            || !reader.ReadFloats(memoryBiasIn, std::size_t(3) * recurrentSize)
            || !reader.ReadFloats(memoryBiasHidden, std::size_t(3) * recurrentSize))
        {
            error = Acore::StringFormat("{} is truncated in its memory", path);
            return false;
        }
    }
    else if (layers.size() > 1 && layers.back().In != features)
    {
        error = Acore::StringFormat("{} has no memory, so its action head should take the trunk's {} outputs, "
            "not {}", path, features, layers.back().In);
        return false;
    }

    uint32 goalCount = 0;
    uint32 goalEvery = 0;
    std::vector<float> goalWeight;
    std::vector<float> goalBias;
    std::vector<float> goalEmbedding;
    if (!reader.Read(goalCount) || !reader.Read(goalEvery) || goalCount > MAX_LAYER_WIDTH)
    {
        error = Acore::StringFormat("{} is truncated before its goals", path);
        return false;
    }

    if (goalCount)
    {
        uint32 const width = layers.back().In;
        if (!reader.ReadFloats(goalWeight, std::size_t(goalCount) * width)
            || !reader.ReadFloats(goalBias, goalCount)
            || !reader.ReadFloats(goalEmbedding, std::size_t(goalCount) * width))
        {
            error = Acore::StringFormat("{} is truncated in its goals", path);
            return false;
        }
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
    _recurrentSize = recurrentSize;
    _memoryWeightIn = std::move(memoryWeightIn);
    _memoryWeightHidden = std::move(memoryWeightHidden);
    _memoryBiasIn = std::move(memoryBiasIn);
    _memoryBiasHidden = std::move(memoryBiasHidden);
    _goalCount = goalCount;
    _goalEvery = std::max<uint32>(1, goalEvery);
    _goalWeight = std::move(goalWeight);
    _goalBias = std::move(goalBias);
    _goalEmbedding = std::move(goalEmbedding);
    widest = std::max(widest, recurrentSize);
    _scratchA.assign(widest, 0.0f);
    _scratchB.assign(widest, 0.0f);
    _gates.assign(std::size_t(3) * recurrentSize, 0.0f);
    _hiddenGates.assign(std::size_t(3) * recurrentSize, 0.0f);
    return true;
}

void Animus::MlpPolicy::Unload()
{
    _layers.clear();
    _scratchA.clear();
    _scratchB.clear();
    _gates.clear();
    _hiddenGates.clear();
    _memoryWeightIn.clear();
    _memoryWeightHidden.clear();
    _memoryBiasIn.clear();
    _memoryBiasHidden.clear();
    _goalWeight.clear();
    _goalBias.clear();
    _goalEmbedding.clear();
    _obsDim = 0;
    _numAgents = 0;
    _numActions = 0;
    _recurrentSize = 0;
    _goalCount = 0;
    _goalEvery = 0;
}

std::string Animus::MlpPolicy::Describe() const
{
    if (_layers.empty())
        return "not loaded";

    std::string shape = Acore::StringFormat("{}+{}", _obsDim, _numAgents);
    for (std::size_t index = 0; index < _layers.size(); ++index)
    {
        if (_recurrentSize && index + 1 == _layers.size())
            shape += Acore::StringFormat(" -> memory {}", _recurrentSize);
        shape += Acore::StringFormat(" -> {}", _layers[index].Out);
    }

    if (_goalCount)
        shape += Acore::StringFormat(" ({} goals every {} decisions)", _goalCount, _goalEvery);

    return shape;
}

namespace
{
    float Sigmoid(float value)
    {
        return 1.0f / (1.0f + std::exp(-value));
    }
}

int32 Animus::MlpPolicy::Decide(float const* obs, uint8 const* mask, State* state)
{
    if (_layers.empty())
        return 0;

    // Input: observation, then the one-hot id of agent 0.
    float* in = _scratchA.data();
    float* out = _scratchB.data();
    std::copy(obs, obs + _obsDim, in);
    std::fill(in + _obsDim, in + _obsDim + _numAgents, 0.0f);
    in[_obsDim] = 1.0f;

    // Every layer but the action head, which reads the features the memory and the goal are applied to.
    std::size_t const trunkLayers = _layers.size() - 1;
    for (std::size_t index = 0; index < trunkLayers; ++index)
    {
        Layer const& layer = _layers[index];
        for (uint32 row = 0; row < layer.Out; ++row)
        {
            float const* weights = layer.Weight.data() + std::size_t(row) * layer.In;
            float sum = layer.Bias[row];
            for (uint32 col = 0; col < layer.In; ++col)
                sum += weights[col] * in[col];

            out[row] = std::tanh(sum);
        }

        std::swap(in, out);
    }

    uint32 features = _layers.back().In;
    if (_recurrentSize)
    {
        // One GRU cell over the trunk's output and what this seat remembers (torch.nn.GRUCell).
        uint32 const size = _recurrentSize;
        uint32 const trunkOut = _layers[trunkLayers - 1].Out;
        std::vector<float>* memory = state ? &state->Memory : nullptr;
        if (memory && memory->size() != size)
            memory->assign(size, 0.0f);

        float const* carried = memory ? memory->data() : nullptr;
        for (uint32 row = 0; row < 3 * size; ++row)
        {
            float const* input = _memoryWeightIn.data() + std::size_t(row) * trunkOut;
            float sum = _memoryBiasIn[row];
            for (uint32 col = 0; col < trunkOut; ++col)
                sum += input[col] * in[col];
            _gates[row] = sum;

            float const* hidden = _memoryWeightHidden.data() + std::size_t(row) * size;
            float recurrent = _memoryBiasHidden[row];
            if (carried)
                for (uint32 col = 0; col < size; ++col)
                    recurrent += hidden[col] * carried[col];
            _hiddenGates[row] = recurrent;
        }

        // r and z open on both parts; the candidate takes the reset gate on the remembered part only.
        for (uint32 row = 0; row < size; ++row)
        {
            float const reset = Sigmoid(_gates[row] + _hiddenGates[row]);
            float const update = Sigmoid(_gates[size + row] + _hiddenGates[size + row]);
            float const candidate = std::tanh(_gates[2 * size + row] + reset * _hiddenGates[2 * size + row]);
            float const previous = carried ? carried[row] : 0.0f;
            out[row] = (1.0f - update) * candidate + update * previous;
        }

        std::swap(in, out);
        if (memory)
            std::copy(in, in + size, memory->begin());
        features = size;
    }

    if (_goalCount)
    {
        // A goal is chosen on its own clock and kept in between; its embedding is added to the features.
        uint32 goal = state ? state->Goal : 0;
        if (!state || state->Age % _goalEvery == 0)
        {
            float best = -std::numeric_limits<float>::infinity();
            for (uint32 candidate = 0; candidate < _goalCount; ++candidate)
            {
                float const* weights = _goalWeight.data() + std::size_t(candidate) * features;
                float sum = _goalBias[candidate];
                for (uint32 col = 0; col < features; ++col)
                    sum += weights[col] * in[col];

                if (sum > best)
                {
                    best = sum;
                    goal = candidate;
                }
            }
        }

        if (state)
        {
            state->Age = state->Age % _goalEvery == 0 ? 1 : state->Age + 1;
            state->Goal = goal;
        }

        float const* embedding = _goalEmbedding.data() + std::size_t(goal) * features;
        for (uint32 col = 0; col < features; ++col)
            in[col] += embedding[col];
    }

    {
        Layer const& head = _layers.back();
        for (uint32 row = 0; row < head.Out; ++row)
        {
            float const* weights = head.Weight.data() + std::size_t(row) * head.In;
            float sum = head.Bias[row];
            for (uint32 col = 0; col < head.In; ++col)
                sum += weights[col] * in[col];

            out[row] = sum;
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

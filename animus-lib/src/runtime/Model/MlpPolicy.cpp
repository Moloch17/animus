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
    constexpr uint32 AMDL_VERSION = 9;
    /// The oldest format still read: a version 7 model is a version 8 one without the seat sets, an 8 a 9 without the
    /// attention flag.
    constexpr uint32 AMDL_OLDEST_VERSION = 7;

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

    if (!reader.Read(version) || version < AMDL_OLDEST_VERSION || version > AMDL_VERSION)
    {
        error = Acore::StringFormat("{} has model version {}, expected {} to {}", path, version, AMDL_OLDEST_VERSION,
            AMDL_VERSION);
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

    // The predictions fed back, and the slow loop (Component P layer 2, Component D).
    uint32 const featureWidth = layers.back().In;
    uint32 foresightOutputs = 0;
    std::vector<float> foresightWeight, foresightBias, feedbackWeight, feedbackBias;
    if (!reader.Read(foresightOutputs) || foresightOutputs > MAX_LAYER_WIDTH)
    {
        error = Acore::StringFormat("{} is truncated before its predictions", path);
        return false;
    }
    if (foresightOutputs && (!reader.ReadFloats(foresightWeight, std::size_t(foresightOutputs) * featureWidth)
        || !reader.ReadFloats(foresightBias, foresightOutputs)
        || !reader.ReadFloats(feedbackWeight, std::size_t(featureWidth) * foresightOutputs)
        || !reader.ReadFloats(feedbackBias, featureWidth)))
    {
        error = Acore::StringFormat("{} is truncated in its predictions", path);
        return false;
    }
    uint32 slowSize = 0;
    std::vector<float> slowWeightIn, slowWeightHidden, slowBiasIn, slowBiasHidden;
    if (!reader.Read(slowSize) || slowSize > MAX_LAYER_WIDTH)
    {
        error = Acore::StringFormat("{} is truncated before its slow loop", path);
        return false;
    }
    if (slowSize && (!reader.ReadFloats(slowWeightIn, std::size_t(3) * slowSize * featureWidth)
        || !reader.ReadFloats(slowWeightHidden, std::size_t(3) * slowSize * slowSize)
        || !reader.ReadFloats(slowBiasIn, std::size_t(3) * slowSize)
        || !reader.ReadFloats(slowBiasHidden, std::size_t(3) * slowSize)))
    {
        error = Acore::StringFormat("{} is truncated in its slow loop", path);
        return false;
    }

    uint32 goalCount = 0;
    uint32 goalTargets = 1;
    uint32 goalEvery = 0;
    bool lookahead = false;
    Factored success, duration;
    float lookaheadWeight[2] = { 0.0f, 0.0f };
    std::vector<float> kindWeight, kindBias, targetWeight, targetBias, pair, kindEmbedding, targetEmbedding;
    std::vector<float> kindScale, targetScale;
    std::vector<uint8> accepts;
    int32 goalBlockAt = -1;
    uint32 goalSlots = 1;
    std::vector<float> slotBias, drawn, noneBias;
    float gate = 0.0f;
    if (!reader.Read(goalCount) || !reader.Read(goalTargets) || !reader.Read(goalEvery)
        || goalCount > MAX_LAYER_WIDTH || goalTargets == 0 || goalTargets > MAX_LAYER_WIDTH)
    {
        error = Acore::StringFormat("{} is truncated before its goals", path);
        return false;
    }

    if (goalCount)
    {
        uint32 const width = layers.back().In;
        uint32 const goalWidth = slowSize ? slowSize : width;
        std::size_t const joint = std::size_t(goalCount) * goalTargets;
        bool ok = reader.ReadFloats(kindWeight, std::size_t(goalCount) * goalWidth)
            && reader.ReadFloats(kindBias, goalCount);
        if (ok && goalTargets > 1)
            ok = reader.ReadFloats(targetWeight, std::size_t(goalTargets) * goalWidth)
                && reader.ReadFloats(targetBias, goalTargets);
        ok = ok && reader.ReadFloats(pair, joint);
        if (ok)
        {
            accepts.resize(joint);
            ok = reader.Read(reinterpret_cast<char*>(accepts.data()), joint) && reader.Read(goalBlockAt);
        }
        ok = ok && reader.ReadFloats(kindEmbedding, std::size_t(goalCount) * width);
        if (ok && goalTargets > 1)
            ok = reader.ReadFloats(targetEmbedding, std::size_t(goalTargets) * width);
        uint8 hasLookahead = 0;
        ok = ok && reader.Read(hasLookahead);
        lookahead = hasLookahead != 0;
        auto const readFactored = [&](Factored& part)
        {
            bool read = reader.ReadFloats(part.KindWeight, std::size_t(goalCount) * goalWidth)
                && reader.ReadFloats(part.KindBias, goalCount);
            if (read && goalTargets > 1)
                read = reader.ReadFloats(part.TargetWeight, std::size_t(goalTargets) * goalWidth)
                    && reader.ReadFloats(part.TargetBias, goalTargets);
            return read && reader.ReadFloats(part.Pair, joint);
        };
        if (ok && lookahead)
            ok = readFactored(success) && readFactored(duration) && reader.Read(lookaheadWeight[0])
                && reader.Read(lookaheadWeight[1]);
        ok = ok && reader.Read(goalSlots) && goalSlots >= 1 && goalSlots <= 8;
        if (ok && goalSlots > 1)
            ok = reader.ReadFloats(slotBias, std::size_t(goalSlots - 1) * goalWidth)
                && reader.ReadFloats(drawn, (joint + 1) * goalWidth) && reader.ReadFloats(noneBias, goalSlots - 1)
                && reader.Read(gate);
        // Version 7: the goal's scale on the action head's features (FiLM).
        ok = ok && reader.ReadFloats(kindScale, std::size_t(goalCount) * width);
        if (ok && goalTargets > 1)
            ok = reader.ReadFloats(targetScale, std::size_t(goalTargets) * width);
        if (!ok)
        {
            error = Acore::StringFormat("{} is truncated in its goals", path);
            return false;
        }
        // The goal block's columns: kinds, targets, ended, reached, and with two goals the secondary ending, the
        // event, the director's primary and what was achieved.
        uint32 const blockWidth = goalCount + goalTargets + 2 + (goalSlots > 1 ? 3 + 2 * (goalCount + goalTargets) : 0);
        if (goalBlockAt >= 0 && uint32(goalBlockAt) + blockWidth > obsDim)
        {
            error = Acore::StringFormat("{} puts its goal block at {}, past its {} observations", path, goalBlockAt,
                obsDim);
            return false;
        }
    }

    // The director's sets.
    uint8 hasSets = 0;
    uint32 embed = 0;
    SetEncoder members, enemies;
    std::vector<float> poolWeight, poolBias;
    std::vector<Pointer> pointers;
    if (!reader.Read(hasSets))
    {
        error = Acore::StringFormat("{} is truncated before its sets", path);
        return false;
    }
    if (hasSets)
    {
        bool ok = reader.Read(embed) && embed > 0 && embed <= MAX_LAYER_WIDTH;
        for (SetEncoder* set : { &members, &enemies })
            ok = ok && reader.Read(set->First) && reader.Read(set->Slots) && reader.Read(set->Width)
                && reader.Read(set->Present) && set->Width <= MAX_LAYER_WIDTH && set->Present < set->Width
                && set->First + set->Slots * set->Width <= obsDim
                && reader.ReadFloats(set->W1, std::size_t(embed) * set->Width) && reader.ReadFloats(set->B1, embed)
                && reader.ReadFloats(set->W2, std::size_t(embed) * embed) && reader.ReadFloats(set->B2, embed);
        uint32 const adapterOut = layers.front().Out;
        ok = ok && reader.ReadFloats(poolWeight, std::size_t(adapterOut) * 4 * embed)
            && reader.ReadFloats(poolBias, adapterOut);
        uint32 count = 0;
        ok = ok && reader.Read(count) && count <= 16;
        for (uint32 i = 0; ok && i < count; ++i)
        {
            Pointer pointer;
            ok = reader.Read(pointer.First) && reader.Read(pointer.Over) && pointer.Over < 2
                && reader.ReadFloats(pointer.Weight, std::size_t(embed) * featureWidth)
                && reader.ReadFloats(pointer.Bias, embed)
                && pointer.First + (pointer.Over ? enemies.Slots : members.Slots) <= numActions;
            pointers.push_back(std::move(pointer));
        }
        if (!ok)
        {
            error = Acore::StringFormat("{} is truncated or inconsistent in its sets", path);
            return false;
        }
    }

    // A seat layout's sets (version 8).
    uint8 hasSeatSets = 0;
    uint32 seatEmbed = 0;
    std::vector<SeatSet> seatSets;
    std::vector<float> seatPoolWeight, seatPoolBias;
    std::vector<SeatPointer> seatPointers;
    uint8 attending = 0;
    SeatAttention attention;
    if (version >= 8 && !reader.Read(hasSeatSets))
    {
        error = Acore::StringFormat("{} is truncated before its seat sets", path);
        return false;
    }
    if (hasSeatSets)
    {
        uint32 count = 0;
        bool ok = reader.Read(seatEmbed) && seatEmbed > 0 && seatEmbed <= MAX_LAYER_WIDTH && reader.Read(count)
            && count > 0 && count <= 8 && (version < 9 || reader.Read(attending));
        for (uint32 i = 0; ok && i < count; ++i)
        {
            SeatSet set;
            uint32 segments = 0;
            ok = reader.Read(set.Slots) && reader.Read(set.Width) && reader.Read(set.Present)
                && reader.Read(set.PresentStride) && reader.Read(segments) && set.Slots > 0 && set.Slots <= 64
                && set.Width > 0 && set.Width <= MAX_LAYER_WIDTH && segments > 0 && segments <= 4
                && set.Present + (set.Slots - 1) * set.PresentStride < obsDim;
            uint32 width = 0;
            for (uint32 s = 0; ok && s < segments; ++s)
            {
                uint32 first = 0, stride = 0;
                ok = reader.Read(first) && reader.Read(stride) && stride > 0 && first + set.Slots * stride <= obsDim;
                width += stride;
                set.Segments.emplace_back(first, stride);
            }
            ok = ok && width == set.Width && reader.ReadFloats(set.W1, std::size_t(seatEmbed) * set.Width)
                && reader.ReadFloats(set.B1, seatEmbed) && reader.ReadFloats(set.W2, std::size_t(seatEmbed) * seatEmbed)
                && reader.ReadFloats(set.B2, seatEmbed);
            seatSets.push_back(std::move(set));
        }
        if (ok && attending)
        {
            std::size_t const e = seatEmbed;
            SeatAttention& a = attention;
            ok = reader.Read(a.Heads) && a.Heads > 0 && seatEmbed % a.Heads == 0
                && reader.ReadFloats(a.TypeEmbed, count * e) && reader.ReadFloats(a.OwnToken, e)
                && reader.ReadFloats(a.NormAttendW, e) && reader.ReadFloats(a.NormAttendB, e)
                && reader.ReadFloats(a.InProjW, 3 * e * e) && reader.ReadFloats(a.InProjB, 3 * e)
                && reader.ReadFloats(a.OutProjW, e * e) && reader.ReadFloats(a.OutProjB, e)
                && reader.ReadFloats(a.NormMixW, e) && reader.ReadFloats(a.NormMixB, e)
                && reader.ReadFloats(a.MixInW, 2 * e * e) && reader.ReadFloats(a.MixInB, 2 * e)
                && reader.ReadFloats(a.MixOutW, 2 * e * e) && reader.ReadFloats(a.MixOutB, e);
        }
        uint32 const adapterOut = layers.front().Out;
        std::size_t const pooledWidth = std::size_t(2) * seatEmbed * count + (attending ? seatEmbed : 0);
        ok = ok && reader.ReadFloats(seatPoolWeight, std::size_t(adapterOut) * pooledWidth)
            && reader.ReadFloats(seatPoolBias, adapterOut);
        uint32 pointerCount = 0;
        ok = ok && reader.Read(pointerCount) && pointerCount <= 16;
        for (uint32 i = 0; ok && i < pointerCount; ++i)
        {
            SeatPointer pointer;
            ok = reader.Read(pointer.First) && reader.Read(pointer.Set) && pointer.Set < count
                && reader.ReadFloats(pointer.Weight, std::size_t(seatEmbed) * featureWidth)
                && reader.ReadFloats(pointer.Bias, seatEmbed)
                && pointer.First + seatSets[pointer.Set].Slots <= numActions;
            seatPointers.push_back(std::move(pointer));
        }
        if (!ok)
        {
            error = Acore::StringFormat("{} is truncated or inconsistent in its seat sets", path);
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
    _goalTargets = goalTargets;
    _goalEvery = std::max<uint32>(1, goalEvery);
    _kindWeight = std::move(kindWeight);
    _kindBias = std::move(kindBias);
    _targetWeight = std::move(targetWeight);
    _targetBias = std::move(targetBias);
    _pair = std::move(pair);
    _accepts = std::move(accepts);
    _goalBlockAt = goalBlockAt;
    _kindEmbedding = std::move(kindEmbedding);
    _targetEmbedding = std::move(targetEmbedding);
    _kindScale = std::move(kindScale);
    _targetScale = std::move(targetScale);
    _targetScores.assign(goalTargets, 0.0f);
    _lookahead = lookahead;
    _success = std::move(success);
    _duration = std::move(duration);
    _lookaheadWeight[0] = lookaheadWeight[0];
    _lookaheadWeight[1] = lookaheadWeight[1];
    _goalSlots = goalCount ? goalSlots : 1;
    _slotBias = std::move(slotBias);
    _drawn = std::move(drawn);
    _noneBias = std::move(noneBias);
    _gate = gate;
    _shifted.assign(slowSize ? slowSize : featureWidth, 0.0f);
    _foresightOutputs = foresightOutputs;
    _foresightWeight = std::move(foresightWeight);
    _foresightBias = std::move(foresightBias);
    _feedbackWeight = std::move(feedbackWeight);
    _feedbackBias = std::move(feedbackBias);
    _predictions.assign(foresightOutputs, 0.0f);
    _raw.assign(featureWidth, 0.0f);
    _slowSize = slowSize;
    _slowWeightIn = std::move(slowWeightIn);
    _slowWeightHidden = std::move(slowWeightHidden);
    _slowBiasIn = std::move(slowBiasIn);
    _slowBiasHidden = std::move(slowBiasHidden);
    _slowGates.assign(std::size_t(3) * slowSize, 0.0f);
    _sets = hasSets != 0;
    _embed = embed;
    _members = std::move(members);
    _enemies = std::move(enemies);
    _poolWeight = std::move(poolWeight);
    _poolBias = std::move(poolBias);
    _pointers = std::move(pointers);
    _memberCodes.assign(std::size_t(_members.Slots) * embed, 0.0f);
    _enemyCodes.assign(std::size_t(_enemies.Slots) * embed, 0.0f);
    _pooled.assign(std::size_t(4) * embed, 0.0f);
    _setExtra.assign(_sets ? _layers.front().Out : 0, 0.0f);
    _query.assign(std::max(embed, seatEmbed), 0.0f);
    _setHidden.assign(std::max(embed, seatEmbed), 0.0f);
    _seatSets = hasSeatSets != 0;
    _seatEmbed = seatEmbed;
    _seatSetList = std::move(seatSets);
    _seatPoolWeight = std::move(seatPoolWeight);
    _seatPoolBias = std::move(seatPoolBias);
    _seatPointers = std::move(seatPointers);
    _seatCodes.clear();
    uint32 seatWidest = 0;
    for (SeatSet const& set : _seatSetList)
    {
        _seatCodes.emplace_back(std::size_t(set.Slots) * seatEmbed, 0.0f);
        seatWidest = std::max(seatWidest, set.Width);
    }
    _seatAttention = attending != 0;
    _attention = std::move(attention);
    uint32 tokens = 1;
    for (SeatSet const& set : _seatSetList)
        tokens += set.Slots;
    _tokens.assign(_seatAttention ? std::size_t(tokens) * seatEmbed : 0, 0.0f);
    _normed.assign(_tokens.size(), 0.0f);
    _merged.assign(_tokens.size(), 0.0f);
    _qkv.assign(_tokens.size() * 3, 0.0f);
    _mixed.assign(_seatAttention ? std::size_t(2) * seatEmbed : 0, 0.0f);
    _scores.assign(_seatAttention ? tokens : 0, 0.0f);
    _keys.assign(_seatAttention ? tokens : 0, 0);
    _seatPooled.assign(std::size_t(2) * seatEmbed * _seatSetList.size() + (_seatAttention ? seatEmbed : 0), 0.0f);
    _seatExtra.assign(_seatSets ? _layers.front().Out : 0, 0.0f);
    _seatRaw.assign(seatWidest, 0.0f);
    _slowHiddenGates.assign(std::size_t(3) * slowSize, 0.0f);
    _slowOut.assign(slowSize, 0.0f);
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
    _goalSlots = 1;
    _slotBias.clear();
    _drawn.clear();
    _noneBias.clear();
    _shifted.clear();
    _scratchA.clear();
    _scratchB.clear();
    _gates.clear();
    _hiddenGates.clear();
    _memoryWeightIn.clear();
    _memoryWeightHidden.clear();
    _memoryBiasIn.clear();
    _memoryBiasHidden.clear();
    _kindWeight.clear();
    _kindBias.clear();
    _targetWeight.clear();
    _targetBias.clear();
    _pair.clear();
    _accepts.clear();
    _kindEmbedding.clear();
    _targetEmbedding.clear();
    _kindScale.clear();
    _targetScale.clear();
    _targetScores.clear();
    _goalBlockAt = -1;
    _goalTargets = 1;
    _lookahead = false;
    _success = Factored();
    _duration = Factored();
    _foresightOutputs = 0;
    _foresightWeight.clear();
    _foresightBias.clear();
    _feedbackWeight.clear();
    _feedbackBias.clear();
    _predictions.clear();
    _raw.clear();
    _slowSize = 0;
    _slowWeightIn.clear();
    _slowWeightHidden.clear();
    _slowBiasIn.clear();
    _slowBiasHidden.clear();
    _slowGates.clear();
    _slowHiddenGates.clear();
    _slowOut.clear();
    _sets = false;
    _embed = 0;
    _members = SetEncoder();
    _enemies = SetEncoder();
    _poolWeight.clear();
    _poolBias.clear();
    _pointers.clear();
    _seatSets = false;
    _seatAttention = false;
    _attention = SeatAttention();
    _tokens.clear();
    _normed.clear();
    _qkv.clear();
    _merged.clear();
    _mixed.clear();
    _scores.clear();
    _keys.clear();
    _seatEmbed = 0;
    _seatSetList.clear();
    _seatPoolWeight.clear();
    _seatPoolBias.clear();
    _seatPointers.clear();
    _seatCodes.clear();
    _seatPooled.clear();
    _seatExtra.clear();
    _seatRaw.clear();
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
        shape += Acore::StringFormat(" ({} goal kinds x {} targets every {} decisions)", _goalCount, _goalTargets,
            _goalEvery);

    return shape;
}

namespace
{
    float Sigmoid(float value)
    {
        return 1.0f / (1.0f + std::exp(-value));
    }
}

int32 Animus::MlpPolicy::PrimaryOf(State const& state) const
{
    std::size_t const count = std::size_t(_goalCount) * _goalTargets;
    return _goalSlots > 1 ? int32(state.Goal / (count + 1)) : int32(state.Goal);
}

int32 Animus::MlpPolicy::SecondaryOf(State const& state) const
{
    std::size_t const count = std::size_t(_goalCount) * _goalTargets;
    return _goalSlots > 1 ? int32(state.Goal % (count + 1)) - 1 : -1;
}

int32 Animus::MlpPolicy::Decide(float const* obs, uint8 const* mask, State* state)
{
    float const* logits = Forward(obs, state);
    if (!logits)
        return 0;

    int32 best = 0;
    float bestLogit = -std::numeric_limits<float>::infinity();
    bool anyAllowed = false;

    for (uint32 action = 0; action < _numActions; ++action)
    {
        if (!mask[action])
            continue;

        if (!anyAllowed || logits[action] > bestLogit)
        {
            best = int32(action);
            bestLogit = logits[action];
            anyAllowed = true;
        }
    }

    return anyAllowed ? best : 0;
}

bool Animus::MlpPolicy::Logits(float const* obs, float* out, State* state)
{
    float const* logits = Forward(obs, state);
    if (!logits)
        return false;

    std::copy(logits, logits + _numActions, out);
    return true;
}

void Animus::MlpPolicy::Attend()
{
    // x += out_proj(MHA(LN(x))), x += W2 relu(W1 LN(x)) over the tokens, the absent ones no keys (the own token
    // always one, so no softmax row is empty). LayerNorm as torch's: eps 1e-5, the biased variance.
    uint32 const embed = _seatEmbed;
    uint32 const count = uint32(_keys.size());
    uint32 const heads = _attention.Heads;
    uint32 const width = embed / heads;
    auto const norm = [embed](float const* in, float* out, std::vector<float> const& w, std::vector<float> const& b)
    {
        float mean = 0.0f;
        for (uint32 i = 0; i < embed; ++i)
            mean += in[i];
        mean /= float(embed);
        float variance = 0.0f;
        for (uint32 i = 0; i < embed; ++i)
            variance += (in[i] - mean) * (in[i] - mean);
        float const scale = 1.0f / std::sqrt(variance / float(embed) + 1e-5f);
        for (uint32 i = 0; i < embed; ++i)
            out[i] = (in[i] - mean) * scale * w[i] + b[i];
    };
    auto const affine = [](float const* in, uint32 inWidth, float* out, uint32 outWidth, std::vector<float> const& w,
        std::vector<float> const& b)
    {
        for (uint32 row = 0; row < outWidth; ++row)
        {
            float sum = b[row];
            float const* weights = w.data() + std::size_t(row) * inWidth;
            for (uint32 col = 0; col < inWidth; ++col)
                sum += weights[col] * in[col];
            out[row] = sum;
        }
    };

    for (uint32 t = 0; t < count; ++t)
    {
        norm(_tokens.data() + std::size_t(t) * embed, _normed.data() + std::size_t(t) * embed, _attention.NormAttendW,
            _attention.NormAttendB);
        affine(_normed.data() + std::size_t(t) * embed, embed, _qkv.data() + std::size_t(t) * 3 * embed, 3 * embed,
            _attention.InProjW, _attention.InProjB);
    }
    float const scale = 1.0f / std::sqrt(float(width));
    for (uint32 t = 0; t < count; ++t)
        for (uint32 head = 0; head < heads; ++head)
        {
            float const* q = _qkv.data() + std::size_t(t) * 3 * embed + head * width;
            float peak = -std::numeric_limits<float>::infinity();
            for (uint32 k = 0; k < count; ++k)
            {
                if (!_keys[k])
                    continue;
                float const* key = _qkv.data() + std::size_t(k) * 3 * embed + embed + head * width;
                float score = 0.0f;
                for (uint32 i = 0; i < width; ++i)
                    score += q[i] * key[i];
                _scores[k] = score * scale;
                peak = std::max(peak, _scores[k]);
            }
            float total = 0.0f;
            for (uint32 k = 0; k < count; ++k)
                if (_keys[k])
                {
                    _scores[k] = std::exp(_scores[k] - peak);
                    total += _scores[k];
                }
            float* merged = _merged.data() + std::size_t(t) * embed + head * width;
            std::fill(merged, merged + width, 0.0f);
            for (uint32 k = 0; k < count; ++k)
            {
                if (!_keys[k])
                    continue;
                float const* value = _qkv.data() + std::size_t(k) * 3 * embed + 2 * embed + head * width;
                float const weight = _scores[k] / total;
                for (uint32 i = 0; i < width; ++i)
                    merged[i] += weight * value[i];
            }
        }
    for (uint32 t = 0; t < count; ++t)
    {
        float* x = _tokens.data() + std::size_t(t) * embed;
        affine(_merged.data() + std::size_t(t) * embed, embed, _normed.data() + std::size_t(t) * embed, embed,
            _attention.OutProjW, _attention.OutProjB);
        for (uint32 i = 0; i < embed; ++i)
            x[i] += _normed[std::size_t(t) * embed + i];
        norm(x, _normed.data() + std::size_t(t) * embed, _attention.NormMixW, _attention.NormMixB);
        affine(_normed.data() + std::size_t(t) * embed, embed, _mixed.data(), 2 * embed, _attention.MixInW,
            _attention.MixInB);
        for (float& v : _mixed)
            v = std::max(v, 0.0f);
        for (uint32 row = 0; row < embed; ++row)
        {
            float sum = _attention.MixOutB[row];
            float const* weights = _attention.MixOutW.data() + std::size_t(row) * 2 * embed;
            for (uint32 col = 0; col < 2 * embed; ++col)
                sum += weights[col] * _mixed[col];
            x[row] += sum;
        }
    }
}

float const* Animus::MlpPolicy::Forward(float const* obs, State* state)
{
    if (_layers.empty())
        return nullptr;

    // Input: observation, then the one-hot id of agent 0.
    float* in = _scratchA.data();
    float* out = _scratchB.data();
    std::copy(obs, obs + _obsDim, in);
    std::fill(in + _obsDim, in + _obsDim + _numAgents, 0.0f);
    in[_obsDim] = 1.0f;

    // The director's sets: every slot through its set's encoder, the present ones pooled (mean and max) onto the
    // first layer.
    if (_sets)
    {
        uint32 const embed = _embed;
        auto const encode = [&](SetEncoder const& set, std::vector<float>& codes, float* meanOut, float* maxOut)
        {
            std::vector<float>& hidden = _setHidden;
            uint32 present = 0;
            std::fill(meanOut, meanOut + embed, 0.0f);
            std::fill(maxOut, maxOut + embed, -1.0f);
            for (uint32 slot = 0; slot < set.Slots; ++slot)
            {
                float const* raw = obs + set.First + slot * set.Width;
                float* code = codes.data() + std::size_t(slot) * embed;
                for (uint32 row = 0; row < embed; ++row)
                {
                    float sum = set.B1[row];
                    for (uint32 col = 0; col < set.Width; ++col)
                        sum += set.W1[std::size_t(row) * set.Width + col] * raw[col];
                    hidden[row] = std::tanh(sum);
                }
                for (uint32 row = 0; row < embed; ++row)
                {
                    float sum = set.B2[row];
                    for (uint32 col = 0; col < embed; ++col)
                        sum += set.W2[std::size_t(row) * embed + col] * hidden[col];
                    code[row] = std::tanh(sum);
                }
                if (raw[set.Present] > 0.5f)
                {
                    ++present;
                    for (uint32 row = 0; row < embed; ++row)
                    {
                        meanOut[row] += code[row];
                        maxOut[row] = std::max(maxOut[row], code[row]);
                    }
                }
            }
            for (uint32 row = 0; row < embed; ++row)
                meanOut[row] /= float(std::max<uint32>(1, present));
        };
        encode(_members, _memberCodes, _pooled.data(), _pooled.data() + embed);
        encode(_enemies, _enemyCodes, _pooled.data() + 2 * embed, _pooled.data() + 3 * embed);
        for (uint32 row = 0; row < _setExtra.size(); ++row)
        {
            float sum = _poolBias[row];
            for (uint32 col = 0; col < 4 * embed; ++col)
                sum += _poolWeight[std::size_t(row) * 4 * embed + col] * _pooled[col];
            _setExtra[row] = sum;
        }
    }

    // A seat layout's sets: every slot gathered from its segments through its set's encoder, the present ones pooled
    // (mean and max; nothing present pools to zero) onto the first layer, as the learner's EntitySets does.
    if (_seatSets)
    {
        uint32 const embed = _seatEmbed;
        // Every slot's code first (and, with attention, its token: the code plus its set's type embedding).
        uint32 token = 1;
        if (_seatAttention)
        {
            std::copy(_attention.OwnToken.begin(), _attention.OwnToken.end(), _tokens.begin());
            _keys[0] = 1;
        }
        for (std::size_t index = 0; index < _seatSetList.size(); ++index)
        {
            SeatSet const& set = _seatSetList[index];
            for (uint32 slot = 0; slot < set.Slots; ++slot)
            {
                uint32 at = 0;
                for (auto const& [first, stride] : set.Segments)
                    for (uint32 col = 0; col < stride; ++col)
                        _seatRaw[at++] = obs[first + slot * stride + col];
                float* code = _seatCodes[index].data() + std::size_t(slot) * embed;
                for (uint32 row = 0; row < embed; ++row)
                {
                    float sum = set.B1[row];
                    for (uint32 col = 0; col < set.Width; ++col)
                        sum += set.W1[std::size_t(row) * set.Width + col] * _seatRaw[col];
                    _setHidden[row] = std::tanh(sum);
                }
                for (uint32 row = 0; row < embed; ++row)
                {
                    float sum = set.B2[row];
                    for (uint32 col = 0; col < embed; ++col)
                        sum += set.W2[std::size_t(row) * embed + col] * _setHidden[col];
                    code[row] = std::tanh(sum);
                }
                if (_seatAttention)
                {
                    float* at = _tokens.data() + std::size_t(token) * embed;
                    for (uint32 row = 0; row < embed; ++row)
                        at[row] = code[row] + _attention.TypeEmbed[index * embed + row];
                    _keys[token++] = obs[set.Present + slot * set.PresentStride] > 0.5f ? 1 : 0;
                }
            }
        }
        if (_seatAttention)
        {
            Attend();
            // The attended slots are what the sets pool and the pointers read.
            token = 1;
            for (std::size_t index = 0; index < _seatSetList.size(); ++index)
                for (uint32 slot = 0; slot < _seatSetList[index].Slots; ++slot, ++token)
                    std::copy_n(_tokens.data() + std::size_t(token) * embed, embed,
                        _seatCodes[index].data() + std::size_t(slot) * embed);
        }
        for (std::size_t index = 0; index < _seatSetList.size(); ++index)
        {
            SeatSet const& set = _seatSetList[index];
            float* meanOut = _seatPooled.data() + 2 * embed * index;
            float* maxOut = meanOut + embed;
            std::fill(meanOut, meanOut + embed, 0.0f);
            std::fill(maxOut, maxOut + embed, -std::numeric_limits<float>::infinity());
            uint32 present = 0;
            for (uint32 slot = 0; slot < set.Slots; ++slot)
            {
                // The learner's max is over every slot, an absent one counting -1 (EntitySets.pooled): with
                // attention a present slot can read below -1, so the floor is only there when a slot is absent.
                float const* code = _seatCodes[index].data() + std::size_t(slot) * embed;
                bool const here = obs[set.Present + slot * set.PresentStride] > 0.5f;
                present += here ? 1 : 0;
                for (uint32 row = 0; row < embed; ++row)
                {
                    if (here)
                        meanOut[row] += code[row];
                    maxOut[row] = std::max(maxOut[row], here ? code[row] : -1.0f);
                }
            }
            for (uint32 row = 0; row < embed; ++row)
            {
                meanOut[row] /= float(std::max<uint32>(1, present));
                if (!present)
                    maxOut[row] = 0.0f;
            }
        }
        if (_seatAttention)
            std::copy_n(_tokens.data(), embed, _seatPooled.data() + 2 * embed * _seatSetList.size());
        std::size_t const pooledWidth = _seatPooled.size();
        for (uint32 row = 0; row < _seatExtra.size(); ++row)
        {
            float sum = _seatPoolBias[row];
            for (std::size_t col = 0; col < pooledWidth; ++col)
                sum += _seatPoolWeight[std::size_t(row) * pooledWidth + col] * _seatPooled[col];
            _seatExtra[row] = sum;
        }
    }

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
            if (_sets && index == 0)
                sum += _setExtra[row];
            if (_seatSets && index == 0)
                sum += _seatExtra[row];

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

    // The features before the predictions are fed back: what the goal head reads without a slow loop.
    std::copy(in, in + features, _raw.begin());
    if (_foresightOutputs)
    {
        for (uint32 row = 0; row < _foresightOutputs; ++row)
        {
            float const* weights = _foresightWeight.data() + std::size_t(row) * features;
            float sum = _foresightBias[row];
            for (uint32 col = 0; col < features; ++col)
                sum += weights[col] * _raw[col];
            _predictions[row] = sum;
        }
        for (uint32 row = 0; row < features; ++row)
        {
            float const* weights = _feedbackWeight.data() + std::size_t(row) * _foresightOutputs;
            float sum = _feedbackBias[row];
            for (uint32 col = 0; col < _foresightOutputs; ++col)
                sum += weights[col] * _predictions[col];
            in[row] += sum;
        }
    }

    if (_goalCount)
    {
        // The goal decision (the learner's LayoutActor.decide_goals, greedy). With two goals and a queue: an ended
        // secondary is dropped, an ended primary is replaced by the queue's head without a choice, a choice comes on
        // the clock, on an ended primary with nothing queued, or on the goal block's event, and the director's order
        // is the primary whatever was held or drawn. Each is the best (kind, target) its mask allows.
        uint32 const targets = _goalTargets;
        std::size_t const count = std::size_t(_goalCount) * targets;
        bool const paired = _goalSlots > 1;
        float const* block = _goalBlockAt >= 0 ? obs + _goalBlockAt : nullptr;
        uint32 const base = _goalCount + targets;
        bool const ended = block && targets > 1 && block[base] > 0.5f;
        bool const secondaryEnded = paired && block && targets > 1 && block[base + 2] > 0.5f;
        bool const event = paired && block && targets > 1 && block[base + 3] > 0.5f;
        bool const fromOrder = paired && block && targets > 1 && block[base + 4] > 0.5f;
        int32 orderGoal = 0;
        if (fromOrder)
        {
            uint32 kind = 0;
            uint32 target = 0;
            for (uint32 k = 1; k < _goalCount; ++k)
                if (block[base + 5 + k] > block[base + 5 + kind])
                    kind = k;
            for (uint32 t = 1; t < targets; ++t)
                if (block[base + 5 + _goalCount + t] > block[base + 5 + _goalCount + target])
                    target = t;
            orderGoal = int32(kind * targets + target);
        }

        uint32 const held = state ? state->Goal : 0;
        int32 primary = paired ? int32(held / (count + 1)) : int32(held);
        int32 secondary = paired ? int32(held % (count + 1)) - 1 : -1;
        std::array<int32, 2> queue = state ? state->Queue : std::array<int32, 2>{ -1, -1 };
        if (secondaryEnded)
            secondary = -1;
        bool promoted = false;
        if (paired && ended && queue[0] >= 0)
        {
            primary = queue[0];
            queue = { queue[1], -1 };
            promoted = true;
        }
        bool const choose = !state || state->Age % _goalEvery == 0 || (ended && !promoted) || event;
        if (choose)
        {
            // What the goal head reads: the slow loop stepped on the fed-back features, or the plain features.
            float const* source = _raw.data();
            uint32 width = features;
            if (_slowSize)
            {
                uint32 const size = _slowSize;
                std::vector<float>* slow = state ? &state->SlowMemory : nullptr;
                if (slow && slow->size() != size)
                    slow->assign(size, 0.0f);
                float const* carried = slow ? slow->data() : nullptr;
                for (uint32 row = 0; row < 3 * size; ++row)
                {
                    float const* input = _slowWeightIn.data() + std::size_t(row) * features;
                    float sum = _slowBiasIn[row];
                    for (uint32 col = 0; col < features; ++col)
                        sum += input[col] * in[col];
                    _slowGates[row] = sum;
                    float const* hidden = _slowWeightHidden.data() + std::size_t(row) * size;
                    float recurrent = _slowBiasHidden[row];
                    if (carried)
                        for (uint32 col = 0; col < size; ++col)
                            recurrent += hidden[col] * carried[col];
                    _slowHiddenGates[row] = recurrent;
                }
                for (uint32 row = 0; row < size; ++row)
                {
                    float const reset = Sigmoid(_slowGates[row] + _slowHiddenGates[row]);
                    float const update = Sigmoid(_slowGates[size + row] + _slowHiddenGates[size + row]);
                    float const candidate = std::tanh(_slowGates[2 * size + row]
                        + reset * _slowHiddenGates[2 * size + row]);
                    _slowOut[row] = (1.0f - update) * candidate + update * (carried ? carried[row] : 0.0f);
                }
                if (slow)
                    std::copy(_slowOut.begin(), _slowOut.end(), slow->begin());
                source = _slowOut.data();
                width = size;
            }

            auto const dot = [&](float const* reads, float const* weights, float bias)
            {
                float sum = bias;
                for (uint32 col = 0; col < width; ++col)
                    sum += weights[col] * reads[col];
                return sum;
            };
            auto const factored = [&](Factored const& part, uint32 kind, uint32 target)
            {
                float score = dot(source, part.KindWeight.data() + std::size_t(kind) * width, part.KindBias[kind])
                    + part.Pair[std::size_t(kind) * targets + target];
                if (targets > 1)
                    score += dot(source, part.TargetWeight.data() + std::size_t(target) * width,
                        part.TargetBias[target]);
                return score;
            };
            // The best goal from `reads`: the primary (lookahead, block and accepts; the first always allowed), or a
            // later slot (its own mask; -1 when none scores best).
            auto const best = [&](float const* reads, uint32 slot) -> int32
            {
                for (uint32 target = 0; target < targets; ++target)
                    _targetScores[target] = targets > 1
                        ? dot(reads, _targetWeight.data() + std::size_t(target) * width, _targetBias[target]) : 0.0f;
                float top = slot ? _noneBias[slot - 1] : -std::numeric_limits<float>::infinity();
                int32 chosen = slot ? -1 : 0;
                for (uint32 kind = 0; kind < _goalCount; ++kind)
                {
                    // The kinds the block offers, for every slot (a queued goal too, as the learner draws it), and
                    // with no goal block (the director) only the first goal for the primary and none after it.
                    if (block && targets > 1 && block[kind] <= 0.5f)
                        continue;
                    if (!block && targets > 1 && (slot || kind > 0))
                        continue;
                    float const kindScore = dot(reads, _kindWeight.data() + std::size_t(kind) * width,
                        _kindBias[kind]);
                    for (uint32 target = 0; target < targets; ++target)
                    {
                        std::size_t const joint = std::size_t(kind) * targets + target;
                        bool const present = !block || targets <= 1 || block[_goalCount + target] > 0.5f;
                        bool const allowed = (!slot && joint == 0)
                            || ((block != nullptr || targets <= 1) && _accepts[joint] && present);
                        if (!allowed)
                            continue;
                        float score = kindScore + _targetScores[target] + _pair[joint];
                        if (_lookahead && !slot)
                            score += _lookaheadWeight[0] * factored(_success, kind, target)
                                + _lookaheadWeight[1] * Sigmoid(factored(_duration, kind, target));
                        if (score > top)
                        {
                            top = score;
                            chosen = int32(joint);
                        }
                    }
                }
                return chosen;
            };

            int32 const drawnPrimary = best(source, 0);
            primary = drawnPrimary;
            if (paired)
            {
                // Each later slot reads the features plus its bias plus what was drawn before it (the director's
                // primary in place of the drawn one where it gave one).
                std::array<int32, 8> before{};
                uint32 drawnCount = 0;
                before[drawnCount++] = fromOrder ? orderGoal : drawnPrimary;
                std::array<int32, 8> slots{};
                for (uint32 slot = 1; slot < _goalSlots && slot < slots.size(); ++slot)
                {
                    float const* bias = _slotBias.data() + std::size_t(slot - 1) * width;
                    for (uint32 col = 0; col < width; ++col)
                        _shifted[col] = source[col] + bias[col];
                    for (uint32 index = 0; index < drawnCount; ++index)
                    {
                        float const* row = _drawn.data() + std::size_t(before[index] + 1) * width;
                        for (uint32 col = 0; col < width; ++col)
                            _shifted[col] += row[col];
                    }
                    slots[slot] = best(_shifted.data(), slot);
                    before[drawnCount++] = slots[slot];
                }
                secondary = slots[1];
                queue = { _goalSlots > 2 ? slots[2] : -1, _goalSlots > 3 ? slots[3] : -1 };
            }
        }
        if (paired)
        {
            if (fromOrder)
                primary = orderGoal;
            if (secondary == primary)
                secondary = -1;
        }

        if (state)
        {
            state->Age = choose ? 1 : state->Age + 1;
            state->Goal = paired ? uint32(std::size_t(primary) * (count + 1) + std::size_t(secondary + 1))
                : uint32(primary);
            state->Queue = queue;
        }

        // The goals on the features the action head reads: features x (1 + scale) + embedding, the primary's and
        // the secondary's through the gate (GoalEmbedding.condition).
        auto const value = [&](std::vector<float> const& kind, std::vector<float> const& target, int32 goal,
            uint32 col)
        {
            float v = kind[std::size_t(goal / int32(targets)) * features + col];
            if (targets > 1)
                v += target[std::size_t(goal % int32(targets)) * features + col];
            return v;
        };
        bool const second = paired && secondary >= 0;
        for (uint32 col = 0; col < features; ++col)
        {
            float scale = value(_kindScale, _targetScale, primary, col);
            float shift = value(_kindEmbedding, _targetEmbedding, primary, col);
            if (second)
            {
                scale += _gate * value(_kindScale, _targetScale, secondary, col);
                shift += _gate * value(_kindEmbedding, _targetEmbedding, secondary, col);
            }
            in[col] = in[col] * (1.0f + scale) + shift;
        }
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

        // The director's per-slot actions: each slot's encoding against a query from the features.
        for (Pointer const& pointer : _pointers)
        {
            for (uint32 row = 0; row < _embed; ++row)
            {
                float sum = pointer.Bias[row];
                for (uint32 col = 0; col < head.In; ++col)
                    sum += pointer.Weight[std::size_t(row) * head.In + col] * in[col];
                _query[row] = sum;
            }
            SetEncoder const& set = pointer.Over ? _enemies : _members;
            std::vector<float> const& codes = pointer.Over ? _enemyCodes : _memberCodes;
            for (uint32 slot = 0; slot < set.Slots; ++slot)
            {
                float score = 0.0f;
                for (uint32 row = 0; row < _embed; ++row)
                    score += codes[std::size_t(slot) * _embed + row] * _query[row];
                out[pointer.First + slot] = score;
            }
        }

        // A seat layout's slot-naming actions, the same way, after the director's.
        for (SeatPointer const& pointer : _seatPointers)
        {
            for (uint32 row = 0; row < _seatEmbed; ++row)
            {
                float sum = pointer.Bias[row];
                for (uint32 col = 0; col < head.In; ++col)
                    sum += pointer.Weight[std::size_t(row) * head.In + col] * in[col];
                _query[row] = sum;
            }
            std::vector<float> const& codes = _seatCodes[pointer.Set];
            for (uint32 slot = 0; slot < _seatSetList[pointer.Set].Slots; ++slot)
            {
                float score = 0.0f;
                for (uint32 row = 0; row < _seatEmbed; ++row)
                    score += codes[std::size_t(slot) * _seatEmbed + row] * _query[row];
                out[pointer.First + slot] = score;
            }
        }

        std::swap(in, out);
    }

    // `in` now holds the logits.
    return in;
}

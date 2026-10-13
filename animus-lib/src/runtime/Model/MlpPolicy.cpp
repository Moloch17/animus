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
    /// 10: the camera (the vision encoder, its map and entity list), the free look, and the goal head's place and cell
    /// pointers, as a tail after the seat sets, with the learner's current goal decision (see DecideGoals).
    constexpr uint32 AMDL_VERSION = 10;
    /// The oldest format still read: a version 7 model is a version 8 one without the seat sets, an 8 a 9 without the
    /// attention flag, a 9 a 10 without the tail (and the older goal rules).
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
        // Format 10's block (revision 2 on) has no order columns: the secondary ending, the event, what was achieved.
        uint32 const blockWidth = goalCount + goalTargets + 2 + (goalSlots <= 1 ? 0
            : version >= 10 ? 2 + goalCount + goalTargets : 3 + 2 * (goalCount + goalTargets));
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

    // Format 10's tail: the goal head's pointers, the camera and its free look.
    bool goalSpace = false;
    PlacePointer place;
    CellHead cells;
    Camera10 camera;
    if (version >= 10)
    {
        uint32 const goalWidth = slowSize ? slowSize : featureWidth;
        uint32 const adapterOut = layers.front().Out;
        auto const flag = [&reader](bool& out)
        {
            uint8 value = 0;
            bool const ok = reader.Read(value);
            out = value != 0;
            return ok;
        };
        bool ok = flag(goalSpace) && flag(place.On);
        if (ok && place.On)
            ok = reader.Read(place.At) && reader.Read(place.First) && reader.Read(place.Count)
                && reader.Read(place.Features) && reader.Read(place.Hidden) && place.Hidden <= MAX_LAYER_WIDTH
                && place.Features <= 64 && goalCount && goalBlockAt >= 0
                && place.First + place.Count <= goalTargets
                && uint32(goalBlockAt) + place.At + place.Count * place.Features <= obsDim
                && reader.ReadFloats(place.W1, std::size_t(place.Hidden) * place.Features)
                && reader.ReadFloats(place.B1, place.Hidden) && reader.ReadFloats(place.W2, place.Hidden)
                && reader.Read(place.B2);
        ok = ok && flag(cells.On);
        if (ok && cells.On)
        {
            ok = reader.Read(cells.CropOffset) && reader.Read(cells.Grid) && reader.Read(cells.Pool)
                && reader.Read(cells.Joint) && reader.Read(cells.Rise) && reader.Read(cells.MinFloor)
                && reader.Read(cells.Features) && reader.Read(cells.Hidden) && reader.Read(cells.ChainHidden)
                && cells.Features == 22 && cells.Grid > 0 && cells.Grid <= 64 && cells.Pool > 0 && cells.Pool <= 8
                && cells.Hidden <= MAX_LAYER_WIDTH && cells.ChainHidden <= MAX_LAYER_WIDTH && goalSlots > 1
                && cells.Joint < goalCount * goalTargets;
            ok = ok && reader.ReadFloats(cells.LocalW, std::size_t(cells.Hidden) * cells.Features)
                && reader.ReadFloats(cells.LocalB, cells.Hidden)
                && reader.ReadFloats(cells.QueryW, std::size_t(cells.Hidden) * goalWidth)
                && reader.ReadFloats(cells.QueryB, cells.Hidden) && reader.ReadFloats(cells.ScoreW, cells.Hidden)
                && reader.Read(cells.ScoreB) && reader.ReadFloats(cells.ChainW1, std::size_t(cells.ChainHidden) * 3)
                && reader.ReadFloats(cells.ChainB1, cells.ChainHidden)
                && reader.ReadFloats(cells.ChainW2, cells.ChainHidden) && reader.Read(cells.ChainB2);
        }
        ok = ok && flag(camera.On);
        if (ok && camera.On)
        {
            Camera10& c = camera;
            uint32 classCount = 0;
            ok = reader.Read(c.Height) && reader.Read(c.Width) && reader.Read(c.Patch) && reader.Read(c.Scalars)
                && reader.Read(c.First) && reader.Read(c.CropBytes) && reader.Read(classCount) && c.Patch > 0
                && c.Height > 0 && c.Width > 0 && c.Height % c.Patch == 0 && c.Width % c.Patch == 0
                && c.Height * c.Width <= (1u << 20) && c.First + c.Scalars <= obsDim && classCount <= 32;
            c.ClassPlane.fill(-1);
            for (uint32 i = 0; ok && i < classCount; ++i)
            {
                uint32 klass = 0;
                ok = reader.Read(klass) && klass < 32;
                if (ok)
                    c.ClassPlane[klass] = int8(i);
            }
            c.Planes = 4 + classCount;
            ok = ok && reader.Read(c.ClassLimit) && reader.Read(c.ClassWidth) && c.ClassLimit >= 32
                && c.ClassWidth <= 64 && reader.ReadFloats(c.ClassEmbed, std::size_t(c.ClassLimit) * c.ClassWidth)
                && reader.Read(c.PatchOut) && reader.Read(c.MixOut) && reader.Read(c.Embed)
                && c.PatchOut <= MAX_LAYER_WIDTH && c.MixOut <= MAX_LAYER_WIDTH && c.Embed <= MAX_LAYER_WIDTH;
            std::vector<float> patchW;
            std::size_t const patchIn = std::size_t(c.Patch) * c.Patch * c.Planes;
            ok = ok && reader.ReadFloats(patchW, c.PatchOut * patchIn) && reader.ReadFloats(c.PatchB, c.PatchOut)
                && reader.ReadFloats(c.MixW, std::size_t(c.MixOut) * c.PatchOut) && reader.ReadFloats(c.MixB, c.MixOut)
                && reader.ReadFloats(c.EmbedW, std::size_t(c.Embed) * (2 * c.MixOut + c.Scalars))
                && reader.ReadFloats(c.EmbedB, c.Embed);
            if (ok)
            {
                c.PatchWT.assign(patchW.size(), 0.0f);
                for (std::size_t row = 0; row < c.PatchOut; ++row)
                    for (std::size_t col = 0; col < patchIn; ++col)
                        c.PatchWT[col * c.PatchOut + row] = patchW[row * patchIn + col];
            }
            ok = ok && flag(c.Entities);
            if (ok && c.Entities)
            {
                ok = reader.Read(c.EFirst) && reader.Read(c.ESlots) && reader.Read(c.EWidth) && reader.Read(c.EPresent)
                    && reader.Read(c.EClass) && reader.Read(c.EType) && reader.Read(c.EObject)
                    && reader.Read(c.EMemory) && reader.Read(c.Buckets) && reader.Read(c.TypeWidth)
                    && reader.Read(c.Token) && c.EWidth > 2 && c.EWidth <= 256 && c.ESlots <= 256
                    && c.EFirst + c.ESlots * c.EWidth <= obsDim && c.EPresent < c.EWidth && c.EClass < c.EWidth
                    && c.EType < c.EWidth && c.EClass != c.EType && c.EObject < int32(c.EWidth)
                    && c.EMemory < int32(c.EWidth) && c.Buckets > 0 && c.TypeWidth <= 64
                    && c.Token <= MAX_LAYER_WIDTH;
                std::size_t const inputs = c.EWidth - 2 + c.ClassWidth + c.TypeWidth;
                ok = ok && reader.ReadFloats(c.TypeEmbed, std::size_t(c.Buckets) * c.TypeWidth)
                    && reader.ReadFloats(c.EW1, c.Token * inputs) && reader.ReadFloats(c.EB1, c.Token)
                    && reader.ReadFloats(c.EW2, std::size_t(c.Token) * c.Token) && reader.ReadFloats(c.EB2, c.Token)
                    && reader.ReadFloats(c.EPoolW, std::size_t(c.Embed) * 2 * c.Token)
                    && reader.ReadFloats(c.EPoolB, c.Embed);
            }
            ok = ok && flag(c.Map);
            if (ok && c.Map)
            {
                ok = reader.Read(c.MFirst) && reader.Read(c.MScalars) && reader.Read(c.MHeight) && reader.Read(c.MWidth)
                    && reader.Read(c.MPatch) && reader.Read(c.Codes) && reader.Read(c.CodeWidth)
                    && reader.Read(c.MPatchOut) && reader.Read(c.MMixOut) && reader.Read(c.MEmbed) && c.MPatch > 0
                    && c.MHeight % c.MPatch == 0 && c.MWidth % c.MPatch == 0 && c.Codes > 0 && c.Codes <= 16
                    && c.CodeWidth <= 64 && c.MFirst + c.MScalars <= obsDim
                    && c.MHeight * c.MWidth * 7 <= c.CropBytes && c.MPatchOut <= MAX_LAYER_WIDTH
                    && c.MMixOut <= MAX_LAYER_WIDTH && c.MEmbed <= MAX_LAYER_WIDTH;
                std::vector<float> codeEmbed, mapPatchW;
                uint32 const cellsInPatch = c.MPatch * c.MPatch;
                uint32 const planes = c.CodeWidth + c.ClassWidth + 6;
                std::size_t const patchInputs = std::size_t(cellsInPatch) * planes;
                ok = ok && reader.ReadFloats(codeEmbed, std::size_t(c.Codes) * c.CodeWidth)
                    && reader.ReadFloats(mapPatchW, c.MPatchOut * patchInputs)
                    && reader.ReadFloats(c.MPatchB, c.MPatchOut)
                    && reader.ReadFloats(c.MMixW, std::size_t(c.MMixOut) * c.MPatchOut)
                    && reader.ReadFloats(c.MMixB, c.MMixOut)
                    && reader.ReadFloats(c.MEmbedW, std::size_t(c.MEmbed) * (3 * c.MMixOut + c.MScalars))
                    && reader.ReadFloats(c.MEmbedB, c.MEmbed)
                    && reader.ReadFloats(c.MJoinW, std::size_t(c.Embed) * c.MEmbed)
                    && reader.ReadFloats(c.MJoinB, c.Embed);
                if (ok)
                {
                    // The code and the class are embeddings, so each (cell, code) and (cell, class) is one column
                    // of the patch layer's product: tabled here, the per-cell work is two lookups and six values.
                    uint32 const out = c.MPatchOut;
                    c.CodeTable.assign(std::size_t(cellsInPatch) * c.Codes * out, 0.0f);
                    c.ClassTable.assign(std::size_t(cellsInPatch) * c.ClassLimit * out, 0.0f);
                    c.ValueWT.assign(std::size_t(cellsInPatch) * 6 * out, 0.0f);
                    for (uint32 q = 0; q < cellsInPatch; ++q)
                        for (uint32 row = 0; row < out; ++row)
                        {
                            float const* w = mapPatchW.data() + std::size_t(row) * patchInputs
                                + std::size_t(q) * planes;
                            for (uint32 code = 0; code < c.Codes; ++code)
                            {
                                float sum = 0.0f;
                                for (uint32 j = 0; j < c.CodeWidth; ++j)
                                    sum += w[j] * codeEmbed[std::size_t(code) * c.CodeWidth + j];
                                c.CodeTable[(std::size_t(q) * c.Codes + code) * out + row] = sum;
                            }
                            for (uint32 klass = 0; klass < c.ClassLimit; ++klass)
                            {
                                float sum = 0.0f;
                                for (uint32 j = 0; j < c.ClassWidth; ++j)
                                    sum += w[c.CodeWidth + j] * c.ClassEmbed[std::size_t(klass) * c.ClassWidth + j];
                                c.ClassTable[(std::size_t(q) * c.ClassLimit + klass) * out + row] = sum;
                            }
                            for (uint32 v = 0; v < 6; ++v)
                                c.ValueWT[(std::size_t(q) * 6 + v) * out + row] = w[c.CodeWidth + c.ClassWidth + v];
                        }
                }
            }
            ok = ok && reader.ReadFloats(c.JoinW, std::size_t(adapterOut) * c.Embed)
                && reader.ReadFloats(c.JoinB, adapterOut);
            uint32 heads = 0;
            ok = ok && reader.Read(heads) && heads <= MAX_LOOK_HEADS;
            uint32 total = 0;
            for (uint32 i = 0; ok && i < heads; ++i)
            {
                uint32 size = 0;
                ok = reader.Read(size) && size > 0 && size <= 64;
                c.LookHeads.push_back(size);
                total += size;
            }
            ok = ok && reader.ReadFloats(c.LookW, std::size_t(total) * featureWidth)
                && reader.ReadFloats(c.LookB, total);
        }
        if (ok && cells.On && (!camera.On || !camera.Map
            || cells.CropOffset + cells.Grid * cells.Pool * cells.Grid * cells.Pool * 7 > camera.CropBytes))
            ok = false;
        if (ok && goalCount && goalSlots > MAX_GOAL_SLOTS)
            ok = false;
        if (!ok)
        {
            error = Acore::StringFormat("{} is truncated or inconsistent in its camera or goal pointers", path);
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

    _version = version;
    _goalSpace = goalSpace;
    _place = std::move(place);
    _placeScores.assign(goalTargets, 0.0f);
    _cells = std::move(cells);
    uint32 const blocks = _cells.On ? _cells.Grid * _cells.Grid : 0;
    _cellFeatures.assign(std::size_t(blocks) * _cells.Features, 0.0f);
    _cellHidden.assign(std::size_t(blocks) * _cells.Hidden, 0.0f);
    _cellExtra.assign(std::size_t(blocks) * 7, 0.0f);
    _cellQuery.assign(_cells.Hidden, 0.0f);
    _cellChain.assign(_cells.ChainHidden, 0.0f);
    _cellValid.assign(blocks, 0);
    _cellAllowed.assign(blocks, 0);
    _camera = std::move(camera);
    if (_camera.On)
    {
        Camera10& c = _camera;
        // The keypoints' grids, as torch.linspace(-1, 1, n) makes them (from both ends towards the middle).
        auto const linspace = [](uint32 steps, uint32 i)
        {
            if (steps == 1)
                return -1.0f;
            float const step = 2.0f / float(steps - 1);
            return i < steps / 2 ? -1.0f + step * float(i) : 1.0f - step * float(steps - 1 - i);
        };
        auto const grids = [&](uint32 rows, uint32 cols, std::vector<float>& x, std::vector<float>& y)
        {
            x.assign(std::size_t(rows) * cols, 0.0f);
            y.assign(std::size_t(rows) * cols, 0.0f);
            for (uint32 r = 0; r < rows; ++r)
                for (uint32 col = 0; col < cols; ++col)
                {
                    x[std::size_t(r) * cols + col] = linspace(cols, col);
                    y[std::size_t(r) * cols + col] = linspace(rows, r);
                }
        };
        grids(c.Height / c.Patch, c.Width / c.Patch, c.GridX, c.GridY);
        if (c.Map)
            grids(c.MHeight / c.MPatch, c.MWidth / c.MPatch, c.MGridX, c.MGridY);
        uint32 const patches = (c.Height / c.Patch) * (c.Width / c.Patch);
        uint32 const mapPatches = c.Map ? (c.MHeight / c.MPatch) * (c.MWidth / c.MPatch) : 0;
        c.Acc.assign(std::max(c.PatchOut, c.MPatchOut), 0.0f);
        c.Hidden.assign(std::max({ c.PatchOut, c.MPatchOut, c.Token }), 0.0f);
        c.Features.assign(std::size_t(std::max(patches * c.MixOut, mapPatches * c.MMixOut)), 0.0f);
        c.Pooled.assign(std::max(2 * c.MixOut + c.Scalars, 3 * c.MMixOut + c.MScalars), 0.0f);
        c.Embedded.assign(c.Embed, 0.0f);
        c.MapOut.assign(std::max(c.MEmbed, 2 * c.Token), 0.0f);
        c.Term.assign(_layers.front().Out, 0.0f);
        c.Raw.assign(c.EWidth, 0.0f);
        c.Input.assign(c.Entities ? c.EWidth - 2 + c.ClassWidth + c.TypeWidth : 0, 0.0f);
        c.Code.assign(c.Token, 0.0f);
        c.Mean.assign(c.Token, 0.0f);
        c.Max.assign(c.Token, 0.0f);
    }
    _lookLogits.assign(_camera.LookB.size(), 0.0f);
    _lastFeatures.assign(featureWidth, 0.0f);
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
    _version = 0;
    _goalSpace = false;
    _place = PlacePointer();
    _placeScores.clear();
    _cells = CellHead();
    _cellFeatures.clear();
    _cellHidden.clear();
    _cellExtra.clear();
    _cellQuery.clear();
    _cellChain.clear();
    _cellValid.clear();
    _cellAllowed.clear();
    _camera = Camera10();
    _lookLogits.clear();
    _lastFeatures.clear();
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
    return Decide(obs, mask, state, Camera(), nullptr);
}

int32 Animus::MlpPolicy::Decide(float const* obs, uint8 const* mask, State* state, Camera const& camera,
    Decision* decision)
{
    float const* logits = Forward(obs, state, &camera);
    if (decision)
        *decision = Decision();
    if (!logits)
        return 0;

    if (decision)
    {
        // The free look: each head's argmax (the first of equals, as torch's), and the goal choice's cells.
        uint32 offset = 0;
        for (uint32 head = 0; head < _camera.LookHeads.size(); ++head)
        {
            uint32 const size = _camera.LookHeads[head];
            int32 best = 0;
            for (uint32 i = 1; i < size; ++i)
                if (_lookLogits[offset + i] > _lookLogits[offset + best])
                    best = int32(i);
            decision->Look[head] = best;
            offset += size;
        }
        decision->GoalChosen = _lastChosen;
        decision->Cells = _lastCells;
    }

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
    float const* logits = Forward(obs, state, nullptr);
    if (!logits)
        return false;

    std::copy(logits, logits + _numActions, out);
    return true;
}

bool Animus::MlpPolicy::Logits(float const* obs, float* out, State* state, Camera const& camera, float* look)
{
    float const* logits = Forward(obs, state, &camera);
    if (!logits)
        return false;

    std::copy(logits, logits + _numActions, out);
    if (look)
        std::copy(_lookLogits.begin(), _lookLogits.end(), look);
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

float const* Animus::MlpPolicy::Forward(float const* obs, State* state, Camera const* camera)
{
    if (_layers.empty())
        return nullptr;

    // A camera model reads its image (and the map's crop, when it encodes it or points at its cells).
    _lastChosen = false;
    _lastCells = { -1, -1, -1, -1 };
    if (_camera.On)
    {
        if (!camera || !camera->Image || ((_camera.Map || _cells.On) && !camera->Crop))
            return nullptr;
        SeeCamera(obs, *camera);
    }

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
            if (_camera.On && index == 0)
                sum += _camera.Term[row];

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

    if (_goalCount && _version >= 10)
        DecideGoals(obs, camera, in, _raw.data(), features, state);
    else if (_goalCount)
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

    // The action head's input, and the free look on the same features (LookHead).
    std::copy(in, in + features, _lastFeatures.begin());
    for (uint32 row = 0; row < _lookLogits.size(); ++row)
    {
        float const* weights = _camera.LookW.data() + std::size_t(row) * features;
        float sum = _camera.LookB[row];
        for (uint32 col = 0; col < features; ++col)
            sum += weights[col] * in[col];
        _lookLogits[row] = sum;
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

namespace
{
    float Silu(float value)
    {
        return value / (1.0f + std::exp(-value));
    }

    /// out[row] = bias[row] + weight[row] . in, weight row-major [outWidth][inWidth].
    void Affine(float const* weight, float const* bias, float const* in, uint32 inWidth, uint32 outWidth, float* out)
    {
        for (uint32 row = 0; row < outWidth; ++row)
        {
            float const* w = weight + std::size_t(row) * inWidth;
            float sum = bias[row];
            for (uint32 col = 0; col < inWidth; ++col)
                sum += w[col] * in[col];
            out[row] = sum;
        }
    }

    /// The spatial softmax of the patch features [patches][channels]: per channel the expected (x, y) of its softmax
    /// over the patches, x for every channel then y for every channel; and, with `mean`, each channel's mean.
    void Keypoints(float const* features, uint32 patches, uint32 channels, float const* gridX, float const* gridY,
        float* out, float* mean)
    {
        for (uint32 c = 0; c < channels; ++c)
        {
            float peak = -std::numeric_limits<float>::infinity();
            for (uint32 p = 0; p < patches; ++p)
                peak = std::max(peak, features[std::size_t(p) * channels + c]);
            float total = 0.0f, x = 0.0f, y = 0.0f, sum = 0.0f;
            for (uint32 p = 0; p < patches; ++p)
            {
                float const value = features[std::size_t(p) * channels + c];
                float const weight = std::exp(value - peak);
                total += weight;
                x += weight * gridX[p];
                y += weight * gridY[p];
                sum += value;
            }
            out[c] = x / total;
            out[channels + c] = y / total;
            if (mean)
                mean[c] = sum / float(patches);
        }
    }

    /// The map crop's channels and codes (the sim's Vision::CropChannel, Vision::MapCode; the learner's MAP_*).
    constexpr uint32 MAP_CHANNELS = 7;
    constexpr uint32 MAP_CODE = 0, MAP_HEIGHT = 1, MAP_VISITED = 2, MAP_AGE = 3, MAP_CLASS = 4, MAP_FRONTIER = 5,
        MAP_SEARCHED = 6;
    constexpr uint8 MAP_FLOOR = 1, MAP_WALL = 2, MAP_DOOR = 3, MAP_HAZARD = 4;
    constexpr int32 MAP_HEIGHT_ZERO = 128;
    constexpr float MAP_SEARCHED_MAX = 4.0f;
    /// A cell word: ticket << CELL_BITS | (block + 1); tickets 1..TICKET_COUNT (the learner's _plan_words).
    constexpr uint32 CELL_BITS = 12;
    constexpr uint32 TICKET_COUNT = (1u << 11) - 1;
    constexpr float CELL_OFFSET_SCALE = 24.0f;
}

void Animus::MlpPolicy::SeeCamera(float const* obs, Camera const& camera)
{
    // The learner's VisionEncoder.forward, then its VisionJoin.
    Camera10& c = _camera;
    uint32 const rows = c.Height / c.Patch, cols = c.Width / c.Patch, patches = rows * cols;
    float* acc = c.Acc.data();
    float* hidden = c.Hidden.data();
    for (uint32 pr = 0; pr < rows; ++pr)
        for (uint32 pc = 0; pc < cols; ++pc)
        {
            // Every pixel's planes -- distance, height, normal, objective, then its class one-hot -- through the patch
            // layer, column by column: the one-hot is one column a pixel.
            std::copy(c.PatchB.begin(), c.PatchB.end(), acc);
            for (uint32 i = 0; i < c.Patch; ++i)
                for (uint32 j = 0; j < c.Patch; ++j)
                {
                    uint8 const* pixel = camera.Image
                        + (std::size_t(pr * c.Patch + i) * c.Width + pc * c.Patch + j) * 4;
                    float const values[4] = {
                        pixel[0] == 255 ? 1.0f : float(pixel[0]) / 254.0f,
                        (float(pixel[1]) - 128.0f) / 125.0f,
                        float(pixel[2]) / 255.0f,
                        float((pixel[3] >> 5) & 1),
                    };
                    std::size_t const base = std::size_t(i * c.Patch + j) * c.Planes;
                    for (uint32 v = 0; v < 4; ++v)
                    {
                        float const* w = c.PatchWT.data() + (base + v) * c.PatchOut;
                        float const value = values[v];
                        for (uint32 k = 0; k < c.PatchOut; ++k)
                            acc[k] += w[k] * value;
                    }
                    int32 const plane = c.ClassPlane[pixel[3] & 0x1F];
                    if (plane >= 0)
                    {
                        float const* w = c.PatchWT.data() + (base + 4 + plane) * c.PatchOut;
                        for (uint32 k = 0; k < c.PatchOut; ++k)
                            acc[k] += w[k];
                    }
                }
            for (uint32 k = 0; k < c.PatchOut; ++k)
                hidden[k] = Silu(acc[k]);
            float* features = c.Features.data() + std::size_t(pr * cols + pc) * c.MixOut;
            Affine(c.MixW.data(), c.MixB.data(), hidden, c.PatchOut, c.MixOut, features);
            for (uint32 k = 0; k < c.MixOut; ++k)
                features[k] = Silu(features[k]);
        }
    Keypoints(c.Features.data(), patches, c.MixOut, c.GridX.data(), c.GridY.data(), c.Pooled.data(), nullptr);
    std::copy(obs + c.First, obs + c.First + c.Scalars, c.Pooled.data() + 2 * c.MixOut);
    Affine(c.EmbedW.data(), c.EmbedB.data(), c.Pooled.data(), 2 * c.MixOut + c.Scalars, c.Embed, c.Embedded.data());

    // The mental map (MapEncoder): its crop's cells -- the code's and the class's embeddings and six values -- in
    // patches, keypoints and the patches' mean, its scalars, then its own join onto the camera's embedding.
    if (c.Map)
    {
        uint32 const mrows = c.MHeight / c.MPatch, mcols = c.MWidth / c.MPatch, mpatches = mrows * mcols;
        uint32 const out = c.MPatchOut;
        for (uint32 pr = 0; pr < mrows; ++pr)
            for (uint32 pc = 0; pc < mcols; ++pc)
            {
                std::copy(c.MPatchB.begin(), c.MPatchB.end(), acc);
                for (uint32 i = 0; i < c.MPatch; ++i)
                    for (uint32 j = 0; j < c.MPatch; ++j)
                    {
                        uint8 const* cell = camera.Crop
                            + (std::size_t(pr * c.MPatch + i) * c.MWidth + pc * c.MPatch + j) * MAP_CHANNELS;
                        uint32 const q = i * c.MPatch + j;
                        uint32 const code = std::min<uint32>(cell[MAP_CODE], c.Codes - 1);
                        uint32 const klass = std::min<uint32>(cell[MAP_CLASS] & 0x1F, c.ClassLimit - 1);
                        float const* codeRow = c.CodeTable.data() + (std::size_t(q) * c.Codes + code) * out;
                        float const* classRow = c.ClassTable.data() + (std::size_t(q) * c.ClassLimit + klass) * out;
                        bool const known = cell[MAP_HEIGHT] != 0;
                        float const values[6] = {
                            known ? (float(cell[MAP_HEIGHT]) - float(MAP_HEIGHT_ZERO)) / 127.0f : 0.0f,
                            known ? 1.0f : 0.0f,
                            cell[MAP_VISITED] != 0 ? 1.0f : 0.0f,
                            float(cell[MAP_AGE]) / 255.0f,
                            cell[MAP_FRONTIER] != 0 ? 1.0f : 0.0f,
                            float(cell[MAP_SEARCHED]) / MAP_SEARCHED_MAX,
                        };
                        for (uint32 k = 0; k < out; ++k)
                            acc[k] += codeRow[k] + classRow[k];
                        for (uint32 v = 0; v < 6; ++v)
                        {
                            float const* w = c.ValueWT.data() + (std::size_t(q) * 6 + v) * out;
                            float const value = values[v];
                            for (uint32 k = 0; k < out; ++k)
                                acc[k] += w[k] * value;
                        }
                    }
                for (uint32 k = 0; k < out; ++k)
                    hidden[k] = Silu(acc[k]);
                float* features = c.Features.data() + std::size_t(pr * mcols + pc) * c.MMixOut;
                Affine(c.MMixW.data(), c.MMixB.data(), hidden, out, c.MMixOut, features);
                for (uint32 k = 0; k < c.MMixOut; ++k)
                    features[k] = Silu(features[k]);
            }
        Keypoints(c.Features.data(), mpatches, c.MMixOut, c.MGridX.data(), c.MGridY.data(), c.Pooled.data(),
            c.Pooled.data() + 2 * c.MMixOut);
        std::copy(obs + c.MFirst, obs + c.MFirst + c.MScalars, c.Pooled.data() + 3 * c.MMixOut);
        Affine(c.MEmbedW.data(), c.MEmbedB.data(), c.Pooled.data(), 3 * c.MMixOut + c.MScalars, c.MEmbed,
            c.MapOut.data());
        for (uint32 k = 0; k < c.MEmbed; ++k)
            c.MapOut[k] = Silu(c.MapOut[k]);
        for (uint32 row = 0; row < c.Embed; ++row)
        {
            float const* w = c.MJoinW.data() + std::size_t(row) * c.MEmbed;
            float sum = c.MJoinB[row];
            for (uint32 col = 0; col < c.MEmbed; ++col)
                sum += w[col] * c.MapOut[col];
            c.Embedded[row] += sum;
        }
    }

    // The entity list (VisibleEntities): every slot's token -- its columns but the class and the type (the memory id
    // read as 0), the class's and the type bucket's embeddings -- the present ones pooled (mean, max).
    if (c.Entities)
    {
        uint32 const token = c.Token;
        std::fill(c.Mean.begin(), c.Mean.end(), 0.0f);
        std::fill(c.Max.begin(), c.Max.end(), -1.0f);
        uint32 present = 0;
        uint32 const inputs = uint32(c.Input.size());
        for (uint32 slot = 0; slot < c.ESlots; ++slot)
        {
            float const* raw = obs + c.EFirst + std::size_t(slot) * c.EWidth;
            auto const read = [&](int32 col) { return int32(col) == c.EMemory ? 0.0f : raw[col]; };
            // Only the present slots are pooled, and nothing else reads a token.
            if (read(int32(c.EPresent)) <= 0.5f)
                continue;
            uint32 at = 0;
            for (uint32 col = 0; col < c.EWidth; ++col)
                if (col != c.EClass && col != c.EType)
                    c.Input[at++] = int32(col) == c.EMemory ? 0.0f : raw[col];
            float const klassValue = std::nearbyint(read(int32(c.EClass)));
            uint32 const klass = uint32(std::clamp(klassValue, 0.0f, float(c.ClassLimit - 1)));
            int64 const entry = int64(std::max(std::nearbyint(read(int32(c.EType))), 0.0f));
            int64 const object = c.EObject >= 0 ? int64(std::nearbyint(read(c.EObject))) : 0;
            int64 const buckets = int64(c.Buckets);
            int64 const bucket = ((entry * 2 + object) % buckets + buckets) % buckets;
            std::copy_n(c.ClassEmbed.data() + std::size_t(klass) * c.ClassWidth, c.ClassWidth, c.Input.data() + at);
            std::copy_n(c.TypeEmbed.data() + std::size_t(bucket) * c.TypeWidth, c.TypeWidth,
                c.Input.data() + at + c.ClassWidth);
            Affine(c.EW1.data(), c.EB1.data(), c.Input.data(), inputs, token, hidden);
            for (uint32 k = 0; k < token; ++k)
                hidden[k] = std::tanh(hidden[k]);
            Affine(c.EW2.data(), c.EB2.data(), hidden, token, token, c.Code.data());
            ++present;
            for (uint32 k = 0; k < token; ++k)
            {
                float const code = std::tanh(c.Code[k]);
                c.Mean[k] += code;
                c.Max[k] = std::max(c.Max[k], code);
            }
        }
        for (uint32 k = 0; k < token; ++k)
        {
            c.MapOut[k] = c.Mean[k] / float(std::max<uint32>(1, present));
            c.MapOut[token + k] = present ? c.Max[k] : 0.0f;
        }
        for (uint32 row = 0; row < c.Embed; ++row)
        {
            float const* w = c.EPoolW.data() + std::size_t(row) * 2 * token;
            float sum = c.EPoolB[row];
            for (uint32 col = 0; col < 2 * token; ++col)
                sum += w[col] * c.MapOut[col];
            c.Embedded[row] += sum;
        }
    }

    for (float& value : c.Embedded)
        value = Silu(value);
    Affine(c.JoinW.data(), c.JoinB.data(), c.Embedded.data(), c.Embed, uint32(c.Term.size()), c.Term.data());
}

void Animus::MlpPolicy::CellInputs(uint8 const* crop)
{
    // The learner's cell_features and cell_valid: per pool x pool block of the crop, its 22 features, and whether a
    // cell goal may name it; then the blocks' hidden features, SiLU(local x + b).
    CellHead const& h = _cells;
    uint32 const grid = h.Grid, pool = h.Pool, side = grid * pool, blocks = grid * grid;
    float const area = float(pool * pool);
    enum Base : uint32 { FLOOR, DOOR, WALL, HAZARD, UNKNOWN, VISITED, FRONTIER, ENTITY, NEWEST, NEVER, HEIGHT, SEARCHED,
        BASES };
    std::vector<float>& base = _cellFeatures;      // [blocks][22]: the bases first, the rest after
    uint32 const F = h.Features;
    crop += h.CropOffset;
    for (uint32 br = 0; br < grid; ++br)
        for (uint32 bc = 0; bc < grid; ++bc)
        {
            uint32 counts[BASES] = {};
            uint32 barred = 0, off = 0, like = 0, seen = 0, entity = 0, searched = 0;
            float rise = 0.0f;
            int32 newest = 255;
            for (uint32 i = 0; i < pool; ++i)
                for (uint32 j = 0; j < pool; ++j)
                {
                    uint8 const* cell = crop + (std::size_t(br * pool + i) * side + bc * pool + j) * MAP_CHANNELS;
                    uint8 const code = cell[MAP_CODE];
                    int32 const height = cell[MAP_HEIGHT];
                    counts[FLOOR] += code == MAP_FLOOR;
                    counts[DOOR] += code == MAP_DOOR;
                    counts[WALL] += code == MAP_WALL;
                    counts[HAZARD] += code == MAP_HAZARD;
                    counts[UNKNOWN] += code == 0;
                    counts[VISITED] += cell[MAP_VISITED] != 0;
                    counts[FRONTIER] += cell[MAP_FRONTIER] != 0;
                    entity += (cell[MAP_CLASS] & 0x1F) != 0;
                    bool const isSeen = code != 0;
                    seen += isSeen;
                    newest = std::min<int32>(newest, isSeen ? cell[MAP_AGE] : 255);
                    bool const isLike = code == MAP_FLOOR || code == MAP_DOOR;
                    like += isLike;
                    barred += code == MAP_WALL || code == MAP_HAZARD;
                    off += isLike && (height == 0 || std::abs(height - MAP_HEIGHT_ZERO) > int32(h.Rise));
                    if (isLike)
                        rise += float(height - MAP_HEIGHT_ZERO) / float(h.Rise);
                    searched += cell[MAP_SEARCHED];
                }
            uint32 const block = br * grid + bc;
            float* f = base.data() + std::size_t(block) * F;
            for (uint32 k = FLOOR; k <= FRONTIER; ++k)
                f[k] = float(counts[k]) / area;
            f[ENTITY] = entity ? 1.0f : 0.0f;
            f[NEWEST] = float(newest) / 255.0f;
            f[NEVER] = seen ? 0.0f : 1.0f;
            f[HEIGHT] = std::clamp(rise / std::max(float(like), 1.0f), -1.0f, 1.0f);
            f[SEARCHED] = float(searched) / (MAP_SEARCHED_MAX * area);     // kept here until the layout below
            _cellValid[block] = barred == 0 && off == 0 && like >= h.MinFloor;
        }

    // The neighbourhoods: avg_pool2d with count_include_pad=False, 3 x 3 over floor, wall, unknown and frontier, 3 x 3
    // and 7 x 7 over the searched share, 7 x 7 over the frontier.
    auto const around = [&](uint32 br, uint32 bc, int32 radius, uint32 feature)
    {
        float sum = 0.0f;
        uint32 count = 0;
        for (int32 dr = -radius; dr <= radius; ++dr)
            for (int32 dc = -radius; dc <= radius; ++dc)
            {
                int32 const r = int32(br) + dr, col = int32(bc) + dc;
                if (r < 0 || col < 0 || r >= int32(grid) || col >= int32(grid))
                    continue;
                sum += base[(std::size_t(r) * grid + col) * F + feature];
                ++count;
            }
        return sum / float(count);
    };
    std::vector<float>& extra = _cellExtra;
    for (uint32 br = 0; br < grid; ++br)
        for (uint32 bc = 0; bc < grid; ++bc)
        {
            float* e = extra.data() + std::size_t(br * grid + bc) * 7;
            e[0] = around(br, bc, 1, FLOOR);
            e[1] = around(br, bc, 1, WALL);
            e[2] = around(br, bc, 1, UNKNOWN);
            e[3] = around(br, bc, 1, FRONTIER);
            e[4] = around(br, bc, 1, SEARCHED);
            e[5] = around(br, bc, 3, SEARCHED);
            e[6] = around(br, bc, 3, FRONTIER);
        }
    // The features in the learner's order: the eleven bases, the four 3 x 3 averages, forward, right and distance, the
    // searched share, its 3 x 3 and 7 x 7 averages, the frontier's 7 x 7 average.
    for (uint32 br = 0; br < grid; ++br)
        for (uint32 bc = 0; bc < grid; ++bc)
        {
            uint32 const block = br * grid + bc;
            float* f = base.data() + std::size_t(block) * F;
            float const* e = extra.data() + std::size_t(block) * 7;
            float const searchedShare = f[SEARCHED];
            float const forward = (float(side) - float(pool) * (2.0f * float(br) + 1.0f)) / 48.0f;
            float const right = (float(pool) * (2.0f * float(bc) + 1.0f) - float(side)) / 48.0f;
            f[11] = e[0];
            f[12] = e[1];
            f[13] = e[2];
            f[14] = e[3];
            f[15] = forward;
            f[16] = right;
            f[17] = std::sqrt(forward * forward + right * right);
            f[18] = searchedShare;
            f[19] = e[4];
            f[20] = e[5];
            f[21] = e[6];
        }
    for (uint32 block = 0; block < blocks; ++block)
    {
        float* hidden = _cellHidden.data() + std::size_t(block) * h.Hidden;
        Affine(h.LocalW.data(), h.LocalB.data(), base.data() + std::size_t(block) * F, F, h.Hidden, hidden);
        for (uint32 k = 0; k < h.Hidden; ++k)
            hidden[k] = Silu(hidden[k]);
    }
}

int32 Animus::MlpPolicy::BestCell(float const* shifted, float prevRow, float prevCol, std::vector<uint8> const& allowed)
{
    // The learner's cell_scores, its argmax over the allowed blocks (block 0 when none is, as masked_logits falls
    // back).
    CellHead const& h = _cells;
    uint32 const grid = h.Grid, blocks = grid * grid, width = _slowSize ? _slowSize : uint32(_raw.size());
    Affine(h.QueryW.data(), h.QueryB.data(), shifted, width, h.Hidden, _cellQuery.data());
    int32 best = -1;
    float top = 0.0f;
    for (uint32 block = 0; block < blocks; ++block)
    {
        if (!allowed[block])
            continue;
        float const* hidden = _cellHidden.data() + std::size_t(block) * h.Hidden;
        float score = h.ScoreB;
        for (uint32 k = 0; k < h.Hidden; ++k)
            score += h.ScoreW[k] * std::tanh(hidden[k] + _cellQuery[k]);
        float const dRow = (float(block / grid) - prevRow) / CELL_OFFSET_SCALE;
        float const dCol = (float(block % grid) - prevCol) / CELL_OFFSET_SCALE;
        float const offset[3] = { dRow, dCol, std::sqrt(dRow * dRow + dCol * dCol) };
        Affine(h.ChainW1.data(), h.ChainB1.data(), offset, 3, h.ChainHidden, _cellChain.data());
        float chain = h.ChainB2;
        for (uint32 k = 0; k < h.ChainHidden; ++k)
            chain += h.ChainW2[k] * std::tanh(_cellChain[k]);
        score += chain;
        if (best < 0 || score > top)
        {
            best = int32(block);
            top = score;
        }
    }
    return best < 0 ? 0 : best;
}

void Animus::MlpPolicy::DecideGoals(float const* obs, Camera const* camera, float* in, float const* raw,
    uint32 features, State* state)
{
    // The learner's LayoutActor.decide_goals with GoalHead.draw, deterministic (every choice its argmax, the first of
    // equals as torch's): an ended secondary is dropped; an ended primary is replaced by the queue's head, with no
    // choice; a choice is made on the clock, on an ended primary with nothing queued, or on the block's event. A choice
    // draws the primary, then each later slot from the features plus its bias plus what was drawn before it, each able
    // to say none; with a cell pointer every slot whose goal is the cell goal also names a block of the crop.
    uint32 const kinds = _goalCount, targets = _goalTargets;
    uint32 const count = kinds * targets;
    bool const paired = _goalSlots > 1;
    float const* block = _goalBlockAt >= 0 ? obs + _goalBlockAt : nullptr;
    uint32 const base = kinds + targets;
    bool const ended = block && block[base] > 0.5f;
    bool const secondaryEnded = paired && block && block[base + 2] > 0.5f;
    bool const event = paired && block && block[base + 3] > 0.5f;
    auto const present = [&](uint32 joint)
    {
        return block[joint / targets] > 0.5f && block[kinds + joint % targets] > 0.5f;
    };

    uint32 const held = state ? state->Goal : 0;
    int32 primary = paired ? int32(held / (count + 1)) : int32(held);
    int32 secondary = paired ? int32(held % (count + 1)) - 1 : -1;
    std::array<int32, 2> queue = state ? state->Queue : std::array<int32, 2>{ -1, -1 };
    if (secondaryEnded)
        secondary = -1;
    bool promote = false;
    if (paired && ended && queue[0] >= 0)
    {
        primary = queue[0];
        queue = { _goalSlots > 3 ? queue[1] : -1, -1 };
        promote = true;
    }
    bool const clock = !state || state->Age % _goalEvery == 0;
    bool const choose = clock || (ended && !promote) || event;
    std::array<int32, MAX_GOAL_SLOTS> slots{ -1, -1, -1, -1 };
    std::array<int32, MAX_GOAL_SLOTS> cells{ -1, -1, -1, -1 };
    bool const cellOn = _cells.On && paired && camera && camera->Crop;

    if (choose)
    {
        // What the goal head reads: the slow loop stepped on the fed-back features, or the plain features.
        float const* source = raw;
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

        auto const dot = [width](float const* reads, float const* weights, float bias)
        {
            float sum = bias;
            for (uint32 col = 0; col < width; ++col)
                sum += weights[col] * reads[col];
            return sum;
        };
        auto const factored = [&](Factored const& part, float const* reads, uint32 kind, uint32 target)
        {
            float score = dot(reads, part.KindWeight.data() + std::size_t(kind) * width, part.KindBias[kind])
                + part.Pair[std::size_t(kind) * targets + target];
            if (targets > 1)
                score += dot(reads, part.TargetWeight.data() + std::size_t(target) * width, part.TargetBias[target]);
            return score;
        };

        // The place pointer's score per target (GoalHead.place_scores): zero inputs for a layout without the block.
        std::fill(_placeScores.begin(), _placeScores.end(), 0.0f);
        if (_place.On)
            for (uint32 slot = 0; slot < _place.Count; ++slot)
            {
                float score = _place.B2;
                for (uint32 k = 0; k < _place.Hidden; ++k)
                {
                    float sum = _place.B1[k];
                    for (uint32 f = 0; f < _place.Features; ++f)
                        sum += _place.W1[std::size_t(k) * _place.Features + f]
                            * (block ? block[_place.At + slot * _place.Features + f] : 0.0f);
                    score += _place.W2[k] * std::tanh(sum);
                }
                _placeScores[_place.First + slot] = score;
            }

        // A goal's score: its kind's (given), its target's (targetScores, last filled), the pair's and the place's.
        auto const score = [&](uint32 kind, float kindScore, uint32 target)
        {
            return kindScore + _targetScores[target] + _pair[std::size_t(kind) * targets + target]
                + _placeScores[target];
        };
        auto const targetScores = [&](float const* reads)
        {
            for (uint32 target = 0; target < targets; ++target)
                _targetScores[target] = targets > 1
                    ? dot(reads, _targetWeight.data() + std::size_t(target) * width, _targetBias[target]) : 0.0f;
        };

        // The primary (GoalHead.logits): accepts, the block's kinds and targets (a layout without the block has
        // nothing where the stage has goal blocks), the lookahead; the first goal only when nothing else is allowed.
        targetScores(source);
        auto const primaryAllowed = [&](uint32 joint)
        {
            if (!_accepts[joint])
                return false;
            if (targets > 1)
                return block ? present(joint) : !_goalSpace;
            return true;
        };
        bool anyAllowed = false;
        for (uint32 joint = 0; joint < count && !anyAllowed; ++joint)
            anyAllowed = primaryAllowed(joint);
        int32 drawnPrimary = -1;
        float top = 0.0f;
        for (uint32 kind = 0; kind < kinds; ++kind)
        {
            float const kindScore = dot(source, _kindWeight.data() + std::size_t(kind) * width, _kindBias[kind]);
            for (uint32 target = 0; target < targets; ++target)
            {
                uint32 const joint = kind * targets + target;
                if (!(primaryAllowed(joint) || (joint == 0 && !anyAllowed)))
                    continue;
                float value = score(kind, kindScore, target);
                if (_lookahead)
                    value += _lookaheadWeight[0] * factored(_success, source, kind, target)
                        + _lookaheadWeight[1] * Sigmoid(factored(_duration, source, kind, target));
                if (drawnPrimary < 0 || value > top)
                {
                    drawnPrimary = int32(joint);
                    top = value;
                }
            }
        }
        slots[0] = drawnPrimary;

        // The cells: the choosable blocks less every block within one of a cell drawn before, scored from the
        // previous cell of the plan (the seat's own block, the grid's middle, at the start).
        uint32 const grid = _cells.Grid;
        float previous[2] = { float(grid - 1) / 2.0f, float(grid - 1) / 2.0f };
        std::array<int32, MAX_GOAL_SLOTS> earlierRow{}, earlierCol{};
        std::array<bool, MAX_GOAL_SLOTS> earlierCell{};
        uint32 earlier = 0;
        auto const allowedBlocks = [&]()
        {
            bool any = false;
            for (uint32 b = 0; b < grid * grid; ++b)
            {
                bool ok = _cellValid[b] != 0;
                for (uint32 e = 0; ok && e < earlier; ++e)
                    if (earlierCell[e] && std::abs(int32(b / grid) - earlierRow[e]) <= 1
                        && std::abs(int32(b % grid) - earlierCol[e]) <= 1)
                        ok = false;
                _cellAllowed[b] = ok;
                any = any || ok;
            }
            return any;
        };
        auto const drawCell = [&](float const* reads, int32 goal) -> int32
        {
            int32 const chosenBlock = BestCell(reads, previous[0], previous[1], _cellAllowed);
            bool const isCell = goal == int32(_cells.Joint);
            earlierRow[earlier] = chosenBlock / int32(grid);
            earlierCol[earlier] = chosenBlock % int32(grid);
            earlierCell[earlier] = isCell;
            ++earlier;
            if (!isCell)
                return -1;
            previous[0] = float(chosenBlock / int32(grid));
            previous[1] = float(chosenBlock % int32(grid));
            return chosenBlock;
        };
        if (cellOn)
        {
            CellInputs(camera->Crop);
            allowedBlocks();
            cells[0] = drawCell(source, slots[0]);
        }

        if (paired)
        {
            std::array<int32, MAX_GOAL_SLOTS> drawn{};
            uint32 drawnCount = 0;
            drawn[drawnCount++] = slots[0];
            for (uint32 slot = 1; slot < _goalSlots; ++slot)
            {
                bool const cellOk = cellOn ? allowedBlocks() : true;
                float const* bias = _slotBias.data() + std::size_t(slot - 1) * width;
                for (uint32 col = 0; col < width; ++col)
                    _shifted[col] = source[col] + bias[col];
                for (uint32 index = 0; index < drawnCount; ++index)
                {
                    float const* row = _drawn.data() + std::size_t(drawn[index] + 1) * width;
                    for (uint32 col = 0; col < width; ++col)
                        _shifted[col] += row[col];
                }
                targetScores(_shifted.data());
                // GoalHead.slot_logits: accepts and the block's kinds and targets, no goal already drawn at this
                // choice after the secondary (the cell goal excepted), the cell goal only with a block left; the
                // first goal when nothing is allowed (masked_logits), then none; only none without the block.
                auto const allowed = [&](uint32 joint)
                {
                    bool ok = _accepts[joint] && (!block || present(joint));
                    if (slot >= 2)
                        for (uint32 index = 0; ok && index < drawnCount; ++index)
                            if (drawn[index] >= 0 && drawn[index] == int32(joint)
                                && !(_cells.On && joint == _cells.Joint))
                                ok = false;
                    if (ok && cellOn && joint == _cells.Joint && !cellOk)
                        ok = false;
                    return ok;
                };
                bool any = false;
                for (uint32 joint = 0; joint < count && !any; ++joint)
                    any = allowed(joint);
                int32 pick = -1;
                float best = _noneBias[slot - 1];
                if (!_goalSpace || block)
                {
                    int32 top = -1;
                    float topScore = 0.0f;
                    for (uint32 kind = 0; kind < kinds; ++kind)
                    {
                        float const kindScore = dot(_shifted.data(), _kindWeight.data() + std::size_t(kind) * width,
                            _kindBias[kind]);
                        for (uint32 target = 0; target < targets; ++target)
                        {
                            uint32 const joint = kind * targets + target;
                            if (!(allowed(joint) || (joint == 0 && !any)))
                                continue;
                            float const value = score(kind, kindScore, target);
                            if (top < 0 || value > topScore)
                            {
                                top = int32(joint);
                                topScore = value;
                            }
                        }
                    }
                    // The masked goals' logit stands in when nothing else is allowed; none wins only above the best.
                    if (top >= 0 && topScore >= best)
                        pick = top;
                }
                slots[slot] = pick;
                if (cellOn)
                    cells[slot] = drawCell(_shifted.data(), pick);
                drawn[drawnCount++] = pick;
            }
        }
        primary = slots[0];
        if (paired)
        {
            secondary = slots[1];
            queue = { _goalSlots > 2 ? slots[2] : -1, _goalSlots > 3 ? slots[3] : -1 };
        }
    }
    if (paired && secondary == primary && !(_cells.On && primary == int32(_cells.Joint)))
        secondary = -1;

    // The cell words at the plan's four positions (the learner's _plan_words).
    if (state && _cells.On)
    {
        std::array<uint32, 4>& plan = state->Plan;
        if (secondaryEnded)
            plan[1] = 0;
        if (promote)
            plan = { plan[2], plan[1], plan[3], 0 };
        if (choose)
            for (uint32 position = 0; position < 4; ++position)
            {
                int32 const cell = position < _goalSlots ? cells[position] : -1;
                uint32 const ticket = ((state->Serial + 1) * 4 + position) % TICKET_COUNT + 1;
                plan[position] = cell >= 0 ? (ticket << CELL_BITS) | uint32(cell + 1) : 0;
            }
    }
    if (state)
    {
        state->Age = choose ? 1 : state->Age + 1;
        state->Serial += choose ? 1 : 0;
        state->Goal = paired ? uint32(std::size_t(primary) * (count + 1) + std::size_t(secondary + 1))
            : uint32(primary);
        state->Queue = queue;
    }
    _lastChosen = choose;
    if (choose)
        _lastCells = cells;

    // The goals on the features the action head reads: features x (1 + scale) + embedding, the primary's and the
    // secondary's through the gate (GoalEmbedding.condition).
    auto const value = [&](std::vector<float> const& kind, std::vector<float> const& target, int32 goal, uint32 col)
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

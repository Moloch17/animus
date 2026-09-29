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

#ifndef ANIMUS_LIB_MLP_POLICY_H
#define ANIMUS_LIB_MLP_POLICY_H

#include "Define.h"
#include <algorithm>
#include <string>
#include <vector>

namespace Animus
{
    /// An exported trained actor (.amdl, format version AMDL_VERSION): dense layers with tanh between
    /// them, fed the observation followed by a one-hot agent id.
    ///
    /// A model may also carry a memory (a GRU between the trunk and the action head, kept from decision to decision
    /// and cleared when a fight is over) and goals (one chosen every few decisions from its own head and kept in
    /// between, added to the features the action head reads). Both live in the caller's State, so one policy serves
    /// every seat that plays it.
    class MlpPolicy
    {
    public:
        /// What one seat carries between its decisions. A model without a memory or goals never touches it.
        struct State
        {
            std::vector<float> Memory;      // the GRU's state; sized on first use
            std::vector<float> SlowMemory;  // the two-clock seat's slow loop, stepped when a goal is chosen
            uint32 Goal = 0;
            uint32 Age = 0;                 // decisions the goal has been held, 0 = choose one now

            /// Nothing remembered and no goal: a new fight starts here.
            void Clear()
            {
                std::fill(Memory.begin(), Memory.end(), 0.0f);
                std::fill(SlowMemory.begin(), SlowMemory.end(), 0.0f);
                Goal = 0;
                Age = 0;
            }
        };

        /// Load `path` and check it was trained for `scenario` with these shapes. On any failure the
        /// policy is left unloaded and `error` says why.
        bool Load(std::string const& path, std::string const& scenario, uint32 obsDim, uint32 numActions,
            std::string& error);

        void Unload();

        [[nodiscard]] bool IsLoaded() const { return !_layers.empty(); }

        /// "9+1 -> 128 -> 128 -> 3"
        [[nodiscard]] std::string Describe() const;

        /// Greedy action for agent 0: the allowed action with the highest logit, or 0 when nothing is
        /// allowed. obs: [ObsDim], mask: [NumActions]. Does not allocate after the first decision. Not
        /// thread-safe (shared scratch buffers); call from one thread. `state` carries this seat's memory and goal,
        /// and is updated; without one the policy decides as if every decision were its first.
        int32 Decide(float const* obs, uint8 const* mask, State* state = nullptr);

        /// Whether the model carries a memory or goals, so its caller must keep a State per seat.
        [[nodiscard]] bool HasMemory() const { return _recurrentSize != 0; }
        [[nodiscard]] uint32 GoalCount() const { return _goalCount; }       // kinds; 0 without goals
        [[nodiscard]] uint32 GoalTargets() const { return _goalTargets; }

    private:
        struct Layer
        {
            uint32 In = 0;
            uint32 Out = 0;
            std::vector<float> Weight;      // [Out * In], row-major
            std::vector<float> Bias;        // [Out]
        };

        uint32 _obsDim = 0;
        uint32 _numAgents = 0;
        uint32 _numActions = 0;
        std::vector<Layer> _layers;

        /// The memory: one GRU cell (torch.nn.GRUCell's weights, gates in reset, update, candidate order).
        uint32 _recurrentSize = 0;
        std::vector<float> _memoryWeightIn;      // [3R * features]
        std::vector<float> _memoryWeightHidden;  // [3R * R]
        std::vector<float> _memoryBiasIn;        // [3R]
        std::vector<float> _memoryBiasHidden;    // [3R]

        /// The goals (the learner's GoalHead): a goal is kind * targets + target; its logit the kind's plus the
        /// target's plus the pair's, masked by the goal block's columns and the accepts table; what it adds to
        /// the features is its kind's embedding plus its target's.
        uint32 _goalCount = 0;                   // kinds
        uint32 _goalTargets = 1;
        uint32 _goalEvery = 0;
        std::vector<float> _kindWeight;          // [K * features]
        std::vector<float> _kindBias;            // [K]
        std::vector<float> _targetWeight;        // [T * features], empty with one target
        std::vector<float> _targetBias;          // [T]
        std::vector<float> _pair;                // [K * T]
        std::vector<uint8> _accepts;             // [K * T]
        int32 _goalBlockAt = -1;                 // the goal block's first observation column
        std::vector<float> _kindEmbedding;       // [K * features]
        std::vector<float> _targetEmbedding;     // [T * features]
        std::vector<float> _targetScores;        // scratch [T]

        /// A score per (kind, target), built as the goal head's logits are (the learner's _Factored).
        struct Factored
        {
            std::vector<float> KindWeight, KindBias, TargetWeight, TargetBias, Pair;
        };
        /// Goal-level lookahead (Component P, layer 3): the chance each goal is reached and how long it takes,
        /// added to the goal logits by weight.
        bool _lookahead = false;
        Factored _success;
        Factored _duration;
        float _lookaheadWeight[2] = { 0.0f, 0.0f };

        /// Predictions fed back (Component P, layer 2): the foresight head and its projection onto the features.
        uint32 _foresightOutputs = 0;
        std::vector<float> _foresightWeight, _foresightBias, _feedbackWeight, _feedbackBias;
        std::vector<float> _predictions;         // scratch [F]
        std::vector<float> _raw;                 // scratch: the features before the feedback

        /// The two-clock seat's slow loop (a GRU over the fed-back features, stepped when a goal is chosen).
        uint32 _slowSize = 0;
        std::vector<float> _slowWeightIn, _slowWeightHidden, _slowBiasIn, _slowBiasHidden;
        std::vector<float> _slowGates, _slowHiddenGates, _slowOut;

        std::vector<float> _scratchA;
        std::vector<float> _scratchB;
        std::vector<float> _gates;        // the GRU's input part
        std::vector<float> _hiddenGates;  // ... and its remembered part
    };
}

#endif

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
#include <array>
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
    ///
    /// Format 10 adds the camera (the image's and the map crop's bytes come with each decision, Camera), the free look
    /// (Decision::Look), the goal head's place and cell pointers (Decision::Cells, State::Plan) and the learner's current
    /// goal rules. Every choice is the argmax, as the learner evaluates (deterministic): the realm plays its best guess.
    /// tools/golden/ checks this reader against the learner's own network.
    class MlpPolicy
    {
    public:
        /// What one seat carries between its decisions. A model without a memory or goals never touches it.
        struct State
        {
            std::vector<float> Memory;      // the GRU's state; sized on first use
            std::vector<float> SlowMemory;  // the two-clock seat's slow loop, stepped when a goal is chosen
            /// The goal held; with two goals (GoalSlots() > 1) the pair, primary * (count + 1) + secondary + 1, as the
            /// learner's goal_pair: PrimaryOf and SecondaryOf read it.
            uint32 Goal = 0;
            uint32 Age = 0;                 // decisions the goal has been held, 0 = choose one now
            /// The goals queued behind the pair held (-1: none), promoted when the primary ends.
            std::array<int32, 2> Queue{ -1, -1 };
            /// The cell goals' words at the four plan positions (format 10, a cell pointer): 0 none, else
            /// ticket << 12 | (block + 1), as the learner's decide_goals writes them for the sim's goal latch.
            std::array<uint32, 4> Plan{};
            uint32 Serial = 0;              // goal choices made this episode (the tickets are drawn from it)

            /// Nothing remembered and no goal: a new fight starts here.
            void Clear()
            {
                std::fill(Memory.begin(), Memory.end(), 0.0f);
                std::fill(SlowMemory.begin(), SlowMemory.end(), 0.0f);
                Goal = 0;
                Age = 0;
                Queue = { -1, -1 };
                Plan = {};
                Serial = 0;
            }
        };

        /// The camera's bytes of one decision (format 10), as the sim's Vision writes them: the image, ImageBytes()
        /// bytes ([row][col][distance, height, normal, class byte]), and the mental map's crop, CropBytes() bytes
        /// ([row][col][7 channels]). Either may be null for a model that reads none.
        struct Camera
        {
            uint8 const* Image = nullptr;
            uint8 const* Crop = nullptr;
        };

        static constexpr uint32 MAX_LOOK_HEADS = 8;
        static constexpr uint32 MAX_GOAL_SLOTS = 4;

        /// What a decision chose beside its action (format 10). Everything is the argmax, as the learner evaluates.
        struct Decision
        {
            int32 Action = 0;
            std::array<int32, MAX_LOOK_HEADS> Look{};   // per look head (LookHeads()) its choice; 0 is never "hold"
            bool GoalChosen = false;                    // the goals were chosen this decision
            std::array<int32, MAX_GOAL_SLOTS> Cells{ -1, -1, -1, -1 };  // the block each slot named; -1 none
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
        /// A model with a camera (HasCamera) needs its bytes: this overload decides nothing for it (returns 0).
        int32 Decide(float const* obs, uint8 const* mask, State* state = nullptr);

        /// The same with the camera's bytes (format 10), and what was chosen beside the action into `decision` (the
        /// free look, whether the goals were chosen and the cells named), when given.
        int32 Decide(float const* obs, uint8 const* mask, State* state, Camera const& camera,
            Decision* decision = nullptr);

        /// The logits Decide chooses from, every action's, into `out` [NumActions]; `state` steps as Decide steps it.
        /// False when no model is loaded. For checking the reader against the learner (the golden vectors).
        bool Logits(float const* obs, float* out, State* state = nullptr);

        /// The same with the camera's bytes, and the free look's logits into `look` [LookOutputs()] when given.
        bool Logits(float const* obs, float* out, State* state, Camera const& camera, float* look = nullptr);

        /// Whether the model carries a memory or goals, so its caller must keep a State per seat.
        [[nodiscard]] bool HasMemory() const { return _recurrentSize != 0; }
        [[nodiscard]] uint32 GoalCount() const { return _goalCount; }       // kinds; 0 without goals
        [[nodiscard]] uint32 GoalTargets() const { return _goalTargets; }
        [[nodiscard]] uint32 GoalSlots() const { return _goalSlots; }        // 1: one goal; more: two and a queue
        /// The primary and secondary goal a state holds (kind * targets + target; the secondary -1 for none).
        [[nodiscard]] int32 PrimaryOf(State const& state) const;
        [[nodiscard]] int32 SecondaryOf(State const& state) const;

        [[nodiscard]] uint32 Version() const { return _version; }
        /// The camera (format 10): the bytes a decision needs, and the free look's heads.
        [[nodiscard]] bool HasCamera() const { return _camera.On; }
        [[nodiscard]] uint32 ImageBytes() const { return _camera.On ? _camera.Height * _camera.Width * 4 : 0; }
        [[nodiscard]] uint32 CropBytes() const { return _camera.On ? _camera.CropBytes : 0; }
        [[nodiscard]] uint32 LookHeads() const { return uint32(_camera.LookHeads.size()); }
        [[nodiscard]] uint32 LookHeadSize(uint32 head) const { return _camera.LookHeads[head]; }
        [[nodiscard]] uint32 LookOutputs() const { return uint32(_camera.LookB.size()); }
        [[nodiscard]] bool HasCellGoals() const { return _cells.On; }
        /// The last decision's action head input (the features after the goal's conditioning) and what the camera
        /// added to the first layer: for checking the reader against the learner.
        [[nodiscard]] std::vector<float> const& LastFeatures() const { return _lastFeatures; }
        [[nodiscard]] std::vector<float> const& LastCameraTerm() const { return _camera.Term; }

    private:
        /// One decision's forward pass: the logits (in scratch, valid until the next call), or nullptr unloaded (or a
        /// camera model without its bytes).
        float const* Forward(float const* obs, State* state, Camera const* camera = nullptr);
        /// What the camera adds to the first layer, into _camera.Term (format 10).
        void SeeCamera(float const* obs, Camera const& camera);
        /// The goal decision of format 10 (the learner's LayoutActor.decide_goals, greedy) and its conditioning of the
        /// action head's features `in`; `raw` the features before the predictions were fed back.
        void DecideGoals(float const* obs, Camera const* camera, float* in, float const* raw, uint32 features,
            State* state);
        /// The cell pointer's per-block hidden features and which blocks are choosable, from the crop.
        void CellInputs(uint8 const* crop);
        /// The best block for a slot reading `shifted` from the previous cell (prevRow, prevCol) among `allowed`.
        int32 BestCell(float const* shifted, float prevRow, float prevCol, std::vector<uint8> const& allowed);
        /// The seat sets' attention layer (format 9), in place on _tokens.
        void Attend();

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
        std::vector<float> _kindScale;           // [K * features] the goal's scale on the features (format 7)
        std::vector<float> _targetScale;         // [T * features]
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

        /// Two goals and a queue (format 6, the learner's GoalHead slots): each slot after the primary reads the
        /// features plus its bias plus what was drawn before it, and can say none; the secondary adds its
        /// embedding to the action head's features through the gate.
        uint32 _goalSlots = 1;
        std::vector<float> _slotBias;            // [(S - 1) * goal width]
        std::vector<float> _drawn;               // [(K * T + 1) * goal width], goal + 1 (0: none)
        std::vector<float> _noneBias;            // [S - 1]
        float _gate = 0.0f;
        std::vector<float> _shifted;             // scratch [goal width]

        /// Predictions fed back (Component P, layer 2): the foresight head and its projection onto the features.
        uint32 _foresightOutputs = 0;
        std::vector<float> _foresightWeight, _foresightBias, _feedbackWeight, _feedbackBias;
        std::vector<float> _predictions;         // scratch [F]
        std::vector<float> _raw;                 // scratch: the features before the feedback

        /// The two-clock seat's slow loop (a GRU over the fed-back features, stepped when a goal is chosen).
        uint32 _slowSize = 0;
        std::vector<float> _slowWeightIn, _slowWeightHidden, _slowBiasIn, _slowBiasHidden;
        std::vector<float> _slowGates, _slowHiddenGates, _slowOut;

        /// The director's members and enemies as sets (the learner's DirectorSets): a shared encoder per set, their
        /// pooled encodings added to the first layer, and pointer heads scoring the per-slot actions.
        struct SetEncoder
        {
            uint32 First = 0, Slots = 0, Width = 0, Present = 0;
            std::vector<float> W1, B1, W2, B2;
        };
        struct Pointer
        {
            uint32 First = 0;
            uint32 Over = 0;                     // 0 members, 1 enemies
            std::vector<float> Weight, Bias;
        };
        bool _sets = false;
        uint32 _embed = 0;
        SetEncoder _members, _enemies;
        std::vector<float> _poolWeight, _poolBias;
        std::vector<Pointer> _pointers;
        std::vector<float> _memberCodes, _enemyCodes, _pooled, _setExtra, _query, _setHidden;   // scratch

        /// A seat layout's entities as sets (format 8; the learner's EntitySets): per set a slot gathered from its
        /// segments of the observation, one encoder per set, the present slots pooled (mean and max) onto the first
        /// layer, and pointer heads scoring the actions that name a slot. None in a version 7 model.
        struct SeatSet
        {
            uint32 Slots = 0, Width = 0, Present = 0, PresentStride = 0;
            std::vector<std::pair<uint32, uint32>> Segments;     // (first column of slot 0, columns a slot)
            std::vector<float> W1, B1, W2, B2;
        };
        struct SeatPointer
        {
            uint32 First = 0;
            uint32 Set = 0;
            std::vector<float> Weight, Bias;
        };
        /// Format 9: one pre-norm attention layer over the seat's tokens -- its own token, then every set's slots
        /// (code + the set's type embedding) -- the absent slots no keys; the sets then pool the attended slots and
        /// the attended own token joins the pool (its columns last).
        struct SeatAttention
        {
            uint32 Heads = 0;
            std::vector<float> TypeEmbed, OwnToken, NormAttendW, NormAttendB, InProjW, InProjB, OutProjW, OutProjB,
                NormMixW, NormMixB, MixInW, MixInB, MixOutW, MixOutB;
        };
        bool _seatAttention = false;
        SeatAttention _attention;
        std::vector<float> _tokens, _normed, _qkv, _merged, _mixed, _scores;   // scratch [tokens * ...]
        std::vector<uint8> _keys;                                              // scratch: a present key each token
        bool _seatSets = false;
        uint32 _seatEmbed = 0;
        std::vector<SeatSet> _seatSetList;
        std::vector<float> _seatPoolWeight, _seatPoolBias;
        std::vector<SeatPointer> _seatPointers;
        std::vector<std::vector<float>> _seatCodes;              // scratch: per set [slots * embed]
        std::vector<float> _seatPooled, _seatExtra, _seatRaw;    // scratch

        /// Format 10's tail.
        uint32 _version = 0;
        bool _goalSpace = false;                 // the stage had a goal block: a layout without one chooses nothing

        /// The goal head's pointer over place slots: a per-slot MLP score added to its target's logit.
        struct PlacePointer
        {
            bool On = false;
            uint32 At = 0, First = 0, Count = 0, Features = 0, Hidden = 0;
            std::vector<float> W1, B1, W2;
            float B2 = 0.0f;
        };
        PlacePointer _place;
        std::vector<float> _placeScores;         // scratch [goal targets]

        /// The goal head's cell pointer (CellPointer): a cell goal names a block of the map's crop.
        struct CellHead
        {
            bool On = false;
            uint32 CropOffset = 0, Grid = 0, Pool = 0, Joint = 0, Rise = 0, MinFloor = 0, Features = 0, Hidden = 0,
                ChainHidden = 0;
            std::vector<float> LocalW, LocalB, QueryW, QueryB, ScoreW, ChainW1, ChainB1, ChainW2;
            float ScoreB = 0.0f, ChainB2 = 0.0f;
        };
        CellHead _cells;
        std::vector<float> _cellFeatures, _cellHidden, _cellExtra, _cellQuery, _cellChain;     // scratch
        std::vector<uint8> _cellValid, _cellAllowed;                               // scratch [blocks]

        /// The camera (VisionEncoder with its map and entity list), its join onto the first layer, and the free look.
        struct Camera10
        {
            bool On = false;
            uint32 Height = 0, Width = 0, Patch = 0, Scalars = 0, First = 0, CropBytes = 0;
            std::array<int8, 32> ClassPlane{};   // a pixel's class (b & 31) -> its one-hot plane, -1 none
            uint32 Planes = 0;                   // 4 + the pixel classes
            uint32 ClassLimit = 0, ClassWidth = 0;
            std::vector<float> ClassEmbed;
            uint32 PatchOut = 0, MixOut = 0, Embed = 0;
            std::vector<float> PatchWT;          // [patch inputs][PatchOut], transposed for the sparse pixel planes
            std::vector<float> PatchB, MixW, MixB, EmbedW, EmbedB;
            std::vector<float> GridX, GridY;     // the patch grid's keypoint coordinates

            bool Entities = false;
            uint32 EFirst = 0, ESlots = 0, EWidth = 0, EPresent = 0, EClass = 0, EType = 0;
            int32 EObject = -1, EMemory = -1;
            uint32 Buckets = 0, TypeWidth = 0, Token = 0;
            std::vector<float> TypeEmbed, EW1, EB1, EW2, EB2, EPoolW, EPoolB;

            bool Map = false;
            uint32 MFirst = 0, MScalars = 0, MHeight = 0, MWidth = 0, MPatch = 0, Codes = 0, CodeWidth = 0,
                MPatchOut = 0, MMixOut = 0, MEmbed = 0;
            std::vector<float> CodeTable;        // [cell in patch][code][MPatchOut]: the code's embedding through patch
            std::vector<float> ClassTable;       // [cell in patch][class][MPatchOut]
            std::vector<float> ValueWT;          // [cell in patch][6 values][MPatchOut]
            std::vector<float> MPatchB, MMixW, MMixB, MEmbedW, MEmbedB, MJoinW, MJoinB, MGridX, MGridY;

            std::vector<float> JoinW, JoinB;     // VisionJoin: Embed -> the first layer's width
            std::vector<uint32> LookHeads;
            std::vector<float> LookW, LookB;

            // scratch
            std::vector<float> Acc, Hidden, Features, Pooled, Embedded, Term, Raw, Input, Code, Mean, Max, MapOut;
        };
        Camera10 _camera;
        std::vector<float> _lookLogits;          // scratch [LookOutputs()]
        std::vector<float> _lastFeatures;        // the action head's input of the last decision
        std::array<int32, MAX_GOAL_SLOTS> _lastCells{ -1, -1, -1, -1 };
        bool _lastChosen = false;

        std::vector<float> _scratchA;
        std::vector<float> _scratchB;
        std::vector<float> _gates;        // the GRU's input part
        std::vector<float> _hiddenGates;  // ... and its remembered part
    };
}

#endif

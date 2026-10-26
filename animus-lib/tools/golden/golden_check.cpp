/*
 * The realm's forward pass against the learner's: reads an exported model (<model>.amdl) and its golden vectors
 * (<model>.golden, written by the forge's `python -m animus.golden`), replays the seat's decisions through
 * MlpPolicy from a cleared state, and reports the largest difference of every output. Exit 1 when a float output
 * differs by more than the tolerance or a discrete one (the action, the look, the goals, the cells, the plan) at all.
 *
 *     golden_check <dir> <model> [<model> ...] [--tolerance 1e-4] [--time N]
 *
 * --time N then times N decisions of the first step's inputs (one without a goal choice and one with), single thread.
 * Built by build.sh with the stub headers beside it, against Model/MlpPolicy.cpp alone.
 */

#include "MlpPolicy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    struct Cursor
    {
        std::vector<char> Data;
        std::size_t At = 0;

        template <typename T>
        void Take(T* out, std::size_t count)
        {
            std::size_t const bytes = count * sizeof(T);
            if (At + bytes > Data.size())
                throw std::runtime_error("golden file truncated");
            std::memcpy(out, Data.data() + At, bytes);
            At += bytes;
        }

        template <typename T>
        T One()
        {
            T value;
            Take(&value, 1);
            return value;
        }
    };

    struct Step
    {
        std::vector<float> Obs;
        std::vector<uint8> Mask, Image, Crop;
        std::vector<float> Logits, LookLogits, Memory, Slow, Features, Term;
        int32 Action = 0, Goal = 0;
        std::vector<int32> Look;
        uint8 Chosen = 0;
        int32 Queue[2] = {}, Cells[4] = {};
        uint32 Plan[4] = {}, Serial = 0, Age = 0;
    };

    struct Worst
    {
        char const* Name;
        double Value = 0.0;
        int Step = -1;
    };

    void Note(Worst& worst, float const* a, float const* b, std::size_t count, int step,
        std::vector<std::size_t> const* only = nullptr)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (only && std::find(only->begin(), only->end(), i) == only->end())
                continue;
            double const d = std::fabs(double(a[i]) - double(b[i]));
            if (d > worst.Value)
            {
                worst.Value = d;
                worst.Step = step;
            }
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: golden_check <dir> <model> [<model> ...] [--tolerance 1e-4] [--time N]\n");
        return 2;
    }
    std::string const dir = argv[1];
    std::vector<std::string> models;
    double tolerance = 1e-4;
    int timeRuns = 0;
    for (int i = 2; i < argc; ++i)
    {
        std::string const arg = argv[i];
        if (arg == "--tolerance" && i + 1 < argc)
            tolerance = std::stod(argv[++i]);
        else if (arg == "--time" && i + 1 < argc)
            timeRuns = std::stoi(argv[++i]);
        else
            models.push_back(arg);
    }

    bool failed = false;
    for (std::string const& model : models)
    {
        std::ifstream file(dir + "/" + model + ".golden", std::ios::binary);
        Cursor in{ std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()) };
        char magic[4];
        in.Take(magic, 4);
        if (std::memcmp(magic, "AGLD", 4) != 0 || in.One<uint32>() != 1)
        {
            std::fprintf(stderr, "%s: not a golden file of version 1\n", model.c_str());
            return 2;
        }
        uint32 header[11];
        in.Take(header, 11);
        auto const [obsDim, actions, imageBytes, cropBytes, lookTotal, lookHeads, recurrent, slow, features, term,
            steps] = header;

        Animus::MlpPolicy policy;
        std::string error;
        if (!policy.Load(dir + "/" + model + ".amdl", model, obsDim, actions, error))
        {
            std::fprintf(stderr, "%s: %s\n", model.c_str(), error.c_str());
            return 2;
        }
        if (policy.ImageBytes() != imageBytes || policy.CropBytes() != cropBytes || policy.LookOutputs() != lookTotal)
        {
            std::fprintf(stderr, "%s: the model reads %u image and %u crop bytes and has %u look outputs; the golden "
                "file %u, %u, %u\n", model.c_str(), policy.ImageBytes(), policy.CropBytes(), policy.LookOutputs(),
                imageBytes, cropBytes, lookTotal);
            return 2;
        }

        std::vector<Step> recorded(steps);
        for (Step& s : recorded)
        {
            s.Obs.resize(obsDim);
            s.Mask.resize(actions);
            s.Image.resize(imageBytes);
            s.Crop.resize(cropBytes);
            in.Take(s.Obs.data(), obsDim);
            in.Take(s.Mask.data(), actions);
            in.Take(s.Image.data(), imageBytes);
            in.Take(s.Crop.data(), cropBytes);
            s.Logits.resize(actions);
            in.Take(s.Logits.data(), actions);
            s.Action = in.One<int32>();
            s.LookLogits.resize(lookTotal);
            in.Take(s.LookLogits.data(), lookTotal);
            s.Look.resize(lookHeads);
            in.Take(s.Look.data(), lookHeads);
            s.Chosen = in.One<uint8>();
            s.Goal = in.One<int32>();
            in.Take(s.Queue, 2);
            in.Take(s.Cells, 4);
            in.Take(s.Plan, 4);
            s.Serial = in.One<uint32>();
            s.Age = in.One<uint32>();
            s.Memory.resize(recurrent);
            in.Take(s.Memory.data(), recurrent);
            s.Slow.resize(slow);
            in.Take(s.Slow.data(), slow);
            s.Features.resize(features);
            in.Take(s.Features.data(), features);
            s.Term.resize(term);
            in.Take(s.Term.data(), term);
        }

        Worst logits{ "logits (allowed)" }, logitsAll{ "logits (all)" }, look{ "look logits" }, memory{ "memory" },
            slowWorst{ "slow memory" }, feat{ "head features" }, camera{ "camera term" };
        int discrete = 0;
        Animus::MlpPolicy::State state;
        std::vector<float> got(actions), gotLook(lookTotal);
        for (uint32 index = 0; index < steps; ++index)
        {
            Step const& s = recorded[index];
            Animus::MlpPolicy::Camera const cam{ s.Image.data(), s.Crop.data() };
            Animus::MlpPolicy::State before = state;
            if (!policy.Logits(s.Obs.data(), got.data(), &before, cam, gotLook.data()))
            {
                std::fprintf(stderr, "%s step %u: no logits\n", model.c_str(), index);
                return 2;
            }
            Animus::MlpPolicy::Decision decision;
            int32 const action = policy.Decide(s.Obs.data(), s.Mask.data(), &state, cam, &decision);

            std::vector<std::size_t> allowed;
            for (std::size_t a = 0; a < actions; ++a)
                if (s.Mask[a])
                    allowed.push_back(a);
            Note(logits, got.data(), s.Logits.data(), actions, int(index), &allowed);
            Note(logitsAll, got.data(), s.Logits.data(), actions, int(index));
            Note(look, gotLook.data(), s.LookLogits.data(), lookTotal, int(index));
            Note(memory, state.Memory.data(), s.Memory.data(), recurrent, int(index));
            if (slow)
                Note(slowWorst, state.SlowMemory.data(), s.Slow.data(), slow, int(index));
            Note(feat, policy.LastFeatures().data(), s.Features.data(), features, int(index));
            Note(camera, policy.LastCameraTerm().data(), s.Term.data(), term, int(index));

            bool same = action == s.Action && int32(state.Goal) == s.Goal && state.Queue[0] == s.Queue[0]
                && state.Queue[1] == s.Queue[1] && uint8(decision.GoalChosen) == s.Chosen && state.Serial == s.Serial
                && state.Age == s.Age;
            for (uint32 h = 0; h < lookHeads; ++h)
                same = same && decision.Look[h] == s.Look[h];
            for (uint32 c = 0; c < 4; ++c)
                same = same && decision.Cells[c] == s.Cells[c] && state.Plan[c] == s.Plan[c];
            if (!same)
            {
                ++discrete;
                std::printf("  %s step %u differs: action %d/%d goal %u/%d queue %d,%d/%d,%d chosen %d/%d cells "
                    "%d,%d,%d,%d/%d,%d,%d,%d look %d,%d,%d/%d,%d,%d\n", model.c_str(), index, action, s.Action,
                    state.Goal, s.Goal, state.Queue[0], state.Queue[1], s.Queue[0], s.Queue[1],
                    int(decision.GoalChosen), int(s.Chosen), decision.Cells[0], decision.Cells[1], decision.Cells[2],
                    decision.Cells[3], s.Cells[0], s.Cells[1], s.Cells[2], s.Cells[3], decision.Look[0],
                    decision.Look[1], decision.Look[2], lookHeads > 0 ? s.Look[0] : 0, lookHeads > 1 ? s.Look[1] : 0,
                    lookHeads > 2 ? s.Look[2] : 0);
                // Carry on from the learner's state, so one difference is reported once.
                state.Goal = uint32(s.Goal);
                state.Queue = { s.Queue[0], s.Queue[1] };
                state.Serial = s.Serial;
                state.Age = s.Age;
                std::copy(std::begin(s.Plan), std::end(s.Plan), state.Plan.begin());
            }
        }

        double worstFloat = 0.0;
        std::printf("%s (version %u, %s): %u steps, %d with a discrete difference\n", model.c_str(), policy.Version(),
            policy.Describe().c_str(), steps, discrete);
        for (Worst const* w : { &logits, &logitsAll, &look, &feat, &memory, &slowWorst, &camera })
        {
            std::printf("  max |diff| %-17s %.3g (step %d)\n", w->Name, w->Value, w->Step);
            worstFloat = std::max(worstFloat, w->Value);
        }
        if (worstFloat > tolerance || discrete)
            failed = true;

        if (timeRuns > 0)
        {
            // A decision without a goal choice (the clock mid-way) and one with (the clock at its start).
            Step const& s = recorded[0];
            Animus::MlpPolicy::Camera const cam{ s.Image.data(), s.Crop.data() };
            auto const time = [&](bool choose)
            {
                Animus::MlpPolicy::State timed;
                policy.Decide(s.Obs.data(), s.Mask.data(), &timed, cam);
                auto const start = std::chrono::steady_clock::now();
                for (int run = 0; run < timeRuns; ++run)
                {
                    timed.Age = choose ? 0 : 1;
                    policy.Decide(s.Obs.data(), s.Mask.data(), &timed, cam);
                }
                return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count()
                    / timeRuns;
            };
            double const plain = time(false);
            double const choosing = time(true);
            std::printf("  %.1f us a decision, %.1f us with a goal choice (%d runs each)\n", plain, choosing,
                timeRuns);
        }
    }
    std::printf(failed ? "FAILED (tolerance %.0e)\n" : "OK (tolerance %.0e)\n", tolerance);
    return failed ? 1 : 0;
}

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

#include "FollowPolicy.h"
#include "Log.h"
#include "MlpPolicy.h"
#include <algorithm>

namespace
{
    /// One follower's policy: the class's model (shared, world thread only, as ModelLibrary keeps it) and this seat's
    /// memory and goals. Every choice is the argmax, as the learner evaluates (train.py, deterministic).
    class MlpFollowPolicy final : public Animus::Follow::FollowPolicy
    {
    public:
        explicit MlpFollowPolicy(Animus::MlpPolicy* model) : _model(model) { }

        bool Decide(Animus::Follow::FollowPolicyInput const& input, Animus::Follow::FollowDecision& decision) override
        {
            if (input.EpisodeStart)
                _state.Clear();
            Animus::MlpPolicy::Camera const camera{ input.Image, input.MapCrop };
            Animus::MlpPolicy::Decision chosen;
            decision.Action = _model->Decide(input.Obs, input.Mask, &_state, camera, &chosen);
            uint32 const heads = std::min<uint32>(_model->LookHeads(), uint32(decision.Look.size()));
            for (uint32 head = 0; head < heads; ++head)
                decision.Look[head] = chosen.Look[head];
            // The goals held after this decision: the pair the state carries (chosen now or held on), as the learner
            // sends them to the sim every decision.
            decision.Goals[0] = _model->GoalCount() ? _model->PrimaryOf(_state) : -1;
            decision.Goals[1] = _model->GoalCount() && _model->GoalSlots() > 1 ? _model->SecondaryOf(_state) : -1;
            return true;
        }

        void Reset() override { _state.Clear(); }

    private:
        Animus::MlpPolicy* _model;
        Animus::MlpPolicy::State _state;
    };
}

std::unique_ptr<Animus::Follow::FollowPolicy> Animus::Follow::MakeFollowPolicy(std::string const& modelName,
    MlpPolicy* model)
{
    if (!model || !model->IsLoaded())
    {
        LOG_ERROR("module.animus", "Follow model {}: not loaded", modelName);
        return nullptr;
    }
    if (!model->HasCamera())
    {
        // A follow layout reads the camera: a model without one was exported from something else.
        LOG_ERROR("module.animus", "Follow model {}: it has no camera (format {}); export the follow checkpoint with "
            "format 10", modelName, model->Version());
        return nullptr;
    }
    return std::make_unique<MlpFollowPolicy>(model);
}

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

#include "AnimusConfig.h"
#include "Config.h"
#include "Log.h"
#include <array>
#include <string_view>
#include <algorithm>

void Animus::AnimusConfig::Load()
{
    Enable = sConfigMgr->GetOption<bool>("Animus.Enable", true);
    ModelDir = sConfigMgr->GetOption<std::string>("Animus.ModelDir", "animus");
    DecisionMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Animus.DecisionMs", 50));
    CurriculumDecisionMs = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Animus.Curriculum.DecisionMs", 100));

    // Stage names as in the forge's scenario names (class_role_<stage>).
    static constexpr std::array<std::pair<std::string_view, Curriculum::Stage>, 8> STAGES =
    { {
        { "dummy", Curriculum::Stage::Dummy }, { "duel", Curriculum::Stage::Duel }, { "pack", Curriculum::Stage::Pack },
        { "gauntlet", Curriculum::Stage::Gauntlet }, { "companion", Curriculum::Stage::Companion },
        { "party", Curriculum::Stage::Party }, { "pvp", Curriculum::Stage::Pvp }, { "arena", Curriculum::Stage::Arena },
    } };

    std::string const stage = sConfigMgr->GetOption<std::string>("Animus.Curriculum.Stage", "party");
    auto const itr = std::find_if(STAGES.begin(), STAGES.end(), [&](auto const& entry) { return entry.first == stage; });
    if (itr != STAGES.end())
        CurriculumStage = itr->second;
    else
    {
        CurriculumStage = Curriculum::Stage::Party;
        LOG_ERROR("module.animus", "Animus.Curriculum.Stage \"{}\" is not a stage; using \"party\"", stage);
    }
}

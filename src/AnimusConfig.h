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

#ifndef ANIMUS_CONFIG_H
#define ANIMUS_CONFIG_H

#include "Camera.h"
#include "Define.h"
#include "FollowParty.h"
#include "MentalMap.h"
#include <string>
#include <vector>

namespace Animus
{
    /// Module settings, read at startup and on config reload (conf/mod_animus.conf.dist documents every key).
    struct AnimusConfig
    {
        bool Enable = true;

        /// Animus.ModelDir; a relative one is looked for where the build installs models (see Load).
        std::string ModelDir;

        /// Animus.Curriculum.Stage: the stage whose models the followers play (always one that exists: Load falls back
        /// to move4_follow, then to the first stage).
        std::string CurriculumStage = "move4_follow";

        /// Animus.DataDir: a writable directory for the module's data; the camera scenes go in its scenes/.
        std::string DataDir;
        [[nodiscard]] std::string SceneDir() const;

        /// Animus.Vision.Maps: the maps whose camera scenes are baked and loaded at startup.
        std::vector<uint32> VisionMaps;
        /// The camera the models trained with (Animus.Vision.*), with the one render size the realm casts at as its
        /// only RenderSizes entry; and the mental map's caps (Animus.Map.*, KeepShare 0: the realm's keep rule is the
        /// follow driver's).
        Vision::Settings Camera;
        /// Animus.Vision.RenderSize: the one of Camera.RenderSizes every follower casts its frames at.
        Vision::Resolution Render;
        Vision::MapRunSettings MapRun;
        uint32 ClusterMinCells = 2;
        uint32 PocketMinCells = 6;

        /// The follow driver's decision clock and tuning (Animus.Curriculum.*, Animus.Follow.*).
        Follow::FollowSettings FollowTuning;
        /// Animus.Follow.Security: the account security `.animus follow` and the addon's Summon need (0: every
        /// player; 2: game masters, as the live follow test had it).
        uint32 FollowSecurity = 0;

        /// Animus.PartyFill.Enable: a player entering a five-player dungeon alone, or as its group's leader, gets
        /// followers summoned to fill the group (the player's own setting and a game master's realm override apply:
        /// AnimusMod::PartyFillFor).
        bool PartyFill = true;
        /// Animus.Prepare.*: the account security the addon's Prepare needs (SEC_GAMEMASTER = 2 by default; 0 lets
        /// every player), and the bag it puts in each empty bag slot afterwards (0 for none).
        uint32 PrepareSecurity = 2;
        uint32 PrepareBagItem = 4500;

        void Load();
    };
}

#endif

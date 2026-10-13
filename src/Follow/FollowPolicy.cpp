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

std::unique_ptr<Animus::Follow::FollowPolicy> Animus::Follow::MakeFollowPolicy(std::string const& modelName)
{
    // The .amdl format 10 reader (image and map crop inputs, look and goal heads) is being written on another branch
    // against Model/MlpPolicy; until it is linked here no follower can be summoned.
    LOG_ERROR("module.animus", "Follow model {}: follow model runtime not yet linked", modelName);
    return nullptr;
}

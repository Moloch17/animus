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

#ifndef ANIMUS_LIB_MODEL_LIBRARY_H
#define ANIMUS_LIB_MODEL_LIBRARY_H

#include "MlpPolicy.h"
#include <string>
#include <unordered_map>

namespace Animus
{
    namespace Curriculum
    {
        struct Layout;
    }

    /// The class/role models of the model directory, loaded on first use.
    ///
    /// A layout's model is <dir>/<model name>.amdl (warrior_dps_party.amdl) with its layout manifest beside it
    /// (warrior_dps_party.json, exported with the model). The model is used only if that manifest is exactly the
    /// server's own manifest for the layout: the same stage, class/role, sizes, block offsets, actions and talents. A
    /// model trained on a different layout would read observations and actions as something else.
    ///
    /// World thread only.
    class ModelLibrary
    {
    public:
        /// Forget every model (and every failure); the next Find loads from `dir`.
        void Reset(std::string dir);

        /// The layout's model, or null with `error` set. A failure is remembered until the next Reset.
        MlpPolicy* Find(Curriculum::Layout const& layout, std::string& error);

    private:
        struct Entry
        {
            MlpPolicy Policy;
            std::string Error;
        };

        std::string _dir;
        std::unordered_map<std::string, Entry> _models;     // by model name
    };
}

#endif

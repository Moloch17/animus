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

#ifndef ANIMUS_COMPANION_LOADER_H
#define ANIMUS_COMPANION_LOADER_H

#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"
#include <functional>
#include <string>
#include <vector>

class Player;

namespace Animus::CompanionLoader
{
    /// A saved companion character comes back into the world: its rows are read as a login reads them (the core's
    /// LoginQueryHolder, copied), on the database thread; when they arrive, Update builds the Player on the world
    /// thread, as the core's HandlePlayerLoginFromDB does for a client, and calls `done` with it -- or with null
    /// when the load failed (logged). The caller places it and takes it over.
    using Done = std::function<void(Player* bot)>;
    void Begin(ObjectGuid bot, uint32 account, std::string const& name, Done done);

    /// World thread: finish the loads whose rows have arrived.
    void Update();

    /// A loaded companion the caller could not place: gone as if never loaded. The character cache keeps it.
    void Discard(Player* bot);
}

#endif

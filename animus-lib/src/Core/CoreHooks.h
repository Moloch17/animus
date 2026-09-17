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

#ifndef ANIMUS_LIB_CORE_HOOKS_H
#define ANIMUS_LIB_CORE_HOOKS_H

#include "Define.h"

class Group;
class WorldSession;

/*
 * What only the forge core can do, for code that also runs on a stock AzerothCore. The library calls these seams
 * wherever the forge needs its core's help; they do nothing until a host fills them in, which mod-animus-forge does at
 * load (it only builds on the forge core). On a stock core the calls are no-ops: bots write the few database rows a
 * logout writes, and groups are saved like any other.
 */
namespace Animus::CoreHooks
{
    struct Seams
    {
        /// A bot's session: no account or character rows exist, so logout, play time and instance binds write nothing.
        void (*MarkSimSession)(WorldSession* session) = nullptr;

        /// A group of bots that is never written to the character database.
        void (*MarkSimGroup)(Group* group) = nullptr;

        /// Restart the world thread's random numbers from `seed` (0: back to entropy).
        void (*SeedRandom)(uint32 seed) = nullptr;
    };

    /// Set the seams (once, at load, before any bot is created).
    void Install(Seams const& seams);

    void MarkSimSession(WorldSession* session);

    /// Whether bot sessions are sim sessions (the forge core): their instance binds were never written to the
    /// character database, so there is nothing to delete when a bot leaves.
    [[nodiscard]] bool HasSimSessions();
    void MarkSimGroup(Group* group);

    /// False when the core cannot reseed (a stock core): the numbers stay random.
    bool SeedRandom(uint32 seed);
}

#endif

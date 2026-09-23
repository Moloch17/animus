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

#include "CoreHooks.h"

namespace
{
    Animus::CoreHooks::Seams Installed;
}

void Animus::CoreHooks::Install(Seams const& seams)
{
    Installed = seams;
}

void Animus::CoreHooks::MarkSimSession(WorldSession* session)
{
    if (Installed.MarkSimSession)
        Installed.MarkSimSession(session);
}

bool Animus::CoreHooks::HasSimSessions()
{
    return Installed.MarkSimSession != nullptr;
}

void Animus::CoreHooks::MarkSimGroup(Group* group)
{
    if (Installed.MarkSimGroup)
        Installed.MarkSimGroup(group);
}

bool Animus::CoreHooks::SeedRandom(uint32 seed)
{
    if (!Installed.SeedRandom)
        return false;

    Installed.SeedRandom(seed);
    return true;
}

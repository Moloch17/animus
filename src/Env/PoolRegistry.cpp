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

#include "PoolRegistry.h"
#include <algorithm>

namespace
{
    std::vector<Animus::EnvPool*> Registered;
}

void Animus::PoolRegistry::Register(EnvPool* pool)
{
    if (pool && std::find(Registered.begin(), Registered.end(), pool) == Registered.end())
        Registered.push_back(pool);
}

void Animus::PoolRegistry::Unregister(EnvPool* pool)
{
    std::erase(Registered, pool);
}

std::vector<Animus::EnvPool*> const& Animus::PoolRegistry::Pools()
{
    return Registered;
}

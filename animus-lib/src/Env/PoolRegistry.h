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

#ifndef ANIMUS_LIB_POOL_REGISTRY_H
#define ANIMUS_LIB_POOL_REGISTRY_H

#include <vector>

namespace Animus
{
    class EnvPool;

    /// The env pools running in this server, which the library's damage, heal and spell hooks (Hooks/) feed.
    ///
    /// Register and Unregister run on the world thread while no map is updating (a WorldScript::OnUpdate, a shutdown);
    /// the hooks read the list from map threads without a lock, as they read a pool's own indexes.
    namespace PoolRegistry
    {
        void Register(EnvPool* pool);
        void Unregister(EnvPool* pool);

        [[nodiscard]] std::vector<EnvPool*> const& Pools();
    }
}

#endif

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

#ifndef ANIMUS_ANIMUS_PLAYER_SETTINGS_H
#define ANIMUS_ANIMUS_PLAYER_SETTINGS_H

#include "Define.h"
#include "ObjectGuid.h"

namespace Animus
{
    /// A player's own settings of the module (2026-10-25: whether their dungeon party is filled with bots), kept
    /// per character in the characters database table `animus_player_settings` -- made on first use with the
    /// companion table's CREATE TABLE IF NOT EXISTS pattern, dropped by a purge -- and in memory from login to
    /// logout. World thread only.
    class PlayerSettings
    {
    public:
        /// The character's row read (a synchronous query at login; no row keeps the defaults).
        static void Load(ObjectGuid guid);
        static void Forget(ObjectGuid guid);

        /// Whether this character wants its dungeon party filled with bots (default on).
        [[nodiscard]] static bool PartyFill(ObjectGuid guid);
        /// ... set, and written to the table at once.
        static void SetPartyFill(ObjectGuid guid, bool on);

        /// The table, gone (AnimusMod::PurgeAll).
        static void DropTable();
    };
}

#endif

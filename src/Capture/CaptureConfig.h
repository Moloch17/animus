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

#ifndef ANIMUS_CAPTURE_CONFIG_H
#define ANIMUS_CAPTURE_CONFIG_H

#include "Define.h"
#include <string>

namespace Animus::Capture
{
    /// Animus.Capture.*: recording play to files (mod_animus.conf.dist documents every key). One tier (format 5):
    /// every player on every map, from login, at full detail, into the hourly files. Read at startup and on config
    /// reload; Dir and BufferRecords take effect at startup only.
    struct CaptureConfig
    {
        bool Enable = false;
        /// Where the files go: <Dir>/<yyyy-mm-dd>/<hh>/<stream>-<map>.bin.gz, index.json per hour, and the salt.
        /// Unset in the conf: <LogsDir>/animus-capture.
        std::string Dir;
        uint32 SnapshotMs = 100;            // a player's own snapshot while in combat or moving
        uint32 IdleSnapshotMs = 1000;       // ... otherwise
        uint32 DetailMs = 1000;             // a player's PlayerDetail
        uint32 UnitMs = 100;                // one pass over a map instance's units and objects this often
        float UnitRange = 150.0f;           // on a world map, the units within this of a human (0: the whole map)
        float MoveYards = 0.5f;             // a unit that moved less than this (and turned < 0.1 rad) is unchanged
        /// Which streams are written: 1 session, 2 move, 4 action, 8 snapshot, 16 outcome, 32 companion, 64 note
        /// (chat). Bit 128 (format 4's packets) is reserved.
        uint32 Streams = 127;
        uint32 FlushMs = 10000;             // how often buffered records become a gzip member on disk
        uint32 BufferRecords = 131072;      // per producing thread, in 128-byte record slots
        float DiskWarnGB = 50.0f;
        float DiskReserveGB = 10.0f;
        std::string LoginNotice;            // the notice at login

        void Load();
    };
}

#endif

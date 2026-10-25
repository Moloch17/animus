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
#include <vector>

namespace Animus::Capture
{
    /// Animus.Capture.*: recording play to files (mod_animus.conf.dist documents every key). Two tiers share the
    /// writer: the realm-wide **automatic** capture of every player (Enable + Auto: the hourly files), and the
    /// **manual recordings** a player starts of their own play (Enable alone makes them available: `recordings/<id>/`,
    /// richer and faster, the Recording.* keys). Read at startup and on config reload; Dir and BufferRecords take
    /// effect at startup only.
    struct CaptureConfig
    {
        bool Enable = false;
        /// The automatic tier: every player from login into the hourly files. Off leaves only manual recordings.
        bool Auto = true;
        /// Where the files go: <Dir>/<yyyy-mm-dd>/<hh>/<stream>-<map>.bin.gz, index.json per hour, the salt, and
        /// <Dir>/recordings/<id>/ for the manual recordings.
        std::string Dir;
        uint32 SnapshotMs = 250;            // while in combat or moving
        uint32 IdleSnapshotMs = 1000;       // otherwise
        /// Which streams the automatic tier writes: 1 session, 2 move, 4 action, 8 snapshot, 16 outcome, 32
        /// companion, 64 note, 128 packet. A manual recording writes every stream.
        uint32 Streams = 255;
        uint32 FlushMs = 1000;              // how often buffered records become a gzip member on disk
        uint32 BufferRecords = 131072;      // per producing thread, in 128-byte record slots
        float DiskWarnGB = 50.0f;
        float DiskReserveGB = 10.0f;
        std::string LoginNotice;            // the automatic tier's notice at login

        /// Animus.Capture.Recording.*: a manual recording's rates and reach.
        struct RecordingConfig
        {
            uint32 SnapshotMs = 100;        // the player's Snapshot, moving or not
            uint32 UnitMs = 100;            // one UnitState per visible unit this often
            float UnitRange = 80.0f;        // ... within this many yards
            uint32 MaxUnits = 128;          // ... the nearest this many
            bool Packets = true;            // the raw packet stream
            uint32 PacketMaxBytes = 1048576;    // a longer packet is cut
            std::vector<uint16> PacketSkip;     // opcodes left out of the packet stream (the chat opcodes, Warden)
            bool OthersChat = false;        // other real players' text outside the group, unredacted
            uint32 MaxMinutes = 240;        // a recording stops by itself after this long
            std::string Notice;             // the chat line at Record
            uint32 GodSecurity = 2;         // the account security `god` needs (SEC_GAMEMASTER)
        } Recording;

        void Load();
    };
}

#endif

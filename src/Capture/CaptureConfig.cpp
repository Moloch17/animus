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

#include "CaptureConfig.h"
#include "CaptureFormat.h"
#include "Config.h"
#include <algorithm>
#include <filesystem>

void Animus::Capture::CaptureConfig::Load()
{
    Enable = sConfigMgr->GetOption<bool>("Animus.Capture.Enable", true);
    Dir = sConfigMgr->GetOption<std::string>("Animus.Capture.Dir", "");
    // Unset: <LogsDir>/animus-capture -- the logs directory is the one place a stock realm (and its container's
    // bind mounts) already writes to; the working directory when there is no LogsDir either.
    if (Dir.empty())
    {
        std::string const logs = sConfigMgr->GetOption<std::string>("LogsDir", "");
        Dir = ((logs.empty() ? std::filesystem::current_path() : std::filesystem::path(logs)) / "animus-capture")
            .string();
    }
    SnapshotMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("Animus.Capture.SnapshotMs", 100));
    IdleSnapshotMs = std::max<uint32>(SnapshotMs, sConfigMgr->GetOption<uint32>("Animus.Capture.IdleSnapshotMs",
        1000));
    DetailMs = std::max<uint32>(100, sConfigMgr->GetOption<uint32>("Animus.Capture.DetailMs", 1000));
    UnitMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("Animus.Capture.UnitMs", 100));
    UnitRange = sConfigMgr->GetOption<float>("Animus.Capture.UnitRange", 150.0f);
    UnitRange = UnitRange > 0.0f ? std::clamp(UnitRange, 10.0f, 1000.0f) : 0.0f;
    MoveYards = std::clamp(sConfigMgr->GetOption<float>("Animus.Capture.MoveYards", 0.5f), 0.0f, 50.0f);
    Streams = sConfigMgr->GetOption<uint32>("Animus.Capture.Streams", Format::STREAMS_ALL) & Format::STREAMS_ALL;
    FlushMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Animus.Capture.FlushMs", 10000), 100, 60000);
    BufferRecords = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Animus.Capture.BufferRecords", 131072), 1024,
        1u << 22);
    DiskWarnGB = std::max(0.0f, sConfigMgr->GetOption<float>("Animus.Capture.DiskWarnGB", 50.0f));
    DiskReserveGB = std::max(0.0f, sConfigMgr->GetOption<float>("Animus.Capture.DiskReserveGB", 10.0f));
    LoginNotice = sConfigMgr->GetOption<std::string>("Animus.Capture.LoginNotice",
        "Play on this realm is recorded (movement, actions, combat, chat and your surroundings; never account "
        "details) to train its bots to play like people.");
}

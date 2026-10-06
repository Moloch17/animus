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
#include "Config.h"
#include <algorithm>

void Animus::Capture::CaptureConfig::Load()
{
    Enable = sConfigMgr->GetOption<bool>("Animus.Capture.Enable", false);
    Dir = sConfigMgr->GetOption<std::string>("Animus.Capture.Dir", "");
    SnapshotMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("Animus.Capture.SnapshotMs", 250));
    IdleSnapshotMs = std::max<uint32>(SnapshotMs, sConfigMgr->GetOption<uint32>("Animus.Capture.IdleSnapshotMs",
        1000));
    Streams = sConfigMgr->GetOption<uint32>("Animus.Capture.Streams", 63) & 63;
    FlushMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Animus.Capture.FlushMs", 1000), 100, 60000);
    BufferRecords = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Animus.Capture.BufferRecords", 32768), 1024,
        1u << 22);
    DiskWarnGB = std::max(0.0f, sConfigMgr->GetOption<float>("Animus.Capture.DiskWarnGB", 50.0f));
    DiskReserveGB = std::max(0.0f, sConfigMgr->GetOption<float>("Animus.Capture.DiskReserveGB", 10.0f));
    LoginNotice = sConfigMgr->GetOption<std::string>("Animus.Capture.LoginNotice",
        "Play on this realm is recorded (movement, actions and combat; never chat or account details) to train "
        "its bots to play like people.");
}

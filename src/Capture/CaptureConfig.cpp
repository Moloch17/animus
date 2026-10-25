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
#include "Opcodes.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include <algorithm>

void Animus::Capture::CaptureConfig::Load()
{
    Enable = sConfigMgr->GetOption<bool>("Animus.Capture.Enable", false);
    Auto = sConfigMgr->GetOption<bool>("Animus.Capture.Auto", true);
    Dir = sConfigMgr->GetOption<std::string>("Animus.Capture.Dir", "");
    SnapshotMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("Animus.Capture.SnapshotMs", 250));
    IdleSnapshotMs = std::max<uint32>(SnapshotMs, sConfigMgr->GetOption<uint32>("Animus.Capture.IdleSnapshotMs",
        1000));
    Streams = sConfigMgr->GetOption<uint32>("Animus.Capture.Streams", Format::STREAMS_ALL) & Format::STREAMS_ALL;
    FlushMs = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Animus.Capture.FlushMs", 1000), 100, 60000);
    BufferRecords = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Animus.Capture.BufferRecords", 131072), 1024,
        1u << 22);
    DiskWarnGB = std::max(0.0f, sConfigMgr->GetOption<float>("Animus.Capture.DiskWarnGB", 50.0f));
    DiskReserveGB = std::max(0.0f, sConfigMgr->GetOption<float>("Animus.Capture.DiskReserveGB", 10.0f));
    LoginNotice = sConfigMgr->GetOption<std::string>("Animus.Capture.LoginNotice",
        "Play on this realm is recorded (movement, actions and combat; never chat or account details) to train "
        "its bots to play like people.");

    Recording.SnapshotMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("Animus.Capture.Recording.SnapshotMs",
        100));
    Recording.UnitMs = std::max<uint32>(50, sConfigMgr->GetOption<uint32>("Animus.Capture.Recording.UnitMs", 100));
    Recording.UnitRange = std::clamp(sConfigMgr->GetOption<float>("Animus.Capture.Recording.UnitRange", 80.0f),
        10.0f, 250.0f);
    Recording.MaxUnits = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Animus.Capture.Recording.MaxUnits", 128),
        1, 1024);
    Recording.Packets = sConfigMgr->GetOption<bool>("Animus.Capture.Recording.Packets", true);
    Recording.PacketMaxBytes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(
        "Animus.Capture.Recording.PacketMaxBytes", 1048576), 1024, 1u << 26);
    // The opcodes left out of the packet stream, by number or name (the chat opcodes: their text is the Chat
    // record's, redacted as the rules say; Warden's bytes are nobody's business).
    Recording.PacketSkip.clear();
    std::string const skip = sConfigMgr->GetOption<std::string>("Animus.Capture.Recording.PacketSkip",
        "SMSG_MESSAGECHAT SMSG_GM_MESSAGECHAT CMSG_MESSAGECHAT SMSG_WARDEN_DATA CMSG_WARDEN_DATA");
    for (std::string_view word : Acore::Tokenize(skip, ' ', false))
    {
        if (Optional<uint32> number = Acore::StringTo<uint32>(word))
        {
            Recording.PacketSkip.push_back(uint16(*number));
            continue;
        }
        for (uint32 opcode = 0; opcode < NUM_OPCODE_HANDLERS; ++opcode)
            if (word == GetOpcodeNameForLogging(Opcodes(opcode)))
            {
                Recording.PacketSkip.push_back(uint16(opcode));
                break;
            }
    }
    std::sort(Recording.PacketSkip.begin(), Recording.PacketSkip.end());
    Recording.OthersChat = sConfigMgr->GetOption<bool>("Animus.Capture.Recording.OthersChat", false);
    Recording.MaxMinutes = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("Animus.Capture.Recording.MaxMinutes",
        240), 1, 1440);
    Recording.Notice = sConfigMgr->GetOption<std::string>("Animus.Capture.Recording.Notice",
        "Your play and surroundings -- movement, actions, combat, your group's chat -- are saved to this realm's "
        "recordings until you stop.");
    Recording.GodSecurity = std::min<uint32>(4, sConfigMgr->GetOption<uint32>("Animus.Capture.Recording.GodSecurity",
        2));
}

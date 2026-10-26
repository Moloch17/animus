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

#include "AnimusAddon.h"
#include "AnimusMod.h"
#include "Capture.h"
#include "CharacterPrep.h"
#include "Chat.h"
#include "Player.h"
#include "WorldSession.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include <charconv>
#include <optional>
#include <string>
#include <vector>

namespace
{
    /// Bumped when a reply changes shape; the addon shows what it expects.
    /// 3 (2026-10-25): recordings (record, note; RECORD, NOTE), settings (partyfill; SETTINGS), Prepare (specs,
    /// prepare; SPEC, LEVELS), god.
    /// 4 (2026-10-25, the follow runtime): the v1 companion is gone with its RACE, COMPANION, PET and PETTALENT lines
    /// and the create/summon/rename/reroll/talent/pet/equip/rate requests; followers come in (`follow <n>`,
    /// `dismiss`, `followers`; FOLLOW, FOLLOWER), Life and its LIFE line go, and HELLO tells whether this account may
    /// summon and how many.
    constexpr uint32 PROTOCOL = 4;

    /// The client drops a longer addon message. Replies put the one free-text field last, so it is what a cut
    /// takes.
    constexpr std::size_t MAX_MESSAGE = 255;

    std::vector<std::string_view> Split(std::string_view text, char separator)
    {
        std::vector<std::string_view> words;
        while (true)
        {
            std::size_t const end = text.find(separator);
            words.push_back(text.substr(0, end));
            if (end == std::string_view::npos)
                return words;
            text.remove_prefix(end + 1);
        }
    }

    /// One reply line, as an addon whisper from the player to themselves (what the addon listens for).
    void Send(Player* player, std::string line)
    {
        line.insert(0, std::string(Animus::Addon::PREFIX) + '\t');
        if (line.size() > MAX_MESSAGE)
            line.resize(MAX_MESSAGE);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, line);
        player->SendDirectMessage(&data);
    }

    void SendResult(Player* player, bool ok, std::string const& message)
    {
        Send(player, Acore::StringFormat("{}\t{}", ok ? "OK" : "ERR", message));
    }

    /// A number the addon sent, if it is one.
    template <typename T>
    std::optional<T> Number(std::string_view word)
    {
        T value{};
        auto const [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
        if (error != std::errc() || end != word.data() + word.size())
            return std::nullopt;
        return value;
    }

    /// FOLLOW <count> <max>, then a FOLLOWER <text> line per follower (its name, level, class and spec, model and
    /// episode clock, as `.animus follow` lists them).
    void SendFollowers(Player* player)
    {
        std::vector<std::string> const lines = sAnimusMod->List(player);
        Send(player, Acore::StringFormat("FOLLOW\t{}\t{}", lines.size(), Animus::Follow::FollowParty::MAX_FOLLOWERS));
        for (std::string const& line : lines)
            Send(player, "FOLLOWER\t" + line);
    }

    bool IsGameMaster(Player* player)
    {
        return player->GetSession() && player->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    }

    /// RECORD <on> <id> <elapsed seconds> <notes> <god>: the player's own recording, or `RECORD 0` with the rest
    /// empty.
    void SendRecord(Player* player)
    {
        Animus::Capture::RecordingStatus const status = Animus::Capture::Status(player);
        Send(player, Acore::StringFormat("RECORD\t{}\t{}\t{}\t{}\t{}\t{}", status.On ? 1 : 0, status.Id,
            status.Seconds, status.Notes, status.God ? 1 : 0, status.Available ? 1 : 0));
    }

    /// SETTINGS <party fill mine> <party fill realm> <realm overridden> <config default>.
    void SendSettings(Player* player)
    {
        Animus::AnimusMod::PartyFillStatus const fill = sAnimusMod->PartyFillState(player);
        Send(player, Acore::StringFormat("SETTINGS\t{}\t{}\t{}\t{}", fill.Mine ? 1 : 0, fill.Realm ? 1 : 0,
            fill.Overridden ? 1 : 0, fill.Config ? 1 : 0));
    }

    /// A SPEC <index> <name> line per spec of the player's class, then LEVELS <min> <max>.
    void SendSpecs(Player* player)
    {
        std::vector<std::string> const specs = Animus::Prepare::Specs(player);
        for (std::size_t index = 0; index < specs.size(); ++index)
            Send(player, Acore::StringFormat("SPEC\t{}\t{}", index, specs[index]));
        uint8 minLevel = 1;
        uint8 maxLevel = 80;
        Animus::Prepare::Levels(player, minLevel, maxLevel);
        Send(player, Acore::StringFormat("LEVELS\t{}\t{}", minLevel, maxLevel));
    }

    /// Everything the addon needs to open: HELLO <protocol> <enabled> <stage> <capture available> <is gm>
    /// <prepare allowed> <may summon>, then the followers, the recording and the settings.
    void SendHello(Player* player)
    {
        Send(player, Acore::StringFormat("HELLO\t{}\t{}\t{}\t{}\t{}\t{}\t{}", PROTOCOL,
            sAnimusMod->IsEnabled() ? 1 : 0, sAnimusMod->CurrentStage(),
            Animus::Capture::Status(player).Available ? 1 : 0, IsGameMaster(player) ? 1 : 0,
            Animus::Prepare::Allowed(player) ? 1 : 0, sAnimusMod->MaySummon(player) ? 1 : 0));
        SendFollowers(player);
        SendRecord(player);
        SendSettings(player);
    }

    /// The addon's free text, last word of a request: tabs cannot reach here (the split), so the rest is taken
    /// as it is, joined back with tabs should the text have held any.
    std::string Rest(std::vector<std::string_view> const& words, std::size_t from)
    {
        std::string text;
        for (std::size_t i = from; i < words.size(); ++i)
            text += (i > from ? " " : "") + std::string(words[i]);
        return text;
    }
}

bool Animus::Addon::Handle(Player* player, std::string_view msg)
{
    if (msg.size() <= PREFIX.size() || msg.substr(0, PREFIX.size()) != PREFIX || msg[PREFIX.size()] != '\t')
        return false;

    std::vector<std::string_view> const words = Split(msg.substr(PREFIX.size() + 1), '\t');
    std::string_view const request = words.front();
    std::string message;

    if (request == "hello")
        SendHello(player);
    else if (request == "followers")
        SendFollowers(player);
    else if (request == "follow")
    {
        // follow <count>: followers into the player's party (Animus.Follow.Security).
        std::optional<uint32> const count = words.size() == 2 ? Number<uint32>(words[1]) : std::nullopt;
        if (!count)
            SendResult(player, false, "follow <1-4>.");
        else if (!sAnimusMod->MaySummon(player))
            SendResult(player, false, "Followers are not for your account on this realm.");
        else
        {
            std::vector<std::string> warnings;
            bool const ok = sAnimusMod->Summon(player, *count, message, warnings);
            for (std::string const& warning : warnings)
                SendResult(player, true, warning);
            SendResult(player, ok, message);
            SendFollowers(player);
        }
    }
    else if (request == "dismiss")
    {
        SendResult(player, sAnimusMod->Dismiss(player, message), message);
        SendFollowers(player);
    }
    else if (request == "record")
    {
        // record | record on | record off <keep|discard> [title]
        if (words.size() == 1)
            SendRecord(player);
        else if (words[1] == "on")
        {
            SendResult(player, Animus::Capture::StartRecording(player, message), message);
            SendRecord(player);
        }
        else if (words[1] == "off")
        {
            std::string_view const verdict = words.size() > 2 ? words[2] : std::string_view();
            if (!verdict.empty() && verdict != "keep" && verdict != "discard")
                SendResult(player, false, "A recording is kept or discarded.");
            else
            {
                SendResult(player, Animus::Capture::StopRecording(player, Animus::Capture::StopReason::Stop, verdict,
                    Rest(words, 3), message), message);
                SendRecord(player);
            }
        }
        else
            SendResult(player, false, "record on, or record off keep|discard [title].");
    }
    else if (request == "note")
    {
        // note <client ms> <text...>
        std::optional<uint32> const clientMs = words.size() >= 3 ? Number<uint32>(words[1]) : std::nullopt;
        if (!clientMs)
            SendResult(player, false, "A note is its clock and its text.");
        else
        {
            uint32 seq = 0;
            if (Animus::Capture::Note(player, Rest(words, 2), *clientMs, seq, message))
                Send(player, Acore::StringFormat("NOTE\t{}\t{}", seq, Animus::Capture::Status(player).Notes));
            else
                SendResult(player, false, message);
        }
    }
    else if (request == "settings")
        SendSettings(player);
    else if (request == "partyfill")
    {
        // partyfill on|off (mine) | partyfill realm on|off (a game master's)
        if (words.size() == 2 && (words[1] == "on" || words[1] == "off"))
        {
            SendResult(player, sAnimusMod->SetPartyFill(player, words[1] == "on", message), message);
            SendSettings(player);
        }
        else if (words.size() == 3 && words[1] == "realm" && (words[2] == "on" || words[2] == "off"))
        {
            if (!IsGameMaster(player))
                SendResult(player, false, "The realm's party fill is a game master's to change.");
            else
            {
                SendResult(player, sAnimusMod->PartyFillRealm(words[2] == "on", message), message);
                SendSettings(player);
            }
        }
        else
            SendResult(player, false, "partyfill on|off, or partyfill realm on|off.");
    }
    else if (request == "specs")
        SendSpecs(player);
    else if (request == "prepare")
    {
        // prepare <level> <spec index>
        std::optional<uint8> const level = words.size() == 3 ? Number<uint8>(words[1]) : std::nullopt;
        std::optional<uint8> const spec = words.size() == 3 ? Number<uint8>(words[2]) : std::nullopt;
        if (!level || !spec)
            SendResult(player, false, "Prepare takes a level and a spec.");
        else
            SendResult(player, Animus::Prepare::Prepare(player, *level, *spec, message), message);
    }
    else if (request == "god")
    {
        if (words.size() != 2 || (words[1] != "on" && words[1] != "off"))
            SendResult(player, false, "god on|off.");
        else if (!player->GetSession() || uint32(player->GetSession()->GetSecurity())
            < sAnimusMod->Config().PrepareSecurity)
            SendResult(player, false, "God mode is not for your account on this realm.");
        else
        {
            SendResult(player, Animus::Capture::God(player, words[1] == "on", message), message);
            SendRecord(player);
        }
    }
    else
        SendResult(player, false, Acore::StringFormat("Unknown request {}.", request));

    return true;
}

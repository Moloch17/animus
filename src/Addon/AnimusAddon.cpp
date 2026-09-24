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
#include "Chat.h"
#include "Player.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include <string>
#include <vector>

namespace
{
    /// Bumped when a reply changes shape; the addon shows what it expects.
    constexpr uint32 PROTOCOL = 1;

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

    std::string Join(std::vector<std::string> const& words, char separator)
    {
        std::string joined;
        for (std::string const& word : words)
            joined += (joined.empty() ? "" : std::string(1, separator)) + word;
        return joined;
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

    /// PARTY <count> <max>, then a MEMBER line per companion.
    void SendParty(Player* player)
    {
        std::vector<Animus::CompanionParty::Summary> const companions = sAnimusMod->Companions(player);
        Send(player, Acore::StringFormat("PARTY\t{}\t{}", companions.size(), Animus::CompanionParty::MaxSize()));
        for (Animus::CompanionParty::Summary const& companion : companions)
            Send(player, Acore::StringFormat("MEMBER\t{}\t{}\t{}\t{}\t{}\t{}\t{}", companion.Name, companion.Class,
                companion.Spec, companion.Level, companion.Parked ? 1 : 0, companion.ModelName, companion.Model));
    }

    /// Everything the addon needs to open: HELLO <protocol> <enabled> <stage>, a RACE line per race of the
    /// player's faction with the classes it can be, WANTS, then the party.
    void SendHello(Player* player)
    {
        Send(player, Acore::StringFormat("HELLO\t{}\t{}\t{}", PROTOCOL, sAnimusMod->IsEnabled() ? 1 : 0,
            sAnimusMod->CurrentStage()));
        for (Animus::AnimusMod::RaceChoice const& race : Animus::AnimusMod::RaceChoices(player))
            Send(player, Acore::StringFormat("RACE\t{}\t{}", race.Race, Join(race.Classes, ',')));
        Send(player, Acore::StringFormat("WANTS\t{}", Join(Animus::AnimusMod::WantChoices(), ',')));
        SendParty(player);
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
    else if (request == "list")
        SendParty(player);
    else if (request == "summon")
    {
        if (words.size() != 4)
            SendResult(player, false, "A summon names a race, a class and what to ask for.");
        else
        {
            SendResult(player, sAnimusMod->Summon(player, words[1], words[2], words[3], message), message);
            SendParty(player);
        }
    }
    else if (request == "dismiss")
    {
        SendResult(player, sAnimusMod->Dismiss(player, message), message);
        SendParty(player);
    }
    else
        SendResult(player, false, Acore::StringFormat("Unknown request {}.", request));

    return true;
}

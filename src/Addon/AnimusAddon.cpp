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
#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

namespace
{
    /// Bumped when a reply changes shape; the addon shows what it expects.
    /// 3 (2026-10-25): recordings (record, note; RECORD, NOTE), settings (partyfill; SETTINGS), Prepare (specs,
    /// prepare; SPEC, LEVELS), god; HELLO gained capture available, isGM and prepare allowed. LIFE (the life
    /// switches, in HELLO and after `life`) was added within 3: an addon that does not know it ignores the line.
    constexpr uint32 PROTOCOL = 3;

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

    /// COMPANION <exists> <name> <race> <class> <level> <spec> <out> <loading> <parked> <model name> <model state>:
    /// the owner's one companion, or `COMPANION 0` when they have none.
    void SendCompanion(Player* player)
    {
        Animus::AnimusMod::Companion const c = sAnimusMod->Describe(player);
        if (!c.Exists)
        {
            Send(player, "COMPANION\t0");
            return;
        }

        Send(player, Acore::StringFormat("COMPANION\t1\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", c.Name, c.Race,
            c.Class, c.Level, c.Spec, c.Out ? 1 : 0, c.Loading ? 1 : 0, c.Parked ? 1 : 0, c.ModelName, c.Model));
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

    /// PET <companion> <pet name> <level> <free points> <talent count>, then a PETTALENT line per talent of its
    /// tree: talent id, row, column, max rank, rank, the ranks' spells (comma-separated), prerequisite talent id
    /// (0 for none) and the rank it needs. ERR for a companion without a hunter pet out.
    void SendPet(Player* player, std::string_view name)
    {
        Animus::CompanionParty::PetView view;
        std::string message;
        if (!sAnimusMod->Pet(player, name, view, message))
        {
            SendResult(player, false, message);
            return;
        }

        Send(player, Acore::StringFormat("PET\t{}\t{}\t{}\t{}\t{}", name, view.PetName, view.Level, view.FreePoints,
            view.Talents.size()));
        for (Animus::CompanionTalents::PetTalent const& talent : view.Talents)
        {
            std::string spells;
            for (uint32 spell : talent.Spells)
                spells += (spells.empty() ? "" : ",") + std::to_string(spell);
            Send(player, Acore::StringFormat("PETTALENT\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", name, talent.TalentId,
                talent.Row, talent.Col, talent.MaxRank, talent.Rank, spells, talent.DependsOn, talent.DependsOnRank));
        }
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

    /// LIFE <all> <quests> <auction> <mail> <taxi> <crafting>: the realm's life switches (any player may read them).
    void SendLife(Player* player)
    {
        std::array<bool, 6> const flags = sAnimusMod->LifeFlags();
        Send(player, Acore::StringFormat("LIFE\t{}\t{}\t{}\t{}\t{}\t{}", flags[0] ? 1 : 0, flags[1] ? 1 : 0,
            flags[2] ? 1 : 0, flags[3] ? 1 : 0, flags[4] ? 1 : 0, flags[5] ? 1 : 0));
    }

    /// Everything the addon needs to open: HELLO <protocol> <enabled> <stage> <capture available> <is gm>
    /// <prepare allowed>, a RACE line per race of the player's faction with the classes it can be, the companion,
    /// the recording and the settings.
    void SendHello(Player* player)
    {
        Send(player, Acore::StringFormat("HELLO\t{}\t{}\t{}\t{}\t{}\t{}", PROTOCOL, sAnimusMod->IsEnabled() ? 1 : 0,
            sAnimusMod->CurrentStage(), Animus::Capture::Status(player).Available ? 1 : 0, IsGameMaster(player) ? 1 : 0,
            Animus::Prepare::Allowed(player) ? 1 : 0));
        for (Animus::AnimusMod::RaceChoice const& race : Animus::AnimusMod::RaceChoices(player))
            Send(player, Acore::StringFormat("RACE\t{}\t{}", race.Race, Join(race.Classes, ',')));
        SendCompanion(player);
        SendRecord(player);
        SendSettings(player);
        SendLife(player);
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

void Animus::Addon::Push(Player* player, std::string const& message)
{
    if (!message.empty())
        SendResult(player, true, message);
    SendCompanion(player);
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
        SendCompanion(player);
    else if (request == "create")
    {
        if (words.size() != 4)
            SendResult(player, false, "Creating a companion needs a name, a race and a class.");
        else
        {
            SendResult(player, sAnimusMod->Create(player, std::string(words[1]), words[2], words[3], message),
                message);
            SendCompanion(player);
        }
    }
    else if (request == "life")
    {
        // The realm's life switches: a game master's to flip (any player may read them). LIFE either way.
        if (words.size() == 1)
            SendLife(player);
        else if (words.size() != 3)
            SendResult(player, false, "A life switch names a feature and on or off.");
        else if (!IsGameMaster(player))
            SendResult(player, false, "The realm's life switches are a game master's to change.");
        else
        {
            SendResult(player, sAnimusMod->LifeToggle(words[1], words[2], message), message);
            SendLife(player);
        }
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
    else if (request == "summon")
    {
        SendResult(player, sAnimusMod->Summon(player, message), message);
        SendCompanion(player);
    }
    else if (request == "dismiss")
    {
        SendResult(player, sAnimusMod->Dismiss(player, message), message);
        SendCompanion(player);
    }
    else if (request == "rename")
    {
        if (words.size() != 2)
            SendResult(player, false, "A rename needs the new name.");
        else
        {
            SendResult(player, sAnimusMod->Rename(player, std::string(words[1]), message), message);
            SendCompanion(player);
        }
    }
    else if (request == "reroll")
    {
        if (words.size() != 3)
            SendResult(player, false, "A new race and class need both words.");
        else
        {
            SendResult(player, sAnimusMod->Reroll(player, words[1], words[2], message), message);
            SendCompanion(player);
        }
    }
    else if (request == "talent" || request == "pettalent")
    {
        // talent <companion> learn|unlearn <talent id>, and the same of its pet
        std::optional<uint32> const talentId = words.size() == 4 ? Number<uint32>(words[3]) : std::nullopt;
        if (!talentId || (words[2] != "learn" && words[2] != "unlearn"))
            SendResult(player, false, "A talent edit names a companion, learn or unlearn, and a talent id.");
        else if (request == "talent")
            SendResult(player, sAnimusMod->Talent(player, words[1], *talentId, words[2] == "learn", message), message);
        else if (sAnimusMod->PetTalent(player, words[1], *talentId, words[2] == "learn", message))
            SendPet(player, words[1]);
        else
            SendResult(player, false, message);
    }
    else if (request == "pet")
    {
        if (words.size() != 2)
            SendResult(player, false, "A pet request names a companion.");
        else
            SendPet(player, words[1]);
    }
    else if (request == "equip")
    {
        // equip <companion> <bag> <slot> <inventory slot>, in the client's numbering
        std::optional<uint8> const bag = words.size() == 5 ? Number<uint8>(words[2]) : std::nullopt;
        std::optional<uint8> const slot = words.size() == 5 ? Number<uint8>(words[3]) : std::nullopt;
        std::optional<uint8> const equipSlot = words.size() == 5 ? Number<uint8>(words[4]) : std::nullopt;
        if (!bag || !slot || !equipSlot)
            SendResult(player, false, "An equip names a companion, a bag, a slot and an equipment slot.");
        else
            SendResult(player, sAnimusMod->Equip(player, words[1], *bag, *slot, *equipSlot, message), message);
    }
    else if (request == "rate")
    {
        // rate +|- [movement|combat|healing|tanking|stuck|other]
        if (words.size() != 2 && words.size() != 3)
            SendResult(player, false, "A rating is + or -, then optionally why.");
        else
            SendResult(player, sAnimusMod->Rate(player, words[1], words.size() == 3 ? words[2] : "", message),
                message);
    }
    else
        SendResult(player, false, Acore::StringFormat("Unknown request {}.", request));

    return true;
}

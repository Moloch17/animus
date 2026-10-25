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

#ifndef ANIMUS_CAPTURE_H
#define ANIMUS_CAPTURE_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <string_view>

class Player;

namespace Animus::Capture
{
    /// What the companion code tells the capture (world thread). Each does nothing while the companion stream is
    /// off. The rest of the capture is hooks (CaptureHooks.cpp).

    /// A companion's decision (CompanionParty::Decide): the model it plays, a hash of its observation, the action
    /// chosen (-1 none) and the goals it holds (NO_GOAL = -1).
    void CompanionDecision(Player* bot, ObjectGuid owner, std::string_view model, float const* obs,
        std::size_t obsCount, int32 action, int32 goal, int32 goal2);
    /// The model a companion plays and its move block's revision, for its MoverState records (world thread, while
    /// maps are idle). Its motion itself is captured as a player's is: its player controller reports through its
    /// session's movement handlers, which the move stream records as client packets of source 2.
    void CompanionModel(Player* bot, std::string_view model, uint8 moveRevision);
    /// A companion's client is about to hand a packet of `opcode` to its session's handler (CompanionClient's
    /// RealmLink): counted as a player's are when they reach theirs (MoveTally `sent`).
    void MovementSent(Player* bot, uint16 opcode);
    /// Whether a packet of `opcode` goes to a handler that records it in the move stream when it keeps it.
    [[nodiscard]] bool ReachesMoveHook(uint16 opcode);

    enum class Command : uint8
    {
        Summon = 1,
        Dismiss = 2,
        Follow = 3,
        Assist = 4,
        Guard = 5,
        Stay = 6,
        Other = 7,
    };

    /// `arg` of an Other command: what the owner changed.
    enum class OtherCommand : uint32
    {
        Create = 1,
        Rename = 2,
        Reroll = 3,
        Talent = 4,
        PetTalent = 5,
        Equip = 6,
    };

    void CompanionCommand(ObjectGuid owner, ObjectGuid companion, Command command, uint32 arg = 0);

    /// The owner's verdict on its companion (`.animus rate`, the addon's rate request): `sign` "+" / "-" (or up,
    /// down, good, bad), `reason` one of movement, combat, healing, tanking, stuck, other, or empty. False with
    /// `message` set when the words are not understood.
    bool CompanionRating(ObjectGuid owner, ObjectGuid companion, std::string_view sign, std::string_view reason,
        std::string& message);

    // ---- manual recordings (format 4): a player's own recording of their play -------------------------------------
    // World thread only (the addon's requests and the chat commands arrive there, while no map updates).

    /// Why a recording ended (RecordingEnd `reason`).
    enum class StopReason : uint8
    {
        Stop = 0,
        Logout = 1,
        Disconnect = 2,
        Shutdown = 3,
        Disk = 4,
        MaxMinutes = 5,
    };

    /// Start recording `player`'s own play: refused (false, `message` says why) when capture is not available on
    /// this realm, the player is a bot, one is already running, or no slot is free. On success `message` is the
    /// line to show the player.
    bool StartRecording(Player* player, std::string& message);
    /// Stop it, with the player's verdict ("keep", "discard" or empty) and an optional title. False when none runs.
    bool StopRecording(Player* player, StopReason reason, std::string_view verdict, std::string_view title,
        std::string& message);
    /// A free-text note into the running recording, with where the player stands and what they target; `clientMs`
    /// the addon's clock when it was typed (0 from a chat command). `seq` gets the note's number. False with
    /// `message` when nothing is recording or the text is empty.
    bool Note(Player* player, std::string_view text, uint32 clientMs, uint32& seq, std::string& message);

    struct RecordingStatus
    {
        bool Available = false;         // capture runs on this realm
        bool On = false;
        std::string Id;
        uint32 Seconds = 0;
        uint32 Notes = 0;
        bool God = false;
    };
    [[nodiscard]] RecordingStatus Status(Player* player);

    /// Invulnerability for a recording player (CHEAT_GOD): on or off, GM-gated by the caller. False with `message`
    /// when the player is not recording. Turned off by itself when the recording stops, if it turned it on.
    bool God(Player* player, bool on, std::string& message);

    /// The recording slot of a player (0 none): what the companion hooks write their records to.
    [[nodiscard]] uint16 RecordingSlotOf(Player* player);

    /// Once a world tick (AnimusMod::OnUpdate): the recordings the writer asked stopped (the disk) and the ones
    /// past Recording.MaxMinutes are stopped, their players told.
    void UpdateRecordings();
}

void AddSC_animus_capture();

#endif

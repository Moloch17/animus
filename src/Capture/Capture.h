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
    /// Where the companion is now, as a move record of source 1 (once per decision, so its motion compares with
    /// a player's).
    void CompanionSample(Player* bot);

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
}

void AddSC_animus_capture();

#endif

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

#ifndef ANIMUS_COMPANION_TALENTS_H
#define ANIMUS_COMPANION_TALENTS_H

#include "Define.h"
#include <map>
#include <string>
#include <vector>

class Pet;
class Player;

namespace Animus::CompanionTalents
{
    /// A character's talents as the owner may edit them from the inspect window: one rank at a time, under the
    /// client's own rules (points in the rows above, prerequisites), so what the window shows is what the core
    /// would have let a player do. Refusals set `message`.
    bool Learn(Player* bot, uint32 talentId, std::string& message);
    bool Unlearn(Player* bot, uint32 talentId, std::string& message);

    /// The same for a hunter pet's talents (the other pet classes have none).
    bool LearnPet(Player* bot, Pet* pet, uint32 talentId, std::string& message);
    bool UnlearnPet(Player* bot, Pet* pet, uint32 talentId, std::string& message);

    /// Talent id -> rank (1-based) of every talent a character knows, or a pet knows.
    using Snapshot = std::map<uint32, uint8>;
    [[nodiscard]] Snapshot Take(Player const* bot);
    [[nodiscard]] Snapshot TakePet(Pet const* pet);
    /// Learn a snapshot again on a character (or pet) whose talents were reset, prerequisites first. Returns the
    /// ranks that could not be learned again (none when the snapshot fits the level).
    uint32 Replay(Player* bot, Snapshot const& talents);
    uint32 ReplayPet(Player* bot, Pet* pet, Snapshot const& talents);

    /// One talent of a pet's tree, for the addon's pet tab (the client cannot read another player's pet).
    struct PetTalent
    {
        uint32 TalentId = 0;
        uint32 Row = 0;
        uint32 Col = 0;
        uint8 MaxRank = 0;
        uint8 Rank = 0;                         // ranks the pet has
        std::vector<uint32> Spells;             // one per rank, for names, icons and tooltips
        uint32 DependsOn = 0;                   // talent id, 0 for none
        uint8 DependsOnRank = 0;                // ranks the prerequisite needs
    };

    /// Every talent of the pet's tree (empty for a pet without one), by row then column.
    [[nodiscard]] std::vector<PetTalent> PetTree(Pet const* pet);
}

#endif

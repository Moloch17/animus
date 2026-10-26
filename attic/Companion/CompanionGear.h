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

#ifndef ANIMUS_COMPANION_GEAR_H
#define ANIMUS_COMPANION_GEAR_H

#include "Define.h"
#include <string>
#include <vector>

class Item;
class Player;

namespace Animus::CompanionGear
{
    /// The owner drags an item from their bags onto a slot of the companion's character pane: the item goes on the
    /// companion, what it wore there goes into the owner's bags (the freed slot first). `bag` and `slot` are the
    /// client's numbering (bag 0 the backpack, slots from 1), `equipSlot` the client's inventory slot (1 to 19).
    /// Refusals set `message` and, when the core refused, send the owner the client's own red error too.
    /// Returns the equipment slot filled (EQUIPMENT_SLOT_*) in `equipped` on success.
    bool Give(Player* owner, Player* bot, uint8 bag, uint8 slot, uint8 equipSlot, uint8& equipped,
        std::string& message);

    /// Take the owner's items off a companion about to be built again (GearBuilder::Equip destroys everything it
    /// carries), then put them back on. Items that no longer fit go to the owner, else are lost.
    std::vector<Item*> Detach(Player* bot, std::vector<uint8> const& slots);
    void Reattach(Player* bot, Player* owner, std::vector<Item*> const& items, std::vector<uint8>& slots);
}

#endif

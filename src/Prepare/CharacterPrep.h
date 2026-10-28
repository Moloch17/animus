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

#ifndef ANIMUS_CHARACTER_PREP_H
#define ANIMUS_CHARACTER_PREP_H

#include "Define.h"
#include <string>
#include <vector>

class Player;

namespace Animus::Prepare
{
    /// **Prepare** (the user, 2026-10-25): a character set to the level the player typed in, geared, talented and
    /// taught for it -- so an annotation recording of a dungeon starts in a minute, not after an afternoon of
    /// levelling. Built the way the module builds a companion (SeatCharacter::Configure: the spec's standard
    /// talents, the class trainers' spells, generated gear, reagents and ammo). World thread.
    ///
    /// Everything the character wore or carried is mailed back to it first (the gear builder destroys the
    /// equipment, the backpack, the bags and their contents), so nothing is lost; its keyring, bank, money and
    /// professions are not touched. Gated by Animus.Prepare.Security (the account security it needs, game master by
    /// default) and refused in a fight, dead, on a taxi, in a battleground, while teleporting, or while recording.

    /// Whether `player` may use it at all (Animus.Prepare.Security).
    [[nodiscard]] bool Allowed(Player const* player);
    /// The specs the player's class has, in index order (the words the addon's dropdown shows).
    [[nodiscard]] std::vector<std::string> Specs(Player const* player);
    /// The levels Prepare takes for the player's class: its minimum (a death knight's 55) and the realm's maximum.
    void Levels(Player const* player, uint8& minLevel, uint8& maxLevel);
    /// Prepare `player` at `level` for spec index `spec`. False with `message` when refused; on success `message`
    /// says what was done and where the old things went. `by` is who asked (the player itself, or a game master
    /// preparing another character by name, 2026-10-28): Animus.Prepare.Security is checked on them.
    bool Prepare(Player* player, Player const* by, uint8 level, uint8 spec, std::string& message);
    inline bool Prepare(Player* player, uint8 level, uint8 spec, std::string& message)
    {
        return Prepare(player, player, level, spec, message);
    }
}

#endif

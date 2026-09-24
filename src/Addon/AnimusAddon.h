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

#ifndef ANIMUS_ADDON_H
#define ANIMUS_ADDON_H

#include <string_view>

class Player;

namespace Animus::Addon
{
    /// The addon message prefix the Animus addon whispers with, and the prefix of every reply.
    constexpr std::string_view PREFIX = "Animus";

    /// How the Animus addon (animus_addon/Animus) talks to the module: addon whispers a player sends to themselves,
    /// tab-separated, the first word a request (`hello`, `list`, `create <name> <race> <class>`, `summon`, `dismiss`,
    /// `rename <name>`, `reroll <race> <class>`, `talent <name> learn|unlearn <id>`, `pettalent ...`, `pet <name>`,
    /// `equip <name> <bag> <slot> <inv slot>`).
    /// Replies are addon whispers from the player to themselves, tab-separated, the first word in capitals (HELLO,
    /// RACE, COMPANION, PET, PETTALENT, OK, ERR), so the addon can tell an answer from its own request
    /// echoed back by a realm without the module. animus_addon/Animus/README.md describes every message.
    ///
    /// Handle `msg` (prefix and all) as one of those; false when it is not an Animus message, so the whisper goes
    /// where it was going. Player security: this is how players use the module, where the `.animus` commands
    /// stay game master ones.
    bool Handle(Player* player, std::string_view msg);

    /// Tell the addon about the companion unasked: OK/ERR `message` when there is one, then the COMPANION line.
    /// For what finishes later than its request (a summon, whose character loads on the database thread).
    void Push(Player* player, std::string const& message);
}

#endif

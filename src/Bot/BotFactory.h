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

#ifndef ANIMUS_BOT_FACTORY_H
#define ANIMUS_BOT_FACTORY_H

#include "Define.h"
#include <string>

class Player;

namespace Animus::BotFactory
{
    struct BotSpec
    {
        uint8 Race = 0;
        uint8 Class = 0;
        uint8 Gender = 0;
        uint8 Level = 1;
    };

    /// Build a socketless session and a freshly created (never saved) character named Animus<N>.
    ///
    /// The session is deliberately NOT registered with WorldSessionMgr: a socketless session is
    /// deleted there on the next update, logging its player out with a save. The bot is driven by
    /// Map::Update (MapSessionFilter + Player::Update) once it is on a map. Autosave is disabled.
    /// Returns nullptr on failure. The player is not in the world yet; pass it to PlaceNear, or to
    /// Discard if it will not be placed.
    Player* Create(BotSpec const& spec);

    /// Put a Create()d bot into the world beside `owner`, on the owner's map and phase. The owner
    /// must be on a non-instanced map. On failure the bot is discarded and false returned.
    bool PlaceNear(Player* bot, Player* owner);

    /// Log an in-world bot out without saving and delete it and its session.
    void Destroy(Player* bot);

    /// Delete a bot that never entered the world.
    void Discard(Player* bot);
}

#endif

/*
 * This file is part of the Animus Forge project, based on AzerothCore.
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

#ifndef ANIMUS_LIB_BOT_FACTORY_H
#define ANIMUS_LIB_BOT_FACTORY_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>

class Map;
class Player;
class WorldSession;
struct Position;

/*
 * Bots are players without a client or a character row, built with public core APIs only, so the same code runs on the
 * forge core and on a stock AzerothCore.
 *
 * Mirrors the create path (CharacterHandler: new Player -> MotionMaster::Initialize -> Create) and the in-world half
 * of the login path (HandlePlayerLoginFromDB: SetPlayer -> SetMover -> ObjectAccessor::AddObject ->
 * Map::AddPlayerToMap), skipping every step that reads or writes the character database and every step whose only
 * effect is a packet to the bot's own client. Real players can see bots (the stage viewer, companions), so a bot also
 * gets an empty social list (whispers and invites read it unchecked) and a character cache entry (name queries answer
 * from it, else clients show "Unknown").
 */
namespace Animus::BotFactory
{
    struct BotSpec
    {
        /// Unique among the server's players; real character names cannot contain digits, so a name with one never
        /// collides with a player's.
        std::string Name;
        uint8 Race = 0;
        uint8 Class = 0;
        uint8 Gender = 0;
        uint8 Level = 1;
        uint32 AccountId = 0;

        /// Player GUID counter to create the bot with; 0 = a new one. A scenario that rebuilds bots
        /// reuses a fixed set: the core keeps some per-GUID state for the life of the server (e.g.
        /// InstanceSaveMgr's bind storage), so a new GUID per rebuild would grow memory forever.
        /// The previous bot with the GUID must be destroyed first.
        ObjectGuid::LowType GuidLow = 0;
    };

    /// Build a socketless session and a freshly created (never saved) character.
    ///
    /// The session is deliberately NOT registered with WorldSessionMgr: a socketless session is
    /// deleted there on the next update, logging its player out with a save. The bot is driven by
    /// Map::Update (MapSessionFilter + Player::Update) once it is on a map. Autosave is disabled.
    /// Returns nullptr on failure. The player is not in the world yet.
    ///
    /// `session` reuses the socketless session of a bot destroyed with keepSession, so a scenario that
    /// rebuilds its bots every episode does not create and delete a session each time.
    Player* Create(BotSpec const& spec, WorldSession* session = nullptr);

    /// Put a Create()d bot into its own new instance of `mapId` at `pos`, the server-side
    /// equivalent of logging in there. Returns the map, or nullptr on failure.
    Map* PlaceInNewInstance(Player* bot, uint32 mapId, Position const& pos);

    /// Put a Create()d bot on continent `mapId` (not instanceable, shared) at `pos`. Returns the map, or nullptr on
    /// failure.
    Map* PlaceOnContinent(Player* bot, uint32 mapId, Position const& pos);

    /// Put a Create()d bot into an existing map at `pos`. Returns false on failure.
    bool PlaceInMap(Player* bot, Map* map, Position const& pos);

    /// Whether `owner` rides something no bot can ride with it: a flight path (a taxi, or a scripted quest flight) or a
    /// vehicle (a quest's bombing run, a siege engine, a seat on someone else's mount). Boats, zeppelins and elevators
    /// are transports, not this.
    [[nodiscard]] bool IsAway(Player* owner);

    /// Whether a bot can be put beside `owner` now: the owner is in the world, not between maps, not away (IsAway),
    /// and not in a battleground or arena (those only take the players the battleground system queued).
    [[nodiscard]] bool CanJoin(Player* owner);

    /// Put a Create()d bot into the world beside `owner`: on the owner's map (open world, dungeon or raid instance)
    /// and phase, with the owner's dungeon and raid difficulty, and on the owner's transport if it rides one. On failure
    /// (CanJoin, the map refusing the bot) the bot is discarded (DestroyUnplaced) and false returned.
    bool PlaceNear(Player* bot, Player* owner);

    /// Teleport a placed or parked (Park) bot beside `owner` as PlaceNear places it, into the owner's instance too,
    /// and complete the teleport as the client's acknowledgement would (see CompleteTeleport). The bot does not need to
    /// meet the map's entry requirements (level, attunement, keys); false if CanJoin fails, the owner's instance
    /// refuses it (full, an encounter in progress) or the teleport fails.
    bool TeleportNear(Player* bot, Player* owner);

    /// Take a placed bot out of the world without destroying it, as a far teleport whose loading screen never ends:
    /// it leaves combat, its transport and its map, and its pet is dismissed until it returns. Bring it back with
    /// TeleportNear (into the owner's exact map and instance), and never CompleteTeleport a parked bot. Destroy still works on it. False if it is not in the world.
    bool Park(Player* bot);

    /// Finish a teleport a bot started without TeleportNear (a transport changing maps, a summoning spell, a
    /// scripted teleport) as the bot's client would acknowledge it: the worldport for another map, MSG_MOVE_TELEPORT_ACK
    /// on the same map. Does nothing while no teleport is pending.
    /// Call it on the world thread, outside map updates.
    void CompleteTeleport(Player* bot);

    /// Teleport a placed bot to `pos` on its own map (an instance too), acknowledging for the client it has not.
    bool TeleportWithinMap(Player* bot, Position const& pos);

    /// Log the bot out without saving and drop its instance bind. Deletes the session unless
    /// keepSession, in which case it is returned for the next Create.
    WorldSession* Destroy(Player* bot, bool keepSession = false);

    /// Delete a Create()d bot that was never placed on a map (and its session).
    void DestroyUnplaced(Player* bot);
}

#endif

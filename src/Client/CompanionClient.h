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

#ifndef ANIMUS_COMPANION_CLIENT_H
#define ANIMUS_COMPANION_CLIENT_H

#include "Client.h"
#include "ClientOrders.h"
#include "Define.h"
#include "MoveControls.h"
#include "ObjectGuid.h"
#include <array>
#include <memory>
#include <vector>

class Player;
class WorldPacket;
class WorldSession;

/// **A companion's game client** (player-controller realm port, R1 + R3). A companion is a Player on a WorldSession
/// with no socket: nothing reads what the server sends it and nothing arrives from it. This is its client:
///
/// - **The controller** (Movement::Client, shared with the forge): every map tick it moves the companion's true body
///   under the keys and mouse the policy holds (Controls, written by the move block at each decision) and decides,
///   at the 3.3.5a client's cadence, what to tell the server -- a change opcode at once, SET_FACING on the
///   mouse-look's 0.1 rad rule, a heartbeat 500 ms after the last packet while moving. It yields to what the server
///   imposes (a root, a stun, a fear, a spline, a teleport) and answers the server's movement orders.
/// - **The server link** (RealmLink, the realm's half of the C4 interface): each report becomes a real movement packet
///   (WorldSession::WriteMovementInfo) handed to the session's own handler -- HandleMovementOpcodes for movement,
///   the stock ack handlers for acks -- so every stock rule applies (VerifyMovementInfo, HandleFall, SetInWater,
///   relocation, the relay to watching players). What the server kept is checked after each one (the post-check).
/// - **The inbox**: what the server sends a client about its own movement (Movement/ClientOrders.h) is taken off the
///   session as it is sent (ServerScript::OnPacketSent, which fires for sessions without a socket), queued under a
///   mutex, and handled in order on the companion's tick.
/// - **A clock** of its own, one second ahead of the server's, and time-sync answers, so movement times are
///   synchronised as a client's are.
///
/// Threads: Push from any thread (the inbox is its only entry). Tick on the companion's map thread
/// (PlayerScript::OnPlayerAfterUpdate), where a client's movement opcodes are handled. Controls and the body are read
/// and written by the party's decision on the world thread, which never runs while maps update.
namespace Animus::Client
{
    /// Why the server did not keep a report as sent (RealmLink's post-check).
    enum class Refusal : uint8
    {
        None,
        Teleporting,        // between a teleport and its ack: the handler ignores movement
        NotMover,           // charmed or on a vehicle: the session moves another unit
        InvalidPosition,    // not a valid map coordinate: a bug in the controller, logged loudly
        Spline,             // a core spline (charge, jump, fear, a taxi) is running
        DisableMove,        // UNIT_FLAG_DISABLE_MOVE: moving packets are ignored
        Rooted,             // MOVEMENTFLAG_ROOT held on the server (never, for a player, on a stock core)
        Other,              // kept differently for a reason not listed (an anticheat hook, an ack's pre-check)
        Count
    };

    [[nodiscard]] char const* RefusalName(Refusal refusal);

    class CompanionClient
    {
    public:
        explicit CompanionClient(ObjectGuid bot);

        [[nodiscard]] ObjectGuid Bot() const { return _bot; }
        [[nodiscard]] Clock const& ClientClock() const { return _clock; }

        /// The keys and mouse the policy holds, and the body they move: what the move block presses and the
        /// observations read (SeatView::Controls, SeatView::Body).
        Curriculum::MoveControls::SeatControls Controls;
        [[nodiscard]] Movement::Client& Mover() { return _mover; }
        [[nodiscard]] Movement::Client const& Mover() const { return _mover; }

        /// An order the server just sent the session, as the client receives it. Any thread.
        void Push(uint16 opcode, uint8 const* data, std::size_t size);

        /// Ask the server for a time sync now (its own ResetTimeSync + SendTimeSync), as it does for a player that
        /// entered a map: a companion placed beside its owner never got one. Answered on the next Tick.
        void RequestTimeSync(Player* bot);

        /// One map tick: answer the orders waiting (time syncs here, the rest through the controller), then step the
        /// body under the held controls and report as a client would. Starts the controller from where the server
        /// has the companion the first time, and again after anything put it somewhere else.
        void Tick(Player* bot, uint32 diff);

        /// Reports kept, and refused by cause, since the client was made (for the status line).
        [[nodiscard]] uint64 Applied() const { return _applied; }
        [[nodiscard]] uint64 Refused(Refusal cause) const { return _refused[std::size_t(cause)]; }

    private:
        friend class RealmLink;

        void AnswerTimeSync(Player* bot, Order const& order);
        void CountRefusal(Player* bot, Refusal cause, uint16 opcode, float x, float y, float z);

        ObjectGuid _bot;
        Clock _clock;
        Inbox _inbox;
        std::vector<Order> _taken;
        Movement::Client _mover;
        uint64 _applied = 0;
        std::array<uint64, std::size_t(Refusal::Count)> _refused{};
        std::array<bool, std::size_t(Refusal::Count)> _logged{};
    };

    /// The realm's server, to the controller (Movement::ServerLink, the C4 interface): a report goes through the
    /// session's own packet handlers, and what the server holds or imposes is read back off the Player. Made for one
    /// tick; holds no state of its own.
    class RealmLink final : public Movement::ServerLink
    {
    public:
        RealmLink(Player* bot, CompanionClient& client) : _bot(bot), _client(client) { }

        bool Apply(Movement::Report const& report) override;
        [[nodiscard]] Movement::ServerState State() const override;

    private:
        /// Movement as WriteMovementInfo writes it after the guid (the acks put their counter between the two).
        void AppendMovement(WorldPacket& packet, Movement::Report const& report) const;
        [[nodiscard]] Refusal Classify(Movement::Report const& report) const;

        Player* _bot;
        CompanionClient& _client;
    };

    /// The companions' clients by session, for the packet hook and the tick: registered and removed on the world
    /// thread, looked up from any thread. A client is shared, so a packet being pushed while its companion leaves
    /// still has a client to push into.
    namespace Clients
    {
        void Register(WorldSession* session, std::shared_ptr<CompanionClient> client);
        void Unregister(WorldSession* session);
        [[nodiscard]] std::shared_ptr<CompanionClient> Find(WorldSession* session);

        /// ServerScript::OnPacketSent: an order for a companion's client goes into its inbox. Every packet to every
        /// session comes through here, so the opcode is tested first and nothing else is done for the rest.
        void OnPacketSent(WorldSession* session, WorldPacket const& packet);
        /// PlayerScript::OnPlayerAfterUpdate (map threads): a companion's client ticks; any other player returns at
        /// the first test (no companion anywhere, or a session that is not one).
        void OnPlayerAfterUpdate(Player* player, uint32 diff);
    }
}

#endif

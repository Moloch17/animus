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

#include "ClientOrders.h"
#include "Define.h"
#include "ObjectGuid.h"
#include <array>
#include <memory>
#include <vector>

struct MovementInfo;
class Player;
class WorldPacket;
class WorldSession;

/// **A companion's client, the realm half** (player-controller realm port, R1). A companion is a Player on a
/// WorldSession with no socket, so nothing ever reads what the server sends it and nothing ever arrives from it. This
/// stands in for the game client on that session, through the stock core's own handlers:
///
/// - **Reports** its movement as a client does: a real movement packet (WorldSession::WriteMovementInfo, the stock
///   serializer) handed to WorldSession::HandleMovementOpcodes, which reads it, verifies it (VerifyMovementInfo: a
///   spline in progress, DISABLE_MOVE, roots, the anticheat hooks), lands falls (HandleFall), moves the player
///   (zone, grid, visibility, transports) and relays it to every player watching. Each report is checked afterwards
///   against what the server kept (the post-check): a refusal is a signal, counted, and returned to the caller.
/// - **Answers the server**: the orders it sends a client about its own movement (Movement/ClientOrders.h, shared
///   with the forge) are taken off the session as they are sent (ServerScript::OnPacketSent, which fires for
///   sessions without a socket), queued, and acknowledged on the companion's tick with the counter they carried,
///   through the stock ack handlers.
/// - **Keeps a clock** and answers time sync, so the server's movement times are synchronised as a client's are.
///
/// When to report, and what the body does about an order, is the controller's (shared with the forge behind the C4
/// applier interface); this is the realm's transport for it.
///
/// Threads: Push from any thread (it is the inbox's only entry); everything else on the companion's map thread, which
/// is where a client's movement opcodes are handled (PROCESS_THREADSAFE).
namespace Animus::Client
{
    /// Why the server did not keep a report as sent.
    enum class Refusal : uint8
    {
        None,
        Teleporting,        // between a teleport and its ack: the handler ignores movement
        NotMover,           // charmed or on a vehicle: the session moves another unit
        InvalidPosition,    // not a valid map coordinate: a bug in the caller, logged loudly
        Spline,             // a core spline (charge, jump, fear, knockback on the server's side) is running
        DisableMove,        // UNIT_FLAG_DISABLE_MOVE: moving packets are ignored, others keep the server's position
        Rooted,             // MOVEMENTFLAG_ROOT on the server and the report moved
        Other,              // kept differently for a reason not listed (an anticheat hook, the under-map kill)
        Count
    };

    [[nodiscard]] char const* RefusalName(Refusal refusal);

    struct ReportResult
    {
        bool Applied = false;       // the server's position and flags are what was sent
        Refusal Cause = Refusal::None;
        uint32 Stripped = 0;        // flags the server took off the report (ReadMovementInfo's rules), when applied
    };

    class CompanionClient
    {
    public:
        explicit CompanionClient(ObjectGuid bot);

        [[nodiscard]] ObjectGuid Bot() const { return _bot; }
        [[nodiscard]] Clock const& ClientClock() const { return _clock; }

        /// An order the server just sent the session, as the client receives it. Any thread.
        void Push(uint16 opcode, uint8 const* data, std::size_t size);

        /// Ask the server for a time sync now (its own ResetTimeSync + SendTimeSync), as it does for a player that
        /// entered a map: a companion placed beside its owner never got one, and until one is answered every
        /// movement packet logs "clockDelta is erronous". Answered on the next TakeOrders.
        void RequestTimeSync(Player* bot);

        /// The orders waiting, oldest first. Time syncs are answered here (the answer needs nothing from the body);
        /// the rest are returned for the caller to take into the body, each then acknowledged with Ack in this order.
        [[nodiscard]] std::vector<Order> TakeOrders(Player* bot);

        /// Acknowledge `order` with the client's movement as it is after taking it (`info`: position, facing and
        /// flags -- FlagsAfter(order, ...) -- of the body), through the stock handler for its ack opcode. The
        /// movement time is the client's clock.
        void Ack(Player* bot, Order const& order, MovementInfo info);

        /// Report `info` (the body's position, facing, pitch, flags, fall) under `opcode` (a MSG_MOVE_* the client
        /// sends) through the session's HandleMovementOpcodes, then check what the server kept.
        ReportResult Report(Player* bot, MovementInfo info, uint16 opcode);

        /// Reports made, and refused by cause, since the client was made (for the status line and the columns).
        [[nodiscard]] uint64 Reports() const { return _reports; }
        [[nodiscard]] uint64 Refused(Refusal cause) const { return _refused[std::size_t(cause)]; }
        [[nodiscard]] uint64 Acks() const { return _acks; }

    private:
        void AnswerTimeSync(Player* bot, Order const& order);
        /// The guid-less body of a MovementInfo, exactly as WriteMovementInfo writes it.
        static void AppendMovement(WorldSession* session, WorldPacket& packet, MovementInfo& info);
        [[nodiscard]] Refusal Classify(Player* bot, MovementInfo const& sent) const;

        ObjectGuid _bot;
        Clock _clock;
        Inbox _inbox;
        std::vector<Order> _taken;
        uint64 _reports = 0;
        uint64 _acks = 0;
        std::array<uint64, std::size_t(Refusal::Count)> _refused{};
        std::array<bool, std::size_t(Refusal::Count)> _logged{};
    };

    /// The companions' clients by session, for OnPacketSent: registered and removed on the world thread, looked up
    /// from any thread that sends a packet. A client is shared, so a packet being pushed while its companion leaves
    /// still has a client to push into.
    namespace Clients
    {
        void Register(WorldSession* session, std::shared_ptr<CompanionClient> client);
        void Unregister(WorldSession* session);
        [[nodiscard]] std::shared_ptr<CompanionClient> Find(WorldSession* session);

        /// ServerScript::OnPacketSent: an order for a companion's client goes into its inbox. Every packet to every
        /// session comes through here, so the opcode is tested first and nothing else is done for the rest.
        void OnPacketSent(WorldSession* session, WorldPacket const& packet);
    }
}

#endif

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

#include "CompanionClient.h"
#include "Capture.h"
#include "Log.h"
#include "MapWorldQuery.h"
#include "MoveSpline.h"
#include "Player.h"
#include "Timer.h"
#include "UnitBody.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <atomic>
#include <shared_mutex>
#include <unordered_map>

namespace
{
    /// The client's clock starts this far ahead of the server's (Clock): the time-sync delta is then about -1000 ms,
    /// never within the 25 ms the core needs to replace its "never synced" 0 (ComputeNewClockDelta).
    constexpr uint32 CLOCK_LEAD_MS = 1000;

    std::shared_mutex RegistryMutex;
    std::unordered_map<WorldSession*, std::shared_ptr<Animus::Client::CompanionClient>> Registry;
    /// How many clients are registered: the per-player tick hook returns at once while there are none.
    std::atomic<uint32> Registered{ 0 };

    enum class AckHandler : uint8
    {
        None,               // a movement packet: HandleMovementOpcodes
        Root,               // HandleMoveRootAck
        Speed,              // HandleForceSpeedChangeAck
        Knockback,          // HandleMoveKnockBackAck
        Flag,               // HandleMoveFlagChangeOpcode, with "is applied"
        Gravity,            // HandleMoveFlagChangeOpcode, without
    };

    AckHandler HandlerOf(uint16 opcode)
    {
        switch (opcode)
        {
            case CMSG_FORCE_MOVE_ROOT_ACK:
            case CMSG_FORCE_MOVE_UNROOT_ACK:
                return AckHandler::Root;
            case CMSG_FORCE_WALK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_RUN_SPEED_CHANGE_ACK:
            case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK:
            case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_TURN_RATE_CHANGE_ACK:
            case CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK:
            case CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:
            case CMSG_FORCE_PITCH_RATE_CHANGE_ACK:
                return AckHandler::Speed;
            case CMSG_MOVE_KNOCK_BACK_ACK:
                return AckHandler::Knockback;
            case CMSG_MOVE_HOVER_ACK:
            case CMSG_MOVE_FEATHER_FALL_ACK:
            case CMSG_MOVE_WATER_WALK_ACK:
            case CMSG_MOVE_SET_CAN_FLY_ACK:
                return AckHandler::Flag;
            case CMSG_MOVE_GRAVITY_DISABLE_ACK:
            case CMSG_MOVE_GRAVITY_ENABLE_ACK:
                return AckHandler::Gravity;
            default:
                return AckHandler::None;
        }
    }

    MovementInfo InfoOf(Player const* bot, Animus::Movement::Report const& report)
    {
        MovementInfo info;
        info.guid = bot->GetGUID();
        info.flags = report.Flags;
        info.flags2 = 0;
        info.time = report.TimeMs;
        info.pos.Relocate(report.X, report.Y, report.Z, report.Yaw);
        info.transport.Reset();
        info.pitch = report.Pitch;
        info.fallTime = report.FallMs;
        info.jump.zspeed = report.JumpZSpeed;
        info.jump.sinAngle = report.JumpSin;
        info.jump.cosAngle = report.JumpCos;
        info.jump.xyspeed = report.JumpXYSpeed;
        return info;
    }
}

char const* Animus::Client::RefusalName(Refusal refusal)
{
    switch (refusal)
    {
        case Refusal::None: return "none";
        case Refusal::Teleporting: return "teleporting";
        case Refusal::NotMover: return "not the mover";
        case Refusal::InvalidPosition: return "invalid position";
        case Refusal::Spline: return "core spline";
        case Refusal::DisableMove: return "movement disabled";
        case Refusal::Rooted: return "rooted";
        case Refusal::Other: return "other";
        default: return "?";
    }
}

Animus::Client::CompanionClient::CompanionClient(ObjectGuid bot)
    : _bot(bot), _clock(std::chrono::steady_clock::now(), getMSTime() + CLOCK_LEAD_MS)
{
}

void Animus::Client::CompanionClient::Push(uint16 opcode, uint8 const* data, std::size_t size)
{
    std::optional<Order> order = Decode(opcode, data, size, _bot.GetRawValue());
    if (!order)
        return;

    // It arrives now: the client's clock and the steady time of its arrival are what a client answering at once
    // would report and be received at.
    order->ArrivedAt = std::chrono::steady_clock::now();
    order->ClientMs = _clock.At(order->ArrivedAt);
    _inbox.Push(*order);
}

void Animus::Client::CompanionClient::RequestTimeSync(Player* bot)
{
    if (!bot || !bot->GetSession())
        return;

    bot->GetSession()->ResetTimeSync();
    bot->GetSession()->SendTimeSync();
}

void Animus::Client::CompanionClient::AnswerTimeSync(Player* bot, Order const& order)
{
    // CMSG_TIME_SYNC_RESP: the request's counter and the client's clock when it handled it. The handler measures the
    // round trip from the packet's receive time (WorldPacket::GetReceivedTime), so the answer is stamped as received
    // the moment the request arrived, which is when a client on a perfect link would have answered it.
    WorldPacket response(Op::CMSG_TIME_SYNC_RESP, 8);
    response << uint32(order.Counter);
    response << uint32(order.ClientMs);
    WorldPacket received(std::move(response), order.ArrivedAt);
    bot->GetSession()->HandleTimeSyncResp(received);
}

void Animus::Client::CompanionClient::Tick(Player* bot, uint32 diff)
{
    if (!bot || !bot->IsInWorld() || !bot->FindMap() || !bot->GetSession())
        return;

    RealmLink link(bot, *this);
    Movement::MapWorldQuery const world(bot->GetMap(), bot->GetPhaseMask());
    Movement::Body const shape = Movement::ShapeOf(bot);
    uint32 const nowMs = _clock.Now();

    // Dead, a body or a ghost: nothing is held and nothing moves. A ghost's run back to its corpse is a skill the
    // companions will learn in a later stage (movement-curriculum §6); until then there is no corpse run at all.
    // The orders still get their answers, as a client's do.
    bool const alive = bot->IsAlive();
    if (!alive)
        Controls.Clear();

    // Time syncs first: they need nothing from the body, and the first report must already go out synchronised
    // (an unanswered sync makes the core log every movement packet). The other orders keep their order after them.
    _inbox.Drain(_taken);
    for (Order const& order : _taken)
        if (order.Kind == OrderKind::TimeSync)
            AnswerTimeSync(bot, order);

    if (!_mover.Started())
        _mover.Start(link, shape, world, nowMs);

    for (Order const& order : _taken)
        if (order.Kind != OrderKind::TimeSync)
            _mover.Order(order, link, shape, world, nowMs);

    if (alive)
        _mover.Tick(Controls.Held, Movement::SpeedsOf(bot), shape, world, diff, nowMs, link);
}

void Animus::Client::CompanionClient::CountRefusal(Player* bot, Refusal cause, uint16 opcode, float x, float y,
    float z)
{
    ++_refused[std::size_t(cause)];
    if (cause == Refusal::InvalidPosition)
        LOG_ERROR("module.animus", "Companion {} reported an invalid position ({}, {}, {}) with opcode {:#x}",
            bot->GetName(), x, y, z, opcode);
    else if (!_logged[std::size_t(cause)])
    {
        _logged[std::size_t(cause)] = true;
        LOG_INFO("module.animus", "Companion {}: the server refused a movement report ({}, opcode {:#x}); further "
            "refusals for this reason are counted only", bot->GetName(), RefusalName(cause), opcode);
    }
}

void Animus::Client::RealmLink::AppendMovement(WorldPacket& packet, Movement::Report const& report) const
{
    // The stock serializer writes the packed guid first; the acks put their counter between the two, so the guid's
    // bytes are skipped here and the rest appended as it wrote it.
    MovementInfo info = InfoOf(_bot, report);
    WorldPacket whole(packet.GetOpcode(), 64);
    _bot->GetSession()->WriteMovementInfo(&whole, &info);
    std::size_t const guidBytes = info.guid.WriteAsPacked().size();
    if (whole.size() > guidBytes)
        packet.append(whole.contents() + guidBytes, whole.size() - guidBytes);
}

bool Animus::Client::RealmLink::Apply(Movement::Report const& report)
{
    Player* bot = _bot;
    if (!bot || !bot->IsInWorld() || bot->IsDuringRemoveFromWorld() || !bot->GetSession())
        return false;
    WorldSession* session = bot->GetSession();
    uint16 const opcode = report.Opcode;

    if (opcode == MSG_MOVE_TELEPORT_ACK)
    {
        // A near teleport waits for the same opcode back (HandleMoveTeleportAck: guid, counter, time). The module's
        // own teleports acknowledge themselves at once (BotFactory), so by now there may be nothing waiting.
        if (bot->IsBeingTeleportedNear())
        {
            WorldPacket ack(MSG_MOVE_TELEPORT_ACK, 16);
            ack << bot->GetPackGUID();
            ack << uint32(report.Counter) << uint32(report.TimeMs);
            session->HandleMoveTeleportAck(ack);
        }
        ++_client._applied;
        return true;
    }

    AckHandler const handler = HandlerOf(opcode);
    // The unroot ack's pre-check: a stock core never holds MOVEMENTFLAG_ROOT for a client-controlled player (the
    // client's ROOT is stripped by ReadMovementInfo), so HandleMoveRootAck returns before it reads the movement. The
    // ack is still sent, as a client sends it; there is nothing for the server to keep.
    bool const unrootIgnored = opcode == CMSG_FORCE_MOVE_UNROOT_ACK
        && !bot->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_ROOT);

    // Counted for the capture as a player's packet is when it reaches the handler (MoveTally); kept ones are recorded
    // by the handler's own hook, as a player's are.
    Capture::MovementSent(bot, opcode);

    WorldPacket packet(opcode, 80);
    if (handler == AckHandler::None)
    {
        MovementInfo info = InfoOf(bot, report);
        session->WriteMovementInfo(&packet, &info);
        session->HandleMovementOpcodes(packet);
    }
    else
    {
        // The stock ack handlers read: packed guid, the counter (the knockback ack's "unk" is the same counter), the
        // client's movement, then the speed (speed acks) or "is applied" (flag acks other than gravity).
        packet << bot->GetPackGUID();
        packet << uint32(report.Counter);
        AppendMovement(packet, report);
        switch (handler)
        {
            case AckHandler::Root:
                session->HandleMoveRootAck(packet);
                break;
            case AckHandler::Speed:
                packet << float(report.Speed);
                session->HandleForceSpeedChangeAck(packet);
                break;
            case AckHandler::Knockback:
                session->HandleMoveKnockBackAck(packet);
                break;
            case AckHandler::Flag:
                packet << uint32(report.Applied ? 1 : 0);
                session->HandleMoveFlagChangeOpcode(packet);
                break;
            default:
                session->HandleMoveFlagChangeOpcode(packet);
                break;
        }
    }

    if (unrootIgnored)
    {
        ++_client._applied;
        return true;
    }

    // The post-check: what the server kept is what a player's client is credited with. Kept as sent, apart from
    // flags ReadMovementInfo strips (a flag the client may not claim), is applied; anything else is a refusal.
    MovementInfo const& kept = bot->m_movementInfo;
    uint32 const stripped = report.Flags & ~kept.flags;
    bool const samePosition = kept.pos.GetPositionX() == report.X && kept.pos.GetPositionY() == report.Y
        && kept.pos.GetPositionZ() == report.Z && kept.pos.GetOrientation() == report.Yaw;
    if (samePosition && (kept.flags | stripped) == report.Flags)
    {
        ++_client._applied;
        return true;
    }

    _client.CountRefusal(bot, Classify(report), opcode, report.X, report.Y, report.Z);
    return false;
}

Animus::Client::Refusal Animus::Client::RealmLink::Classify(Movement::Report const& report) const
{
    // The handler's own reasons, in the order it tests them (HandleMovementOpcodes, then VerifyMovementInfo).
    Player* bot = _bot;
    if (bot->IsBeingTeleported())
        return Refusal::Teleporting;
    if (static_cast<Unit*>(bot->m_mover) != bot)
        return Refusal::NotMover;
    Position const pos(report.X, report.Y, report.Z, report.Yaw);
    if (!pos.IsPositionValid())
        return Refusal::InvalidPosition;
    if (!bot->movespline->Finalized())
        return Refusal::Spline;
    if (bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        return Refusal::DisableMove;
    if (bot->IsRooted())
        return Refusal::Rooted;
    return Refusal::Other;
}

Animus::Movement::ServerState Animus::Client::RealmLink::State() const
{
    Movement::ServerState state;
    Player const* bot = _bot;
    if (!bot)
    {
        state.Imposed = true;
        return state;
    }
    state.X = bot->GetPositionX();
    state.Y = bot->GetPositionY();
    state.Z = bot->GetPositionZ();
    state.Yaw = bot->GetOrientation();
    // What a client is made to yield to, read off the unit every tick: the forge's rule (PlayerLink::State) -- a root,
    // a stun, a fear or confuse, a spline (a charge, a taxi), a teleport under way, a charm, a corpse -- and on the
    // realm also a vehicle seat or a transport (the session moves another unit, or v1's controller does not ride).
    state.Imposed = !bot->IsInWorld() || bot->IsBeingTeleported()
        || bot->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING)
        || bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) || !bot->movespline->Finalized() || bot->IsCharmed()
        || (bot->isDead() && !bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        || static_cast<Unit const*>(bot->m_mover) != bot || bot->GetVehicle() || bot->GetTransport();
    return state;
}

void Animus::Client::Clients::Register(WorldSession* session, std::shared_ptr<CompanionClient> client)
{
    if (!session || !client)
        return;
    std::unique_lock<std::shared_mutex> lock(RegistryMutex);
    Registry[session] = std::move(client);
    Registered.store(uint32(Registry.size()), std::memory_order_relaxed);
}

void Animus::Client::Clients::Unregister(WorldSession* session)
{
    std::unique_lock<std::shared_mutex> lock(RegistryMutex);
    Registry.erase(session);
    Registered.store(uint32(Registry.size()), std::memory_order_relaxed);
}

std::shared_ptr<Animus::Client::CompanionClient> Animus::Client::Clients::Find(WorldSession* session)
{
    if (!Registered.load(std::memory_order_relaxed))
        return nullptr;
    std::shared_lock<std::shared_mutex> lock(RegistryMutex);
    auto const found = Registry.find(session);
    return found == Registry.end() ? nullptr : found->second;
}

void Animus::Client::Clients::OnPacketSent(WorldSession* session, WorldPacket const& packet)
{
    if (!Answers(packet.GetOpcode()) || !session)
        return;

    std::shared_ptr<CompanionClient> client = Find(session);
    if (!client || packet.empty())
        return;

    client->Push(packet.GetOpcode(), packet.contents(), packet.size());
}

void Animus::Client::Clients::OnPlayerAfterUpdate(Player* player, uint32 diff)
{
    if (!Registered.load(std::memory_order_relaxed) || !player)
        return;
    if (std::shared_ptr<CompanionClient> client = Find(player->GetSession()))
        client->Tick(player, diff);
}

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
#include "Log.h"
#include "MoveSpline.h"
#include "Player.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <shared_mutex>
#include <unordered_map>

namespace
{
    /// The client's clock starts this far ahead of the server's (Clock): the time-sync delta is then about -1000 ms,
    /// never within the 25 ms the core needs to replace its "never synced" 0.
    constexpr uint32 CLOCK_LEAD_MS = 1000;

    std::shared_mutex RegistryMutex;
    std::unordered_map<WorldSession*, std::shared_ptr<Animus::Client::CompanionClient>> Registry;

    bool SamePosition(Position const& a, Position const& b)
    {
        // Floats written and read back through a packet are bit-exact, so anything but equality is a change.
        return a.GetPositionX() == b.GetPositionX() && a.GetPositionY() == b.GetPositionY()
            && a.GetPositionZ() == b.GetPositionZ() && a.GetOrientation() == b.GetOrientation();
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

std::vector<Animus::Client::Order> Animus::Client::CompanionClient::TakeOrders(Player* bot)
{
    _inbox.Drain(_taken);
    std::vector<Order> orders;
    orders.reserve(_taken.size());
    for (Order const& order : _taken)
    {
        if (order.Kind == OrderKind::TimeSync)
            AnswerTimeSync(bot, order);
        else
            orders.push_back(order);
    }
    return orders;
}

void Animus::Client::CompanionClient::AnswerTimeSync(Player* bot, Order const& order)
{
    if (!bot || !bot->GetSession())
        return;

    // CMSG_TIME_SYNC_RESP: the request's counter and the client's clock when it handled it. The handler measures the
    // round trip from the packet's receive time (WorldPacket::GetReceivedTime), so the answer is stamped as received
    // the moment the request arrived, which is when a client on a perfect link would have answered it.
    WorldPacket response(Op::CMSG_TIME_SYNC_RESP, 8);
    response << uint32(order.Counter);
    response << uint32(order.ClientMs);
    WorldPacket received(std::move(response), order.ArrivedAt);
    bot->GetSession()->HandleTimeSyncResp(received);
}

void Animus::Client::CompanionClient::AppendMovement(WorldSession* session, WorldPacket& packet, MovementInfo& info)
{
    // The stock serializer writes the packed guid first; the acks put their counter between the two, so the guid's
    // bytes are skipped here and the rest appended as it wrote it.
    WorldPacket whole(packet.GetOpcode(), 64);
    session->WriteMovementInfo(&whole, &info);
    std::size_t const guidBytes = info.guid.WriteAsPacked().size();
    if (whole.size() > guidBytes)
        packet.append(whole.contents() + guidBytes, whole.size() - guidBytes);
}

void Animus::Client::CompanionClient::Ack(Player* bot, Order const& order, MovementInfo info)
{
    if (!bot || !bot->GetSession() || order.Kind == OrderKind::TimeSync)
        return;

    WorldSession* session = bot->GetSession();
    if (order.Kind == OrderKind::Teleport)
    {
        // A near teleport waits for the same opcode back (HandleMoveTeleportAck: guid, counter, time). The module's
        // own teleports acknowledge themselves at once (BotFactory), so by now there may be nothing waiting.
        if (bot->IsBeingTeleportedNear())
        {
            WorldPacket ack(MSG_MOVE_TELEPORT_ACK, 16);
            ack << bot->GetPackGUID();
            ack << uint32(order.Counter) << uint32(_clock.Now());
            session->HandleMoveTeleportAck(ack);
            ++_acks;
        }
        return;
    }
    info.guid = _bot;
    info.time = _clock.Now();
    // A knockback is acknowledged as the fall it launches: HandleMoveKnockBackAck relays these jump fields to every
    // watching player, and they are written only with FALLING set.
    if (order.Kind == OrderKind::Knockback)
    {
        info.AddMovementFlag(MOVEMENTFLAG_FALLING);
        info.RemoveMovementFlag(MOVEMENTFLAG_ROOT);
        info.SetFallTime(0);
        info.jump.zspeed = order.SpeedZ;
        info.jump.sinAngle = order.Sin;
        info.jump.cosAngle = order.Cos;
        info.jump.xyspeed = order.SpeedXY;
    }

    // The stock ack handlers read: packed guid, the counter (the knockback ack's "unk" is the same counter), the
    // client's movement, then the speed (speed acks) or "is applied" (flag acks other than gravity).
    WorldPacket ack(AckOpcode(order), 80);
    ack << info.guid.WriteAsPacked();
    ack << uint32(order.Counter);
    AppendMovement(session, ack, info);
    if (order.Kind == OrderKind::Speed)
        ack << float(order.Value);
    else if (AckCarriesApplied(order.Kind))
        ack << uint32(Applies(order.Kind) ? 1 : 0);

    switch (order.Kind)
    {
        case OrderKind::Root:
        case OrderKind::Unroot:
            session->HandleMoveRootAck(ack);
            break;
        case OrderKind::Speed:
            session->HandleForceSpeedChangeAck(ack);
            break;
        case OrderKind::Knockback:
            session->HandleMoveKnockBackAck(ack);
            break;
        default:
            session->HandleMoveFlagChangeOpcode(ack);
            break;
    }
    ++_acks;
}

Animus::Client::ReportResult Animus::Client::CompanionClient::Report(Player* bot, MovementInfo info, uint16 opcode)
{
    ReportResult result;
    if (!bot || !bot->GetSession())
    {
        result.Cause = Refusal::Other;
        return result;
    }

    info.guid = _bot;
    info.time = _clock.Now();

    WorldPacket packet(opcode, 64);
    bot->GetSession()->WriteMovementInfo(&packet, &info);
    bot->GetSession()->HandleMovementOpcodes(packet);
    ++_reports;

    // The post-check: what the server kept is what a player's client is credited with. Kept as sent, apart from
    // flags ReadMovementInfo strips (a flag the client may not claim), is applied; anything else is a refusal.
    MovementInfo const& kept = bot->m_movementInfo;
    uint32 const stripped = info.flags & ~kept.flags;
    if (SamePosition(kept.pos, info.pos) && (kept.flags | stripped) == info.flags)
    {
        result.Applied = true;
        result.Stripped = stripped;
        return result;
    }

    result.Cause = Classify(bot, info);
    ++_refused[std::size_t(result.Cause)];
    if (result.Cause == Refusal::InvalidPosition)
        LOG_ERROR("module.animus", "Companion {} reported an invalid position ({}, {}, {}, {}) with opcode {:#x}",
            bot->GetName(), info.pos.GetPositionX(), info.pos.GetPositionY(), info.pos.GetPositionZ(),
            info.pos.GetOrientation(), opcode);
    else if (!_logged[std::size_t(result.Cause)])
    {
        _logged[std::size_t(result.Cause)] = true;
        LOG_INFO("module.animus", "Companion {}: the server refused a movement report ({}, opcode {:#x}); further "
            "refusals for this reason are counted only", bot->GetName(), RefusalName(result.Cause), opcode);
    }
    return result;
}

Animus::Client::Refusal Animus::Client::CompanionClient::Classify(Player* bot, MovementInfo const& sent) const
{
    // The handler's own reasons, in the order it tests them (HandleMovementOpcodes, then VerifyMovementInfo).
    if (bot->IsBeingTeleported())
        return Refusal::Teleporting;
    if (static_cast<Unit*>(bot->m_mover) != bot)
        return Refusal::NotMover;
    if (!sent.pos.IsPositionValid())
        return Refusal::InvalidPosition;
    if (!bot->movespline->Finalized())
        return Refusal::Spline;
    if (bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        return Refusal::DisableMove;
    if (bot->IsRooted())
        return Refusal::Rooted;
    return Refusal::Other;
}

void Animus::Client::Clients::Register(WorldSession* session, std::shared_ptr<CompanionClient> client)
{
    if (!session)
        return;
    std::unique_lock<std::shared_mutex> lock(RegistryMutex);
    Registry[session] = std::move(client);
}

void Animus::Client::Clients::Unregister(WorldSession* session)
{
    std::unique_lock<std::shared_mutex> lock(RegistryMutex);
    Registry.erase(session);
}

std::shared_ptr<Animus::Client::CompanionClient> Animus::Client::Clients::Find(WorldSession* session)
{
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

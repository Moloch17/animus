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

// The client's orders (animus-lib/src/runtime/Movement/ClientOrders.h, bundled byte for byte from the forge, where
// its GTests run), checked here against the stock core the realm runs: the module has no test target and a stock core
// builds none for it, so this is a standalone program (outside src/, never built into the server):
//
//     g++ -std=c++20 -O1 -Wall -Wextra -pthread -I animus-lib/src/runtime/Movement -o /tmp/client_orders_test
//         tests/ClientOrdersTest.cpp && /tmp/client_orders_test
//
// Packets are built byte for byte as the stock core writes them (the functions named beside each), and the flags a
// client acknowledges with are checked against the stock core's ReadMovementInfo rules (StockStrips, copied from
// WorldSession.cpp as the test's oracle).

#include "ClientOrders.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace Animus::Client;

namespace
{
    int Failures = 0;

    void Check(bool ok, char const* what, int line)
    {
        if (!ok)
        {
            ++Failures;
            std::printf("FAIL line %d: %s\n", line, what);
        }
    }
#define CHECK(x) Check((x), #x, __LINE__)

    /// A packet as the core's ByteBuffer writes it (little-endian).
    struct Bytes
    {
        std::vector<uint8_t> Data;

        template <typename T>
        Bytes& Put(T value)
        {
            uint8_t raw[sizeof(T)];
            std::memcpy(raw, &value, sizeof(T));
            Data.insert(Data.end(), raw, raw + sizeof(T));
            return *this;
        }

        /// ByteBuffer::appendPackGUID.
        Bytes& PackedGuid(uint64_t guid)
        {
            uint8_t mask = 0;
            std::vector<uint8_t> body;
            for (int i = 0; i < 8; ++i)
                if (uint8_t const byte = uint8_t(guid >> (i * 8)))
                {
                    mask |= uint8_t(1 << i);
                    body.push_back(byte);
                }
            Data.push_back(mask);
            Data.insert(Data.end(), body.begin(), body.end());
            return *this;
        }
    };

    constexpr uint64_t SELF = 0x0000000000012A07ull;     // a player guid: high 0, low 76295
    constexpr uint64_t OTHER = 0x0000000000012A08ull;

    /// The stock ReadMovementInfo's REMOVE_VIOLATING_FLAGS rules (WorldSession.cpp), as the oracle: the flags the
    /// server takes off a client's movement given the player's auras.
    struct Auras
    {
        bool Hover = false;
        bool WaterWalk = false;
        bool Ghost = false;
        bool FeatherFall = false;
        bool Fly = false;
    };

    uint32_t StockStrips(uint32_t f, Auras const& a)
    {
        uint32_t strip = 0;
        auto has = [&f](uint32_t m) { return (f & m) != 0; };
        if (has(0x00000800))
            strip |= 0x00000800;                                    // ROOT: always
        if (has(0x40000000) && !a.Hover)
            strip |= 0x40000000;                                    // HOVER
        if (has(0x00400000) && has(0x00800000))
            strip |= 0x00400000 | 0x00800000;                       // ASCENDING + DESCENDING
        if (has(0x10) && has(0x20))
            strip |= 0x30;                                          // LEFT + RIGHT
        if (has(0x4) && has(0x8))
            strip |= 0xc;                                           // STRAFE both
        if (has(0x40) && has(0x80))
            strip |= 0xc0;                                          // PITCH both
        if (has(0x1) && has(0x2))
            strip |= 0x3;                                           // FORWARD + BACKWARD
        if (has(0x10000000) && !a.WaterWalk && !a.Ghost)
            strip |= 0x10000000;                                    // WATERWALKING
        if (has(0x20000000) && !a.FeatherFall)
            strip |= 0x20000000;                                    // FALLING_SLOW
        if (has(0x02000000 | 0x01000000) && !a.Fly)
            strip |= 0x02000000 | 0x01000000;                       // FLYING / CAN_FLY
        if (has(0x01000000 | 0x00000400) && has(0x00001000))
            strip |= 0x00001000;                                    // FALLING with CAN_FLY or DISABLE_GRAVITY
        return strip;
    }

    void TestDecode()
    {
        // Unit::SendSpeedToController: guid, counter, [uint8(0) for run], speed.
        Bytes run;
        run.PackedGuid(SELF).Put<uint32_t>(7).Put<uint8_t>(0).Put<float>(9.8f);
        auto order = Decode(Op::SMSG_FORCE_RUN_SPEED_CHANGE, run.Data.data(), run.Data.size(), SELF);
        CHECK(order && order->Kind == OrderKind::Speed && order->Speed == SpeedType::Run && order->Counter == 7
            && order->Value == 9.8f);
        CHECK(order && AckOpcode(*order) == Op::CMSG_FORCE_RUN_SPEED_CHANGE_ACK);

        Bytes swim;
        swim.PackedGuid(SELF).Put<uint32_t>(8).Put<float>(4.72f);
        order = Decode(Op::SMSG_FORCE_SWIM_SPEED_CHANGE, swim.Data.data(), swim.Data.size(), SELF);
        CHECK(order && order->Speed == SpeedType::Swim && order->Value == 4.72f
            && AckOpcode(*order) == Op::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK);
        order = Decode(Op::SMSG_FORCE_PITCH_RATE_CHANGE, swim.Data.data(), swim.Data.size(), SELF);
        CHECK(order && AckOpcode(*order) == Op::CMSG_FORCE_PITCH_RATE_CHANGE_ACK);

        // Unit::SendMoveRoot (client-controlled): guid, counter.
        Bytes root;
        root.PackedGuid(SELF).Put<uint32_t>(11);
        order = Decode(Op::SMSG_FORCE_MOVE_ROOT, root.Data.data(), root.Data.size(), SELF);
        CHECK(order && order->Kind == OrderKind::Root && order->Counter == 11
            && AckOpcode(*order) == Op::CMSG_FORCE_MOVE_ROOT_ACK);
        order = Decode(Op::SMSG_FORCE_MOVE_UNROOT, root.Data.data(), root.Data.size(), SELF);
        CHECK(order && order->Kind == OrderKind::Unroot && AckOpcode(*order) == Op::CMSG_FORCE_MOVE_UNROOT_ACK);

        // Another unit's order (a pet's, a vehicle's) is not this client's to answer.
        CHECK(!Decode(Op::SMSG_FORCE_MOVE_ROOT, root.Data.data(), root.Data.size(), OTHER));
        // Short packets are refused whole.
        CHECK(!Decode(Op::SMSG_FORCE_MOVE_ROOT, root.Data.data(), root.Data.size() - 1, SELF));
        CHECK(!Decode(Op::SMSG_FORCE_RUN_SPEED_CHANGE, run.Data.data(), run.Data.size() - 1, SELF));
        CHECK(!Decode(Op::SMSG_FORCE_MOVE_ROOT, nullptr, 0, SELF));
        // Opcodes the client does not answer.
        CHECK(!Answers(0x0B5));     // MSG_MOVE_START_FORWARD (another player's relay)
        CHECK(!Decode(0x0B5, root.Data.data(), root.Data.size(), SELF));

        // Unit::SetCanFly / SetWaterWalking / SetFeatherFall / SetHover / SetDisableGravity: guid, counter.
        struct Flag { uint16_t Smsg; OrderKind Kind; uint16_t Ack; bool Applied; bool CarriesApplied; };
        Flag const flags[] = {
            { Op::SMSG_MOVE_SET_CAN_FLY, OrderKind::CanFly, Op::CMSG_MOVE_SET_CAN_FLY_ACK, true, true },
            { Op::SMSG_MOVE_UNSET_CAN_FLY, OrderKind::UnsetCanFly, Op::CMSG_MOVE_SET_CAN_FLY_ACK, false, true },
            { Op::SMSG_MOVE_WATER_WALK, OrderKind::WaterWalk, Op::CMSG_MOVE_WATER_WALK_ACK, true, true },
            { Op::SMSG_MOVE_LAND_WALK, OrderKind::LandWalk, Op::CMSG_MOVE_WATER_WALK_ACK, false, true },
            { Op::SMSG_MOVE_FEATHER_FALL, OrderKind::FeatherFall, Op::CMSG_MOVE_FEATHER_FALL_ACK, true, true },
            { Op::SMSG_MOVE_NORMAL_FALL, OrderKind::NormalFall, Op::CMSG_MOVE_FEATHER_FALL_ACK, false, true },
            { Op::SMSG_MOVE_SET_HOVER, OrderKind::Hover, Op::CMSG_MOVE_HOVER_ACK, true, true },
            { Op::SMSG_MOVE_UNSET_HOVER, OrderKind::UnsetHover, Op::CMSG_MOVE_HOVER_ACK, false, true },
            { Op::SMSG_MOVE_GRAVITY_DISABLE, OrderKind::GravityDisable, Op::CMSG_MOVE_GRAVITY_DISABLE_ACK, true,
                false },
            { Op::SMSG_MOVE_GRAVITY_ENABLE, OrderKind::GravityEnable, Op::CMSG_MOVE_GRAVITY_ENABLE_ACK, false, false },
        };
        for (Flag const& flag : flags)
        {
            order = Decode(flag.Smsg, root.Data.data(), root.Data.size(), SELF);
            CHECK(order && order->Kind == flag.Kind && order->Counter == 11 && AckOpcode(*order) == flag.Ack);
            CHECK(AckCarriesApplied(flag.Kind) == flag.CarriesApplied);
            CHECK(!flag.CarriesApplied || Applies(flag.Kind) == flag.Applied);
        }

        // Unit::KnockbackFrom (client-controlled): guid, counter, cos, sin, speedXY, -speedZ.
        Bytes knock;
        knock.PackedGuid(SELF).Put<uint32_t>(12).Put<float>(0.6f).Put<float>(0.8f).Put<float>(10.0f).Put<float>(-7.0f);
        order = Decode(Op::SMSG_MOVE_KNOCK_BACK, knock.Data.data(), knock.Data.size(), SELF);
        CHECK(order && order->Kind == OrderKind::Knockback && order->Counter == 12 && order->Cos == 0.6f
            && order->Sin == 0.8f && order->SpeedXY == 10.0f && order->SpeedZ == -7.0f
            && AckOpcode(*order) == Op::CMSG_MOVE_KNOCK_BACK_ACK);

        // Player::SendTeleportAckPacket (a near teleport): guid, counter, then movement the client does not read.
        Bytes teleport;
        teleport.PackedGuid(SELF).Put<uint32_t>(13).Put<uint32_t>(0).Put<uint16_t>(0);
        order = Decode(Op::MSG_MOVE_TELEPORT_ACK, teleport.Data.data(), teleport.Data.size(), SELF);
        CHECK(order && order->Kind == OrderKind::Teleport && order->Counter == 13
            && AckOpcode(*order) == Op::MSG_MOVE_TELEPORT_ACK);
        CHECK(!Decode(Op::MSG_MOVE_TELEPORT_ACK, teleport.Data.data(), teleport.Data.size(), OTHER));

        // WorldSession::SendTimeSync: the counter alone.
        Bytes sync;
        sync.Put<uint32_t>(3);
        order = Decode(Op::SMSG_TIME_SYNC_REQ, sync.Data.data(), sync.Data.size(), SELF);
        CHECK(order && order->Kind == OrderKind::TimeSync && order->Counter == 3
            && AckOpcode(*order) == Op::CMSG_TIME_SYNC_RESP);
    }

    void TestAckFlags()
    {
        // A client acknowledging an order never claims a flag the stock server strips -- given the aura the order
        // came with -- except ROOT, which the stock ReadMovementInfo strips from every client packet (the server's
        // own SendMoveRoot is what a client-controlled player's root rests on).
        uint32_t const states[] = { 0u, 0x1u, 0x1u | 0x8u | 0x10u, 0x00200000u | 0x1u | 0x40u, 0x00001000u,
            0x01000000u | 0x02000000u | 0x00400000u };
        Auras all;
        all.Hover = all.WaterWalk = all.FeatherFall = all.Fly = true;
        for (int k = int(OrderKind::Root); k <= int(OrderKind::Knockback); ++k)
        {
            Order order;
            order.Kind = OrderKind(k);
            for (uint32_t state : states)
            {
                Auras auras = all;
                // Unset orders come once the aura is gone: the flag must be gone too.
                if (order.Kind == OrderKind::UnsetCanFly)
                    auras.Fly = false;
                if (order.Kind == OrderKind::LandWalk)
                    auras.WaterWalk = false;
                if (order.Kind == OrderKind::NormalFall)
                    auras.FeatherFall = false;
                if (order.Kind == OrderKind::UnsetHover)
                    auras.Hover = false;
                // A state the client could hold under those auras (a flying state without the fly aura is not one).
                if ((state & (MoveFlag::CAN_FLY | MoveFlag::FLYING)) && !auras.Fly)
                    continue;
                uint32_t const after = FlagsAfter(order, state);
                uint32_t const strips = StockStrips(after, auras) & ~MoveFlag::ROOT;
                // A knockback onto a flyer is a fall the server takes FALLING off (it flies on): not a client error.
                bool const flyerKnocked = order.Kind == OrderKind::Knockback && (after & MoveFlag::CAN_FLY);
                if (!flyerKnocked)
                    CHECK(strips == 0);
            }
        }

        // The root: every moving key stops, turning is kept, ROOT is claimed; the unroot takes ROOT off only.
        Order root;
        root.Kind = OrderKind::Root;
        uint32_t const running = 0x1u | 0x8u | 0x10u | 0x1000u;   // forward, strafe right, turning left, falling
        CHECK(FlagsAfter(root, running) == (0x10u | MoveFlag::ROOT));
        Order unroot;
        unroot.Kind = OrderKind::Unroot;
        CHECK(FlagsAfter(unroot, FlagsAfter(root, running)) == 0x10u);

        Order knock;
        knock.Kind = OrderKind::Knockback;
        CHECK((FlagsAfter(knock, 0x1u) & MoveFlag::FALLING) != 0);
        Order speed;
        speed.Kind = OrderKind::Speed;
        CHECK(FlagsAfter(speed, running) == running);
    }

    void TestClock()
    {
        auto const origin = std::chrono::steady_clock::now();
        Clock clock(origin);
        CHECK(clock.At(origin) == Clock::CLOCK_BASE);
        CHECK(clock.At(origin + std::chrono::milliseconds(1500)) == Clock::CLOCK_BASE + 1500);
        // Never before its start, never 0, and no wrap in the first seconds (where getMSTime() - offset would).
        CHECK(clock.At(origin - std::chrono::seconds(5)) == Clock::CLOCK_BASE);
        uint32_t last = 0;
        for (int i = 0; i < 1000; ++i)
        {
            uint32_t const now = clock.Now();
            CHECK(now >= last && now != 0);
            last = now;
        }

        // The stock HandleTimeSyncResp delta from an answer stamped at arrival (round trip 0): server time minus the
        // client's, so the client's movement times map onto the server clock exactly and the delta is never 0.
        uint32_t const serverAtSent = 250;           // a server in its first second
        uint32_t const clientAtArrival = clock.At(origin);
        uint32_t const roundTrip = 0;
        int64_t const delta = int64_t(serverAtSent) + int64_t(roundTrip / 2) - int64_t(clientAtArrival);
        CHECK(delta != 0);
        CHECK(int64_t(clientAtArrival + 400) + delta == int64_t(serverAtSent + 400));

        // A companion's client is started a second ahead of the server (CompanionClient): whatever the server's
        // uptime, the delta is about -1000, never within the 25 ms the core needs to replace its unsynced 0.
        for (uint32_t serverUptime : { 0u, 250u, 1000000u, 999990u, 4000000000u })
        {
            Clock ahead(origin, serverUptime + 1000);
            int64_t const d = int64_t(serverUptime) - int64_t(ahead.At(origin));
            CHECK(std::abs(d) > 25 || serverUptime + 1000 == 0);
        }
        CHECK(Clock(origin, 0).At(origin) == Clock::CLOCK_BASE);    // a 0 base is never used
    }

    void TestInboxConcurrent()
    {
        // Several senders (the world thread, map threads) push while the companion's tick drains: nothing is lost,
        // nothing duplicated, and each sender's orders come out in the order it pushed them (FIFO per producer;
        // across producers, the order the pushes took the lock in).
        constexpr int PRODUCERS = 4;
        constexpr uint32_t PER = 20000;
        Inbox inbox;
        std::atomic<int> done{ 0 };
        std::vector<std::thread> producers;
        for (int p = 0; p < PRODUCERS; ++p)
            producers.emplace_back([&inbox, &done, p]()
            {
                for (uint32_t i = 0; i < PER; ++i)
                {
                    Order order;
                    order.Opcode = uint16_t(p);
                    order.Counter = i;
                    inbox.Push(order);
                }
                done.fetch_add(1);
            });

        std::vector<int64_t> next(PRODUCERS, 0);
        uint64_t received = 0;
        bool ordered = true;
        std::vector<Order> batch;
        int drains = 0;
        while (true)
        {
            bool const finished = done.load() == PRODUCERS;
            inbox.Drain(batch);
            ++drains;
            for (Order const& order : batch)
            {
                if (int64_t(order.Counter) != next[order.Opcode])
                    ordered = false;
                next[order.Opcode] = int64_t(order.Counter) + 1;
                ++received;
            }
            if (finished && batch.empty())
                break;
        }
        for (std::thread& producer : producers)
            producer.join();

        CHECK(ordered);
        CHECK(received == uint64_t(PRODUCERS) * PER);
        for (int p = 0; p < PRODUCERS; ++p)
            CHECK(next[p] == int64_t(PER));
        std::printf("inbox: %llu orders from %d threads in %d drains\n", (unsigned long long)received, PRODUCERS,
            drains);

        // Drain leaves it empty and clears the caller's vector first.
        Order one;
        inbox.Push(one);
        batch.assign(3, Order());
        inbox.Drain(batch);
        CHECK(batch.size() == 1);
        inbox.Drain(batch);
        CHECK(batch.empty());
    }
}

int main()
{
    TestDecode();
    TestAckFlags();
    TestClock();
    TestInboxConcurrent();
    if (Failures)
    {
        std::printf("%d check(s) failed\n", Failures);
        return 1;
    }
    std::printf("all client order checks passed\n");
    return 0;
}

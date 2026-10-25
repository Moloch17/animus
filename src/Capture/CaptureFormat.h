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

#ifndef ANIMUS_CAPTURE_FORMAT_H
#define ANIMUS_CAPTURE_FORMAT_H

// The human play capture records (doc/capture-format.md, the forge's apps/forge/python/animus/human/FORMAT.md):
// every record type's payload, packed little-endian, framed `u16 type, u16 length, payload`. Standard library only,
// so tools/capture-sample.cpp builds without the core. Nothing here allocates: records are written into a caller's
// buffer, and a record that does not fit marks the writer failed instead of growing it.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace Animus::Capture::Format
{
    static_assert(std::endian::native == std::endian::little, "capture records are written in host byte order");

    /// 2: companions move by the player controller and report through their session's movement handlers, so their
    /// Move records are client packets (source 2) and the per-decision companion sample (source 1) is gone; the
    /// MoverState record (type 13) carries what a mover's motion is compared under.
    /// 3: every Move carries the server's handling time on its monotonic clock (`server_ms`) beside the client's own
    /// (`client_ms`), MoveTally (type 14) counts each mover's movement packets sent and kept, and MapUpdate (type 15)
    /// records each tick of a map instance that holds a captured mover.
    /// 4 (2026-10-25, manual recordings): a player's own recording of their play (FORMAT §1.2: `recordings/<id>/`,
    /// a manifest.json) beside the realm-wide hourly files. Two streams more -- Note (7: Annotation, RecordingStart,
    /// RecordingEnd, Chat) and Packet (8: RawPacket, every packet both ways) -- and the records a recording adds to
    /// the old streams: Inventory, ActionButtons and Talents (session), UnitState, GameObjectState, PlayerDetail,
    /// MapSpawns and InstanceCreatures (snapshot), CombatEdge (outcome); Kill gains `victim_spawn_id` and Interact
    /// `spawn_id`, appended (an older file reads them as 0).
    constexpr uint16_t FORMAT_VERSION = 4;

    /// FileHeader `stream`, and the file name's prefix (StreamName).
    enum class Stream : uint16_t
    {
        Session = 1,
        Move = 2,
        Action = 3,
        Snapshot = 4,
        Outcome = 5,
        Companion = 6,
        Note = 7,
        Packet = 8,
    };
    constexpr uint32_t STREAM_COUNT = 8;
    /// Every stream's bit (Animus.Capture.Streams).
    constexpr uint32_t STREAMS_ALL = (1u << STREAM_COUNT) - 1;

    constexpr std::string_view StreamName(Stream stream)
    {
        switch (stream)
        {
            case Stream::Session: return "session";
            case Stream::Move: return "move";
            case Stream::Action: return "action";
            case Stream::Snapshot: return "snapshot";
            case Stream::Outcome: return "outcome";
            case Stream::Companion: return "companion";
            case Stream::Note: return "note";
            case Stream::Packet: return "packet";
        }
        return "unknown";
    }

    enum class Type : uint16_t
    {
        FileHeader = 0,
        SessionStart = 1,
        SessionContext = 2,
        SessionEnd = 3,
        GroupState = 4,
        KnownSpells = 5,
        Latency = 6,
        Inventory = 7,
        ActionButtons = 8,
        Talents = 9,
        Move = 10,
        Speeds = 11,
        MotionEvent = 12,
        MoverState = 13,
        MoveTally = 14,
        MapUpdate = 15,
        CastRequest = 20,
        CastResult = 21,
        CastEnd = 22,
        Select = 23,
        ItemUse = 24,
        Attack = 25,
        Interact = 26,
        Snapshot = 30,
        UnitState = 31,
        GameObjectState = 32,
        PlayerDetail = 33,
        MapSpawns = 34,
        InstanceCreatures = 35,
        Damage = 40,
        Heal = 41,
        Kill = 42,
        Death = 43,
        Quest = 44,
        Encounter = 45,
        PvP = 46,
        Area = 47,
        CombatEdge = 48,
        CompanionDecision = 50,
        CompanionCommand = 51,
        CompanionRating = 52,
        Annotation = 60,
        RecordingStart = 61,
        RecordingEnd = 62,
        Chat = 63,
        RawPacket = 70,
    };

    /// The stream a record type is written to.
    constexpr Stream StreamOf(Type type)
    {
        uint16_t const t = uint16_t(type);
        if (t >= 1 && t <= 9)
            return Stream::Session;
        if (t >= 10 && t <= 15)
            return Stream::Move;
        if (t >= 20 && t <= 26)
            return Stream::Action;
        if (t >= 30 && t <= 39)
            return Stream::Snapshot;
        if (t >= 40 && t <= 49)
            return Stream::Outcome;
        if (t >= 60 && t <= 69)
            return Stream::Note;
        if (t >= 70 && t <= 79)
            return Stream::Packet;
        return Stream::Companion;
    }

    /// FORMAT §2.3 MotionEvent `event`.
    enum class Motion : uint8_t
    {
        Mount = 1, Dismount = 2, TaxiStart = 3, TaxiEnd = 4, Teleport = 5, Death = 6, Resurrect = 7, Root = 8,
        Unroot = 9, StunStart = 10, StunEnd = 11, FearStart = 12, FearEnd = 13, Knockback = 14, LoadingStart = 15,
        LoadingEnd = 16, Shapeshift = 17, VehicleEnter = 18, VehicleExit = 19, TransportBoard = 20,
        TransportLeave = 21,
    };

    /// FORMAT §2.4 `target_kind` (also a snapshot Unit's `kind`).
    enum class TargetKind : uint8_t
    {
        None = 0, Self = 1, FriendlyPlayer = 2, FriendlyCreature = 3, HostilePlayer = 4, HostileCreature = 5,
        Neutral = 6, Ground = 7, GameObject = 8,
    };

    /// FORMAT §2.4 Interact `what`.
    enum class Interaction : uint8_t
    {
        Gossip = 1, Loot = 2, QuestAccept = 3, QuestTurnIn = 4, GameObjectUse = 5, Vendor = 6, FlightMaster = 7,
        Mailbox = 8, AuctionHouse = 9, Trainer = 10, Bank = 11, Innkeeper = 12,
    };

    /// A record writer over a caller's buffer. Fails (and stays failed) rather than write past its end.
    class Out
    {
    public:
        Out(uint8_t* data, std::size_t capacity) : _data(data), _capacity(capacity) { }

        template <typename T>
        void Put(T value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (!Room(sizeof(T)))
                return;
            std::memcpy(_data + _size, &value, sizeof(T));
            _size += sizeof(T);
        }

        void PutBytes(void const* bytes, std::size_t count)
        {
            if (!Room(count))
                return;
            std::memcpy(_data + _size, bytes, count);
            _size += count;
        }

        /// `text` in a fixed `N` bytes: cut, or padded with zeros.
        template <std::size_t N>
        void PutChars(std::string_view text)
        {
            if (!Room(N))
                return;
            std::size_t const used = text.size() < N ? text.size() : N;
            std::memcpy(_data + _size, text.data(), used);
            std::memset(_data + _size + used, 0, N - used);
            _size += N;
        }

        /// Overwrite a value written earlier (a count or a length) at `offset`.
        template <typename T>
        void PutAt(std::size_t offset, T value)
        {
            if (!_ok || offset + sizeof(T) > _size)
            {
                _ok = false;
                return;
            }
            std::memcpy(_data + offset, &value, sizeof(T));
        }

        /// Open a record: its type and a length patched by End. Returns where the record starts.
        std::size_t Begin(Type type)
        {
            std::size_t const start = _size;
            Put<uint16_t>(uint16_t(type));
            Put<uint16_t>(0);
            return start;
        }

        /// Close the record opened at `start`; a payload past 65535 bytes fails the writer.
        void End(std::size_t start)
        {
            if (!_ok)
                return;
            std::size_t const payload = _size - start - 4;
            if (payload > 0xFFFF)
            {
                _ok = false;
                return;
            }
            PutAt<uint16_t>(start + 2, uint16_t(payload));
        }

        [[nodiscard]] bool Ok() const { return _ok; }
        [[nodiscard]] std::size_t Size() const { return _size; }
        [[nodiscard]] std::size_t Remaining() const { return _capacity - _size; }
        [[nodiscard]] uint8_t const* Data() const { return _data; }
        void Reset() { _size = 0; _ok = true; }

    private:
        bool Room(std::size_t count)
        {
            if (_ok && _size + count <= _capacity)
                return true;
            _ok = false;
            return false;
        }

        uint8_t* _data;
        std::size_t _capacity;
        std::size_t _size = 0;
        bool _ok = true;
    };

    // ---- §2.1 every file ------------------------------------------------------------------------------------------

    constexpr char MAGIC[8] = { 'A', 'N', 'C', 'A', 'P', 0, 0, 0 };

    struct FileHeader
    {
        uint16_t Format = FORMAT_VERSION;
        Stream StreamId = Stream::Session;
        uint64_t OpenedMs = 0;
        std::string_view ModuleRevision;
        std::string_view RealmBuild;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::FileHeader);
            out.PutBytes(MAGIC, sizeof(MAGIC));
            out.Put<uint16_t>(Format);
            out.Put<uint16_t>(uint16_t(StreamId));
            out.Put<uint64_t>(OpenedMs);
            out.PutChars<40>(ModuleRevision);
            out.PutChars<40>(RealmBuild);
            out.End(start);
        }
    };

    // ---- §2.2 session ---------------------------------------------------------------------------------------------

    struct SessionStart
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Session = 0;
        uint8_t Class = 0;
        uint8_t Race = 0;
        uint8_t Gender = 0;
        uint8_t Level = 0;
        std::array<uint8_t, 3> TreePoints{};
        uint16_t ItemLevel = 0;
        uint32_t Map = 0;
        uint32_t Zone = 0;
        uint32_t Area = 0;
        uint16_t LatencyMs = 0;
        uint32_t ClientBuild = 0;
        uint8_t Kind = 0;               // 0 human player, 1 Animus companion

        /// SessionStart, or SessionContext (the same payload) when `context`.
        void Write(Out& out, bool context = false) const
        {
            std::size_t const start = out.Begin(context ? Type::SessionContext : Type::SessionStart);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Session);
            out.Put<uint8_t>(Class);
            out.Put<uint8_t>(Race);
            out.Put<uint8_t>(Gender);
            out.Put<uint8_t>(Level);
            out.PutBytes(TreePoints.data(), TreePoints.size());
            out.Put<uint16_t>(ItemLevel);
            out.Put<uint32_t>(Map);
            out.Put<uint32_t>(Zone);
            out.Put<uint32_t>(Area);
            out.Put<uint16_t>(LatencyMs);
            out.Put<uint32_t>(ClientBuild);
            out.Put<uint8_t>(Kind);
            out.End(start);
        }
    };

    struct SessionEnd
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Session = 0;
        uint8_t Reason = 0;             // 0 logout, 1 disconnect, 2 shutdown

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::SessionEnd);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Session);
            out.Put<uint8_t>(Reason);
            out.End(start);
        }
    };

    struct GroupMember
    {
        uint64_t Unit = 0;
        uint8_t Class = 0;
        uint8_t Level = 0;
        uint8_t Role = 3;               // 0 dps, 1 tank, 2 heal, 3 unknown
        uint8_t IsCompanion = 0;
    };

    struct GroupState
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint8_t Kind = 0;               // 0 solo, 1 party, 2 raid
        GroupMember const* Members = nullptr;
        uint8_t Count = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::GroupState);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint8_t>(Kind);
            out.Put<uint8_t>(Count);
            for (uint8_t i = 0; i < Count; ++i)
            {
                out.Put<uint64_t>(Members[i].Unit);
                out.Put<uint8_t>(Members[i].Class);
                out.Put<uint8_t>(Members[i].Level);
                out.Put<uint8_t>(Members[i].Role);
                out.Put<uint8_t>(Members[i].IsCompanion);
            }
            out.End(start);
        }
    };

    struct KnownSpells
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint32_t const* Spells = nullptr;
        uint16_t Count = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::KnownSpells);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint16_t>(Count);
            out.PutBytes(Spells, std::size_t(Count) * sizeof(uint32_t));
            out.End(start);
        }
    };

    struct Latency
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint16_t LatencyMs = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Latency);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint16_t>(LatencyMs);
            out.End(start);
        }
    };

    /// Format 4, a recording's: what the player carries and wears -- every item of the equipment, the backpack, the
    /// bags and the bag slots themselves -- at the recording's start and, at most once a second, after a change.
    struct InventoryItem
    {
        uint8_t Bag = 0;                // INVENTORY_SLOT_BAG_0 (255) for the equipment and the backpack
        uint8_t Slot = 0;
        uint32_t Entry = 0;
        uint32_t Count = 0;
        uint32_t PermEnchant = 0;
        uint32_t TempEnchant = 0;
        int32_t RandomProperty = 0;
        uint32_t Durability = 0;
        uint32_t MaxDurability = 0;
    };

    struct Inventory
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        InventoryItem const* Items = nullptr;
        uint16_t Count = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Inventory);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint16_t>(Count);
            for (uint16_t i = 0; i < Count; ++i)
            {
                out.Put<uint8_t>(Items[i].Bag);
                out.Put<uint8_t>(Items[i].Slot);
                out.Put<uint32_t>(Items[i].Entry);
                out.Put<uint32_t>(Items[i].Count);
                out.Put<uint32_t>(Items[i].PermEnchant);
                out.Put<uint32_t>(Items[i].TempEnchant);
                out.Put<int32_t>(Items[i].RandomProperty);
                out.Put<uint32_t>(Items[i].Durability);
                out.Put<uint32_t>(Items[i].MaxDurability);
            }
            out.End(start);
        }
    };

    /// Format 4, a recording's: the action bars (every button that holds something), at the start and on change.
    struct ActionButton
    {
        uint8_t Button = 0;
        uint8_t Kind = 0;               // ActionButtonType: 0 spell, 64 class, 128 item, 192 macro ...
        uint32_t Action = 0;
    };

    struct ActionButtons
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        ActionButton const* Buttons = nullptr;
        uint8_t Count = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::ActionButtons);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint8_t>(Count);
            for (uint8_t i = 0; i < Count; ++i)
            {
                out.Put<uint8_t>(Buttons[i].Button);
                out.Put<uint8_t>(Buttons[i].Kind);
                out.Put<uint32_t>(Buttons[i].Action);
            }
            out.End(start);
        }
    };

    /// Format 4, a recording's: the active spec's talents (id and rank) and glyphs, at the start and on change.
    struct TalentRank
    {
        uint32_t Talent = 0;
        uint8_t Rank = 0;
    };

    struct Talents
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint8_t ActiveSpec = 0;
        TalentRank const* Ranks = nullptr;
        uint8_t Count = 0;
        uint32_t const* Glyphs = nullptr;
        uint8_t GlyphCount = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Talents);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint8_t>(ActiveSpec);
            out.Put<uint8_t>(Count);
            for (uint8_t i = 0; i < Count; ++i)
            {
                out.Put<uint32_t>(Ranks[i].Talent);
                out.Put<uint8_t>(Ranks[i].Rank);
            }
            out.Put<uint8_t>(GlyphCount);
            out.PutBytes(Glyphs, std::size_t(GlyphCount) * sizeof(uint32_t));
            out.End(start);
        }
    };

    // ---- §2.3 move ------------------------------------------------------------------------------------------------

    /// FORMAT §2.3 Move `source`.
    enum class MoveSource : uint8_t
    {
        ClientPacket = 0,           // a player's client
        CompanionSample = 1,        // format 1 only: a companion's server position once a decision (retired)
        ControllerPacket = 2,       // a companion's player controller, through its session's movement handlers
    };

    struct Move
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint32_t ClientMs = 0;
        uint16_t Opcode = 0;
        uint32_t MoveFlags = 0;
        uint16_t MoveFlags2 = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        float Pitch = 0.0f;
        uint32_t FallMs = 0;
        float JumpZSpeed = 0.0f;
        float JumpSin = 0.0f;
        float JumpCos = 0.0f;
        float JumpXYSpeed = 0.0f;
        uint32_t Map = 0;
        uint8_t Source = 0;             // MoveSource
        uint32_t ServerMs = 0;          // getMSTime() when the server's handler took it (format 3)

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Move);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint32_t>(ClientMs);
            out.Put<uint16_t>(Opcode);
            out.Put<uint32_t>(MoveFlags);
            out.Put<uint16_t>(MoveFlags2);
            out.Put<float>(X);
            out.Put<float>(Y);
            out.Put<float>(Z);
            out.Put<float>(O);
            out.Put<float>(Pitch);
            out.Put<uint32_t>(FallMs);
            out.Put<float>(JumpZSpeed);
            out.Put<float>(JumpSin);
            out.Put<float>(JumpCos);
            out.Put<float>(JumpXYSpeed);
            out.Put<uint32_t>(Map);
            out.Put<uint8_t>(Source);
            out.Put<uint32_t>(ServerMs);
            out.End(start);
        }
    };

    struct Speeds
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        float Walk = 0.0f;
        float Run = 0.0f;
        float RunBack = 0.0f;
        float Swim = 0.0f;
        float SwimBack = 0.0f;
        float Flight = 0.0f;
        float FlightBack = 0.0f;
        float TurnRate = 0.0f;
        float PitchRate = 0.0f;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Speeds);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<float>(Walk);
            out.Put<float>(Run);
            out.Put<float>(RunBack);
            out.Put<float>(Swim);
            out.Put<float>(SwimBack);
            out.Put<float>(Flight);
            out.Put<float>(FlightBack);
            out.Put<float>(TurnRate);
            out.Put<float>(PitchRate);
            out.End(start);
        }
    };

    struct MotionEvent
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        Motion Event = Motion::Mount;
        uint32_t Arg = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        uint32_t Map = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::MotionEvent);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint8_t>(uint8_t(Event));
            out.Put<uint32_t>(Arg);
            out.Put<float>(X);
            out.Put<float>(Y);
            out.Put<float>(Z);
            out.Put<uint32_t>(Map);
            out.End(start);
        }
    };

    /// Who is moving and under what (FORMAT §2.3 MoverState): written at a mover's first update and whenever any
    /// field changes, for players and companions alike, so motion is compared like with like.
    struct MoverState
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint8_t Kind = 0;               // 0 human player, 1 Animus companion
        uint8_t Class = 0;
        uint8_t Race = 0;
        uint8_t Level = 0;
        uint32_t Map = 0;
        uint32_t Zone = 0;
        uint32_t Mount = 0;             // the mount aura's spell, 0 on foot
        uint32_t Form = 0;              // ShapeshiftForm, 0 none
        uint8_t InCombat = 0;
        uint8_t MoveRevision = 0;       // a companion model's move block revision; 0 for a player
        std::string_view Model;         // a companion's model (char[32]); empty for a player

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::MoverState);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint8_t>(Kind);
            out.Put<uint8_t>(Class);
            out.Put<uint8_t>(Race);
            out.Put<uint8_t>(Level);
            out.Put<uint32_t>(Map);
            out.Put<uint32_t>(Zone);
            out.Put<uint32_t>(Mount);
            out.Put<uint32_t>(Form);
            out.Put<uint8_t>(InCombat);
            out.Put<uint8_t>(MoveRevision);
            out.PutChars<32>(Model);
            out.End(start);
        }
    };

    /// A mover's movement packets since its session began (FORMAT §2.3 MoveTally): those that reached the server's
    /// movement handlers (`sent`), and those the handlers kept and the move stream recorded (`kept`). The difference
    /// is what the server refused or ignored (a spline under way, movement disabled, a teleport pending, an ack's
    /// pre-check). Cumulative; written every minute while it changes, and at the session's end.
    struct MoveTally
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint8_t Kind = 0;               // 0 human player, 1 Animus companion
        uint32_t Sent = 0;
        uint32_t Kept = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::MoveTally);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint8_t>(Kind);
            out.Put<uint32_t>(Sent);
            out.Put<uint32_t>(Kept);
            out.End(start);
        }
    };

    /// One update of a map instance holding at least one captured mover (FORMAT §2.3 MapUpdate): the server's unix
    /// time, the map and instance, and the diff its movers were updated with that tick (Player::Update's, which a
    /// companion's controller tick also gets).
    struct MapUpdate
    {
        uint64_t Ms = 0;
        uint32_t Map = 0;
        uint32_t Instance = 0;
        uint32_t DiffMs = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::MapUpdate);
            out.Put<uint64_t>(Ms);
            out.Put<uint32_t>(Map);
            out.Put<uint32_t>(Instance);
            out.Put<uint32_t>(DiffMs);
            out.End(start);
        }
    };

    // ---- §2.4 action ----------------------------------------------------------------------------------------------

    struct CastRequest
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint32_t Spell = 0;
        uint64_t Target = 0;
        TargetKind Kind = TargetKind::None;
        float TX = 0.0f;
        float TY = 0.0f;
        float TZ = 0.0f;
        uint8_t GcdActive = 0;
        uint8_t Casting = 0;
        uint32_t Power = 0;
        uint8_t PowerType = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::CastRequest);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint32_t>(Spell);
            out.Put<uint64_t>(Target);
            out.Put<uint8_t>(uint8_t(Kind));
            out.Put<float>(TX);
            out.Put<float>(TY);
            out.Put<float>(TZ);
            out.Put<uint8_t>(GcdActive);
            out.Put<uint8_t>(Casting);
            out.Put<uint32_t>(Power);
            out.Put<uint8_t>(PowerType);
            out.End(start);
        }
    };

    constexpr uint8_t CAST_GO = 255;

    /// CastResult (`value` the SpellCastResult, CAST_GO for a cast that went off) and CastEnd (`value` how it
    /// ended: 0 completed, 1 cancelled by moving, 2 cancelled other, 3 interrupted) share a payload shape.
    struct CastOutcome
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint32_t Spell = 0;
        uint8_t Value = 0;

        void Write(Out& out, Type type) const
        {
            std::size_t const start = out.Begin(type);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint32_t>(Spell);
            out.Put<uint8_t>(Value);
            out.End(start);
        }
    };

    struct Select
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Target = 0;
        TargetKind Kind = TargetKind::None;
        float Distance = 0.0f;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Select);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Target);
            out.Put<uint8_t>(uint8_t(Kind));
            out.Put<float>(Distance);
            out.End(start);
        }
    };

    struct ItemUse
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint32_t Item = 0;
        uint32_t Spell = 0;
        uint64_t Target = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::ItemUse);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint32_t>(Item);
            out.Put<uint32_t>(Spell);
            out.Put<uint64_t>(Target);
            out.End(start);
        }
    };

    struct Attack
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Target = 0;
        uint8_t Start = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Attack);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Target);
            out.Put<uint8_t>(Start);
            out.End(start);
        }
    };

    struct Interact
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        Interaction What = Interaction::Gossip;
        uint32_t Entry = 0;
        uint64_t Target = 0;
        uint32_t Arg = 0;
        uint32_t SpawnId = 0;           // format 4: the target's world spawn (a door, a lever, the cannon)

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Interact);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint8_t>(uint8_t(What));
            out.Put<uint32_t>(Entry);
            out.Put<uint64_t>(Target);
            out.Put<uint32_t>(Arg);
            out.Put<uint32_t>(SpawnId);
            out.End(start);
        }
    };

    // ---- §2.5 snapshot --------------------------------------------------------------------------------------------

    struct SnapshotSelf
    {
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        float Pitch = 0.0f;
        uint32_t Map = 0;
        float HealthPct = 0.0f;
        float PowerPct = 0.0f;
        uint8_t PowerType = 0;
        uint8_t Flags = 0;              // 1 combat, 2 swimming, 4 flying, 8 mounted, 16 casting, 32 dead, 64 falling
        uint32_t CastingSpell = 0;
        float BreathPct = 0.0f;
        uint64_t Target = 0;
        uint32_t ShapeshiftForm = 0;
    };

    struct SnapshotUnit
    {
        uint64_t Unit = 0;
        uint32_t Entry = 0;
        TargetKind Kind = TargetKind::None;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        float VX = 0.0f;
        float VY = 0.0f;
        float VZ = 0.0f;
        float HealthPct = 0.0f;
        float PowerPct = 0.0f;
        uint8_t Level = 0;
        uint8_t Reaction = 0;           // 0 hostile, 1 neutral, 2 friendly
        uint8_t Flags = 0;              // 1 combat, 2 casting, 4 party member, 8 pet of the player, 16 companion
        uint32_t CastingSpell = 0;
        uint64_t Target = 0;
        float ThreatOnPlayer = 0.0f;
    };

    struct SnapshotAura
    {
        uint32_t Spell = 0;
        uint8_t Stacks = 0;
        int32_t RemainingMs = -1;       // -1 permanent
        uint8_t Positive = 0;
    };

    struct SnapshotCooldown
    {
        uint32_t Spell = 0;
        uint32_t RemainingMs = 0;
    };

    /// A Snapshot written in three runs (units, auras, cooldowns), each count patched when its run ends; at most
    /// 255 of each (more are dropped).
    class SnapshotBuilder
    {
    public:
        SnapshotBuilder(Out& out, uint64_t ms, uint64_t player, SnapshotSelf const& self) : _out(out)
        {
            _start = out.Begin(Type::Snapshot);
            out.Put<uint64_t>(ms);
            out.Put<uint64_t>(player);
            out.Put<float>(self.X);
            out.Put<float>(self.Y);
            out.Put<float>(self.Z);
            out.Put<float>(self.O);
            out.Put<float>(self.Pitch);
            out.Put<uint32_t>(self.Map);
            out.Put<float>(self.HealthPct);
            out.Put<float>(self.PowerPct);
            out.Put<uint8_t>(self.PowerType);
            out.Put<uint8_t>(self.Flags);
            out.Put<uint32_t>(self.CastingSpell);
            out.Put<float>(self.BreathPct);
            out.Put<uint64_t>(self.Target);
            out.Put<uint32_t>(self.ShapeshiftForm);
            OpenRun();
        }

        bool Unit(SnapshotUnit const& unit)
        {
            if (_phase != 0 || _count == 255)
                return false;
            _out.Put<uint64_t>(unit.Unit);
            _out.Put<uint32_t>(unit.Entry);
            _out.Put<uint8_t>(uint8_t(unit.Kind));
            _out.Put<float>(unit.X);
            _out.Put<float>(unit.Y);
            _out.Put<float>(unit.Z);
            _out.Put<float>(unit.O);
            _out.Put<float>(unit.VX);
            _out.Put<float>(unit.VY);
            _out.Put<float>(unit.VZ);
            _out.Put<float>(unit.HealthPct);
            _out.Put<float>(unit.PowerPct);
            _out.Put<uint8_t>(unit.Level);
            _out.Put<uint8_t>(unit.Reaction);
            _out.Put<uint8_t>(unit.Flags);
            _out.Put<uint32_t>(unit.CastingSpell);
            _out.Put<uint64_t>(unit.Target);
            _out.Put<float>(unit.ThreatOnPlayer);
            ++_count;
            return true;
        }

        bool Aura(SnapshotAura const& aura)
        {
            Advance(1);
            if (_phase != 1 || _count == 255)
                return false;
            _out.Put<uint32_t>(aura.Spell);
            _out.Put<uint8_t>(aura.Stacks);
            _out.Put<int32_t>(aura.RemainingMs);
            _out.Put<uint8_t>(aura.Positive);
            ++_count;
            return true;
        }

        bool Cooldown(SnapshotCooldown const& cooldown)
        {
            Advance(2);
            if (_phase != 2 || _count == 255)
                return false;
            _out.Put<uint32_t>(cooldown.Spell);
            _out.Put<uint32_t>(cooldown.RemainingMs);
            ++_count;
            return true;
        }

        /// Close every run still open and the record.
        void Finish()
        {
            Advance(3);
            _out.End(_start);
        }

    private:
        void OpenRun()
        {
            _countAt = _out.Size();
            _out.Put<uint8_t>(0);
            _count = 0;
        }

        void Advance(uint32_t phase)
        {
            while (_phase < phase)
            {
                _out.PutAt<uint8_t>(_countAt, _count);
                ++_phase;
                if (_phase < 3)
                    OpenRun();
            }
        }

        Out& _out;
        std::size_t _start = 0;
        std::size_t _countAt = 0;
        uint8_t _count = 0;
        uint32_t _phase = 0;
    };

    // ---- §2.5 snapshot, format 4: a recording's surroundings ------------------------------------------------------

    /// One unit the recording player's client can see, as the server has it this tick (FORMAT §2.5 UnitState): every
    /// field a tool could want to work the fight out from, its auras and its full threat list. One record per unit
    /// per Recording.UnitMs, the nearest Recording.MaxUnits within Recording.UnitRange.
    struct UnitStateAura
    {
        uint32_t Spell = 0;
        uint8_t Stacks = 0;
        int32_t RemainingMs = -1;
        uint8_t Positive = 0;
        uint64_t Caster = 0;
    };

    struct UnitStateThreat
    {
        uint64_t Unit = 0;
        float Threat = 0.0f;
    };

    struct UnitState
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Unit = 0;
        uint32_t SpawnId = 0;
        uint32_t Entry = 0;
        uint8_t UnitType = 0;           // 0 creature, 1 player, 2 pet or guardian
        TargetKind Kind = TargetKind::None;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        float VX = 0.0f;
        float VY = 0.0f;
        float VZ = 0.0f;
        uint32_t MoveFlags = 0;
        uint32_t Health = 0;
        uint32_t MaxHealth = 0;
        uint8_t PowerType = 0;
        uint32_t Power = 0;
        uint32_t MaxPower = 0;
        uint8_t Level = 0;
        uint8_t Reaction = 0;           // 0 hostile, 1 neutral, 2 friendly
        uint8_t ReactState = 0;         // Creature::GetReactState (0 passive, 1 defensive, 2 aggressive)
        uint8_t Rank = 0;               // CreatureTemplate::rank
        uint8_t Boss = 0;               // CREATURE_FLAG_EXTRA_DUNGEON_BOSS
        uint32_t UnitFlags = 0;
        uint32_t NpcFlags = 0;
        uint32_t DynamicFlags = 0;
        uint32_t UnitStateFlags = 0;    // UNIT_STATE_*
        uint8_t DeathState = 0;
        uint64_t Target = 0;
        uint64_t Victim = 0;
        uint32_t CastingSpell = 0;
        int32_t CastLeftMs = 0;
        uint8_t MotionType = 0;         // the top movement generator's type
        float HomeX = 0.0f;
        float HomeY = 0.0f;
        float HomeZ = 0.0f;
        uint32_t FormationLeaderSpawn = 0;
        uint8_t Flags = 0;              // 1 combat, 2 evading, 4 party, 8 my pet, 16 companion, 32 engaged with my
                                        // group, 64 tapped by me
        UnitStateAura const* Auras = nullptr;
        uint8_t AuraCount = 0;
        UnitStateThreat const* Threats = nullptr;
        uint8_t ThreatCount = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::UnitState);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Unit);
            out.Put<uint32_t>(SpawnId);
            out.Put<uint32_t>(Entry);
            out.Put<uint8_t>(UnitType);
            out.Put<uint8_t>(uint8_t(Kind));
            out.Put<float>(X);
            out.Put<float>(Y);
            out.Put<float>(Z);
            out.Put<float>(O);
            out.Put<float>(VX);
            out.Put<float>(VY);
            out.Put<float>(VZ);
            out.Put<uint32_t>(MoveFlags);
            out.Put<uint32_t>(Health);
            out.Put<uint32_t>(MaxHealth);
            out.Put<uint8_t>(PowerType);
            out.Put<uint32_t>(Power);
            out.Put<uint32_t>(MaxPower);
            out.Put<uint8_t>(Level);
            out.Put<uint8_t>(Reaction);
            out.Put<uint8_t>(ReactState);
            out.Put<uint8_t>(Rank);
            out.Put<uint8_t>(Boss);
            out.Put<uint32_t>(UnitFlags);
            out.Put<uint32_t>(NpcFlags);
            out.Put<uint32_t>(DynamicFlags);
            out.Put<uint32_t>(UnitStateFlags);
            out.Put<uint8_t>(DeathState);
            out.Put<uint64_t>(Target);
            out.Put<uint64_t>(Victim);
            out.Put<uint32_t>(CastingSpell);
            out.Put<int32_t>(CastLeftMs);
            out.Put<uint8_t>(MotionType);
            out.Put<float>(HomeX);
            out.Put<float>(HomeY);
            out.Put<float>(HomeZ);
            out.Put<uint32_t>(FormationLeaderSpawn);
            out.Put<uint8_t>(Flags);
            out.Put<uint8_t>(AuraCount);
            for (uint8_t i = 0; i < AuraCount; ++i)
            {
                out.Put<uint32_t>(Auras[i].Spell);
                out.Put<uint8_t>(Auras[i].Stacks);
                out.Put<int32_t>(Auras[i].RemainingMs);
                out.Put<uint8_t>(Auras[i].Positive);
                out.Put<uint64_t>(Auras[i].Caster);
            }
            out.Put<uint8_t>(ThreatCount);
            for (uint8_t i = 0; i < ThreatCount; ++i)
            {
                out.Put<uint64_t>(Threats[i].Unit);
                out.Put<float>(Threats[i].Threat);
            }
            out.End(start);
        }
    };

    /// A game object the recording player's client can see (FORMAT §2.5 GameObjectState): every one at the
    /// recording's start and at each map entry (`event` 0 seen), then each one whose state changed (1) or went (2).
    struct GameObjectState
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Object = 0;
        uint32_t SpawnId = 0;
        uint32_t Entry = 0;
        uint8_t GoType = 0;
        uint8_t GoState = 0;            // GOState: 0 active (a door open), 1 ready (shut), 2 active alternative
        uint8_t LootState = 0;
        uint32_t Flags = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        uint8_t Event = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::GameObjectState);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Object);
            out.Put<uint32_t>(SpawnId);
            out.Put<uint32_t>(Entry);
            out.Put<uint8_t>(GoType);
            out.Put<uint8_t>(GoState);
            out.Put<uint8_t>(LootState);
            out.Put<uint32_t>(Flags);
            out.Put<float>(X);
            out.Put<float>(Y);
            out.Put<float>(Z);
            out.Put<float>(O);
            out.Put<uint8_t>(Event);
            out.End(start);
        }
    };

    /// The recording player's own numbers (FORMAT §2.5 PlayerDetail), once a second and on change.
    struct PlayerDetail
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        std::array<uint32_t, 5> Stats{};        // strength, agility, stamina, intellect, spirit
        uint32_t Armor = 0;
        std::array<uint32_t, 6> Resistances{};  // holy, fire, nature, frost, shadow, arcane
        uint32_t AttackPower = 0;
        uint32_t RangedAttackPower = 0;
        uint32_t SpellPower = 0;
        uint32_t HealPower = 0;
        std::array<float, 11> Ratings{};        // crit melee/ranged/spell, haste melee/ranged/spell, hit melee/spell,
                                                // dodge, parry, block (percent)
        float Expertise = 0.0f;
        uint32_t MaxHealth = 0;
        std::array<uint32_t, 7> MaxPower{};
        uint32_t Money = 0;
        uint32_t Xp = 0;
        uint32_t NextXp = 0;
        uint8_t ComboPoints = 0;
        uint64_t ComboTarget = 0;
        uint32_t CheatFlags = 0;                // PlayerCommandStates: 1 god, 2 casttime, 4 cooldown, 8 power, ...
        uint8_t GameMaster = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::PlayerDetail);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            for (uint32_t stat : Stats)
                out.Put<uint32_t>(stat);
            out.Put<uint32_t>(Armor);
            for (uint32_t resistance : Resistances)
                out.Put<uint32_t>(resistance);
            out.Put<uint32_t>(AttackPower);
            out.Put<uint32_t>(RangedAttackPower);
            out.Put<uint32_t>(SpellPower);
            out.Put<uint32_t>(HealPower);
            for (float rating : Ratings)
                out.Put<float>(rating);
            out.Put<float>(Expertise);
            out.Put<uint32_t>(MaxHealth);
            for (uint32_t power : MaxPower)
                out.Put<uint32_t>(power);
            out.Put<uint32_t>(Money);
            out.Put<uint32_t>(Xp);
            out.Put<uint32_t>(NextXp);
            out.Put<uint8_t>(ComboPoints);
            out.Put<uint64_t>(ComboTarget);
            out.Put<uint32_t>(CheatFlags);
            out.Put<uint8_t>(GameMaster);
            out.End(start);
        }
    };

    /// The map's spawn table (FORMAT §2.5 MapSpawns), creatures and game objects, so a tool never needs the world
    /// database: written once per map a recording enters, in parts of at most MAP_SPAWNS_PART entries.
    constexpr uint16_t MAP_SPAWNS_PART = 1500;

    struct MapSpawn
    {
        uint32_t SpawnId = 0;
        uint32_t Entry = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        uint8_t Rank = 0;
        uint8_t Boss = 0;
        uint32_t FormationLeaderSpawn = 0;
        uint8_t Kind = 0;               // 0 creature, 1 game object
        uint8_t GoType = 0;
    };

    struct MapSpawns
    {
        uint64_t Ms = 0;
        uint32_t Map = 0;
        uint16_t Part = 0;
        uint16_t Parts = 1;
        MapSpawn const* Spawns = nullptr;
        uint16_t Count = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::MapSpawns);
            out.Put<uint64_t>(Ms);
            out.Put<uint32_t>(Map);
            out.Put<uint16_t>(Part);
            out.Put<uint16_t>(Parts);
            out.Put<uint16_t>(Count);
            for (uint16_t i = 0; i < Count; ++i)
            {
                out.Put<uint32_t>(Spawns[i].SpawnId);
                out.Put<uint32_t>(Spawns[i].Entry);
                out.Put<float>(Spawns[i].X);
                out.Put<float>(Spawns[i].Y);
                out.Put<float>(Spawns[i].Z);
                out.Put<float>(Spawns[i].O);
                out.Put<uint8_t>(Spawns[i].Rank);
                out.Put<uint8_t>(Spawns[i].Boss);
                out.Put<uint32_t>(Spawns[i].FormationLeaderSpawn);
                out.Put<uint8_t>(Spawns[i].Kind);
                out.Put<uint8_t>(Spawns[i].GoType);
            }
            out.End(start);
        }
    };

    /// Which of a map instance's spawned creatures are alive (FORMAT §2.5 InstanceCreatures): at the recording's
    /// start, at each map entry and at its end, in parts of at most INSTANCE_CREATURES_PART.
    constexpr uint16_t INSTANCE_CREATURES_PART = 10000;

    struct InstanceCreature
    {
        uint32_t SpawnId = 0;
        uint8_t Alive = 0;
    };

    struct InstanceCreatures
    {
        uint64_t Ms = 0;
        uint32_t Map = 0;
        uint32_t Instance = 0;
        uint16_t Part = 0;
        uint16_t Parts = 1;
        InstanceCreature const* Creatures = nullptr;
        uint16_t Count = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::InstanceCreatures);
            out.Put<uint64_t>(Ms);
            out.Put<uint32_t>(Map);
            out.Put<uint32_t>(Instance);
            out.Put<uint16_t>(Part);
            out.Put<uint16_t>(Parts);
            out.Put<uint16_t>(Count);
            for (uint16_t i = 0; i < Count; ++i)
            {
                out.Put<uint32_t>(Creatures[i].SpawnId);
                out.Put<uint8_t>(Creatures[i].Alive);
            }
            out.End(start);
        }
    };

    // ---- §2.6 outcome ---------------------------------------------------------------------------------------------

    struct Damage
    {
        uint64_t Ms = 0;
        uint64_t Source = 0;
        uint64_t Target = 0;
        uint32_t Spell = 0;             // 0 melee
        uint32_t Amount = 0;
        uint32_t Absorbed = 0;
        uint32_t Overkill = 0;
        uint8_t School = 0;
        uint8_t Flags = 0;              // 1 crit, 2 periodic

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Damage);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Source);
            out.Put<uint64_t>(Target);
            out.Put<uint32_t>(Spell);
            out.Put<uint32_t>(Amount);
            out.Put<uint32_t>(Absorbed);
            out.Put<uint32_t>(Overkill);
            out.Put<uint8_t>(School);
            out.Put<uint8_t>(Flags);
            out.End(start);
        }
    };

    struct Heal
    {
        uint64_t Ms = 0;
        uint64_t Source = 0;
        uint64_t Target = 0;
        uint32_t Spell = 0;
        uint32_t Amount = 0;
        uint32_t Overheal = 0;
        uint8_t Flags = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Heal);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Source);
            out.Put<uint64_t>(Target);
            out.Put<uint32_t>(Spell);
            out.Put<uint32_t>(Amount);
            out.Put<uint32_t>(Overheal);
            out.Put<uint8_t>(Flags);
            out.End(start);
        }
    };

    struct Kill
    {
        uint64_t Ms = 0;
        uint64_t Killer = 0;
        uint64_t Victim = 0;
        uint32_t VictimEntry = 0;
        TargetKind VictimKind = TargetKind::None;
        uint32_t VictimSpawnId = 0;     // format 4: the victim's world spawn (0 a player, a pet or a summon)

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Kill);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Killer);
            out.Put<uint64_t>(Victim);
            out.Put<uint32_t>(VictimEntry);
            out.Put<uint8_t>(uint8_t(VictimKind));
            out.Put<uint32_t>(VictimSpawnId);
            out.End(start);
        }
    };

    struct Death
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Killer = 0;
        uint8_t Cause = 5;              // 0 creature, 1 player, 2 fall, 3 drowning, 4 fire/lava, 5 other
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        uint32_t Map = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Death);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Killer);
            out.Put<uint8_t>(Cause);
            out.Put<float>(X);
            out.Put<float>(Y);
            out.Put<float>(Z);
            out.Put<uint32_t>(Map);
            out.End(start);
        }
    };

    struct Quest
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint32_t QuestId = 0;
        uint8_t Event = 0;              // 0 accepted, 1 completed, 2 rewarded, 3 abandoned

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Quest);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint32_t>(QuestId);
            out.Put<uint8_t>(Event);
            out.End(start);
        }
    };

    struct Encounter
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint32_t Map = 0;
        uint32_t Instance = 0;
        uint32_t BossEntry = 0;
        uint8_t Event = 0;              // 0 engaged, 1 killed, 2 wipe

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Encounter);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint32_t>(Map);
            out.Put<uint32_t>(Instance);
            out.Put<uint32_t>(BossEntry);
            out.Put<uint8_t>(Event);
            out.End(start);
        }
    };

    struct PvP
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Other = 0;
        uint8_t Event = 0;              // 0 kill, 1 death, 2 duel won, 3 duel lost

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::PvP);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Other);
            out.Put<uint8_t>(Event);
            out.End(start);
        }
    };

    struct Area
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint32_t Map = 0;
        uint32_t Zone = 0;
        uint32_t AreaId = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Area);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint32_t>(Map);
            out.Put<uint32_t>(Zone);
            out.Put<uint32_t>(AreaId);
            out.End(start);
        }
    };

    /// A unit entering, leaving or evading a fight on a recording's map (FORMAT §2.6 CombatEdge, format 4): the
    /// unit and the other side when the hook knows it, both positions. The first creature engaged of a pull.
    struct CombatEdge
    {
        uint64_t Ms = 0;
        uint32_t Map = 0;
        uint32_t Instance = 0;
        uint64_t Unit = 0;
        uint32_t SpawnId = 0;
        uint32_t Entry = 0;
        uint8_t Event = 0;              // 0 enter, 1 exit, 2 evade
        uint8_t EvadeReason = 0;
        uint64_t Other = 0;
        uint32_t OtherSpawnId = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float OX = 0.0f;
        float OY = 0.0f;
        float OZ = 0.0f;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::CombatEdge);
            out.Put<uint64_t>(Ms);
            out.Put<uint32_t>(Map);
            out.Put<uint32_t>(Instance);
            out.Put<uint64_t>(Unit);
            out.Put<uint32_t>(SpawnId);
            out.Put<uint32_t>(Entry);
            out.Put<uint8_t>(Event);
            out.Put<uint8_t>(EvadeReason);
            out.Put<uint64_t>(Other);
            out.Put<uint32_t>(OtherSpawnId);
            out.Put<float>(X);
            out.Put<float>(Y);
            out.Put<float>(Z);
            out.Put<float>(OX);
            out.Put<float>(OY);
            out.Put<float>(OZ);
            out.End(start);
        }
    };

    // ---- §2.7 companion -------------------------------------------------------------------------------------------

    struct CompanionDecision
    {
        uint64_t Ms = 0;
        uint64_t Companion = 0;
        uint64_t Owner = 0;
        std::string_view Model;
        uint32_t ObsHash = 0;
        uint16_t const* Actions = nullptr;
        uint16_t ActionCount = 0;
        uint16_t Goal = 0;
        uint16_t Goal2 = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::CompanionDecision);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Companion);
            out.Put<uint64_t>(Owner);
            out.PutChars<32>(Model);
            out.Put<uint32_t>(ObsHash);
            out.Put<uint16_t>(ActionCount);
            out.PutBytes(Actions, std::size_t(ActionCount) * sizeof(uint16_t));
            out.Put<uint16_t>(Goal);
            out.Put<uint16_t>(Goal2);
            out.End(start);
        }
    };

    struct CompanionCommand
    {
        uint64_t Ms = 0;
        uint64_t Owner = 0;
        uint64_t Companion = 0;
        uint8_t Command = 0;    // 1 summon, 2 dismiss, 3 follow, 4 assist, 5 guard, 6 stay, 7 other order
        uint32_t Arg = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::CompanionCommand);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Owner);
            out.Put<uint64_t>(Companion);
            out.Put<uint8_t>(Command);
            out.Put<uint32_t>(Arg);
            out.End(start);
        }
    };

    struct CompanionRating
    {
        uint64_t Ms = 0;
        uint64_t Owner = 0;
        uint64_t Companion = 0;
        int8_t Rating = 0;      // +1 / -1
        uint8_t Reason = 0;     // 0 none, 1 movement, 2 combat, 3 healing, 4 tanking, 5 stuck, 6 other

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::CompanionRating);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Owner);
            out.Put<uint64_t>(Companion);
            out.Put<int8_t>(Rating);
            out.Put<uint8_t>(Reason);
            out.End(start);
        }
    };

    // ---- §2.8 note (format 4, a recording's) ----------------------------------------------------------------------

    /// A free-text note the recording player typed (FORMAT §2.8 Annotation), with where they stood, what they
    /// targeted and the state of the moment.
    struct Annotation
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint32_t Seq = 0;
        uint16_t Kind = 1;              // 1 note
        uint32_t Map = 0;
        uint32_t Instance = 0;
        uint32_t Zone = 0;
        uint32_t Area = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        uint64_t Target = 0;
        uint32_t TargetSpawnId = 0;
        uint32_t TargetEntry = 0;
        TargetKind TargetKindOf = TargetKind::None;
        float TargetHealthPct = 0.0f;
        float TargetDistance = 0.0f;
        uint8_t State = 0;              // 1 combat, 2 dead, 4 mounted, 8 casting, 16 swimming, 32 falling, 64 god
        float HealthPct = 0.0f;
        float PowerPct = 0.0f;
        uint8_t PowerType = 0;
        uint8_t ThreatOnMe = 0;         // creatures with the player on their threat list
        uint8_t HostilesNear = 0;       // hostile units within 30 yards
        uint8_t PartyAlive = 0;
        uint8_t PartySize = 0;
        uint32_t ClientMs = 0;
        std::string_view Text;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Annotation);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint32_t>(Seq);
            out.Put<uint16_t>(Kind);
            out.Put<uint32_t>(Map);
            out.Put<uint32_t>(Instance);
            out.Put<uint32_t>(Zone);
            out.Put<uint32_t>(Area);
            out.Put<float>(X);
            out.Put<float>(Y);
            out.Put<float>(Z);
            out.Put<float>(O);
            out.Put<uint64_t>(Target);
            out.Put<uint32_t>(TargetSpawnId);
            out.Put<uint32_t>(TargetEntry);
            out.Put<uint8_t>(uint8_t(TargetKindOf));
            out.Put<float>(TargetHealthPct);
            out.Put<float>(TargetDistance);
            out.Put<uint8_t>(State);
            out.Put<float>(HealthPct);
            out.Put<float>(PowerPct);
            out.Put<uint8_t>(PowerType);
            out.Put<uint8_t>(ThreatOnMe);
            out.Put<uint8_t>(HostilesNear);
            out.Put<uint8_t>(PartyAlive);
            out.Put<uint8_t>(PartySize);
            out.Put<uint32_t>(ClientMs);
            std::size_t const length = std::min<std::size_t>(Text.size(), 0xFFFF);
            out.Put<uint16_t>(uint16_t(length));
            out.PutBytes(Text.data(), length);
            out.End(start);
        }
    };

    /// The first record of a recording (FORMAT §2.8 RecordingStart): who, where, and under what settings.
    struct RecordingStart
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Session = 0;
        std::string_view Id;            // char[64]: the recording's directory name
        uint8_t Class = 0;
        uint8_t Race = 0;
        uint8_t Gender = 0;
        uint8_t Level = 0;
        uint32_t Map = 0;
        uint32_t Instance = 0;
        uint32_t Zone = 0;
        uint32_t Area = 0;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
        float O = 0.0f;
        uint16_t SnapshotMs = 0;
        uint16_t UnitMs = 0;
        uint32_t Streams = 0;
        uint8_t Flags = 0;              // 1 god, 2 game master, 4 party fill off
        std::string_view Name;          // the character's name

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::RecordingStart);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Session);
            out.PutChars<64>(Id);
            out.Put<uint8_t>(Class);
            out.Put<uint8_t>(Race);
            out.Put<uint8_t>(Gender);
            out.Put<uint8_t>(Level);
            out.Put<uint32_t>(Map);
            out.Put<uint32_t>(Instance);
            out.Put<uint32_t>(Zone);
            out.Put<uint32_t>(Area);
            out.Put<float>(X);
            out.Put<float>(Y);
            out.Put<float>(Z);
            out.Put<float>(O);
            out.Put<uint16_t>(SnapshotMs);
            out.Put<uint16_t>(UnitMs);
            out.Put<uint32_t>(Streams);
            out.Put<uint8_t>(Flags);
            std::size_t const length = std::min<std::size_t>(Name.size(), 0xFF);
            out.Put<uint16_t>(uint16_t(length));
            out.PutBytes(Name.data(), length);
            out.End(start);
        }
    };

    /// The last record of a recording (FORMAT §2.8 RecordingEnd): why it ended and the player's verdict.
    struct RecordingEnd
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint64_t Session = 0;
        uint8_t Reason = 0;             // 0 stop, 1 logout, 2 disconnect, 3 shutdown, 4 disk, 5 max minutes
        uint8_t Verdict = 0;            // 0 none, 1 keep, 2 discard
        uint32_t Notes = 0;
        uint32_t Seconds = 0;
        std::string_view Title;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::RecordingEnd);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint64_t>(Session);
            out.Put<uint8_t>(Reason);
            out.Put<uint8_t>(Verdict);
            out.Put<uint32_t>(Notes);
            out.Put<uint32_t>(Seconds);
            std::size_t const length = std::min<std::size_t>(Title.size(), 0xFFFF);
            out.Put<uint16_t>(uint16_t(length));
            out.PutBytes(Title.data(), length);
            out.End(start);
        }
    };

    /// A chat line the recording player sent or was shown (FORMAT §2.8 Chat). Another real player's text, outside
    /// the recorder's group, is redacted (the length kept) unless Animus.Capture.Recording.OthersChat.
    struct Chat
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint8_t Direction = 0;          // 0 sent, 1 received
        uint8_t ChatType = 0;           // ChatMsg
        uint32_t Language = 0;
        uint64_t Sender = 0;
        uint8_t SenderKind = 0;         // 0 self, 1 group player, 2 companion, 3 creature, 4 other player, 5 system
        uint32_t SenderEntry = 0;
        uint64_t Receiver = 0;
        uint8_t Redacted = 0;
        std::string_view Text;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::Chat);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint8_t>(Direction);
            out.Put<uint8_t>(ChatType);
            out.Put<uint32_t>(Language);
            out.Put<uint64_t>(Sender);
            out.Put<uint8_t>(SenderKind);
            out.Put<uint32_t>(SenderEntry);
            out.Put<uint64_t>(Receiver);
            out.Put<uint8_t>(Redacted);
            std::size_t const length = std::min<std::size_t>(Text.size(), 0xFFFF);
            out.Put<uint16_t>(uint16_t(length));
            out.PutBytes(Text.data(), length);
            out.End(start);
        }
    };

    // ---- §2.9 packet (format 4, a recording's) --------------------------------------------------------------------

    /// A chunk of a packet between the recording player's client and the server (FORMAT §2.9 RawPacket), as sent:
    /// every opcode both ways but those Animus.Capture.Recording.PacketSkip names; a payload longer than
    /// RAW_PACKET_CHUNK bytes is written in chunks (`offset` into `total`, flag 2 on all but the last) and one past
    /// Recording.PacketMaxBytes is cut (flag 1).
    constexpr std::size_t RAW_PACKET_CHUNK = 60000;

    struct RawPacket
    {
        uint64_t Ms = 0;
        uint64_t Player = 0;
        uint8_t Direction = 0;          // 0 client to server, 1 server to client
        uint16_t Opcode = 0;
        uint32_t ServerMs = 0;          // getMSTime()
        uint32_t Total = 0;
        uint32_t Offset = 0;
        uint8_t Flags = 0;              // 1 truncated, 2 continued in the next chunk
        uint8_t const* Bytes = nullptr;
        uint16_t Length = 0;

        void Write(Out& out) const
        {
            std::size_t const start = out.Begin(Type::RawPacket);
            out.Put<uint64_t>(Ms);
            out.Put<uint64_t>(Player);
            out.Put<uint8_t>(Direction);
            out.Put<uint16_t>(Opcode);
            out.Put<uint32_t>(ServerMs);
            out.Put<uint32_t>(Total);
            out.Put<uint32_t>(Offset);
            out.Put<uint8_t>(Flags);
            out.Put<uint16_t>(Length);
            out.PutBytes(Bytes, Length);
            out.End(start);
        }
    };

    // ---- §4 identity ----------------------------------------------------------------------------------------------

    /// SHA-256 (FIPS 180-4), small and allocation-free: hashed on map threads for every unit id.
    class Sha256
    {
    public:
        Sha256() { Reset(); }

        void Reset()
        {
            _state = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu,
                0x5be0cd19u };
            _bytes = 0;
            _used = 0;
        }

        void Update(void const* data, std::size_t size)
        {
            uint8_t const* bytes = static_cast<uint8_t const*>(data);
            _bytes += size;
            while (size)
            {
                std::size_t const take = std::min<std::size_t>(64 - _used, size);
                std::memcpy(_block.data() + _used, bytes, take);
                _used += take;
                bytes += take;
                size -= take;
                if (_used == 64)
                {
                    Compress();
                    _used = 0;
                }
            }
        }

        std::array<uint8_t, 32> Final()
        {
            uint64_t const bits = _bytes * 8;
            uint8_t const one = 0x80;
            Update(&one, 1);
            uint8_t const zero = 0;
            while (_used != 56)
                Update(&zero, 1);
            for (int shift = 56; shift >= 0; shift -= 8)
            {
                uint8_t const b = uint8_t(bits >> shift);
                Update(&b, 1);
            }
            std::array<uint8_t, 32> digest{};
            for (std::size_t i = 0; i < 8; ++i)
                for (std::size_t j = 0; j < 4; ++j)
                    digest[i * 4 + j] = uint8_t(_state[i] >> (24 - 8 * j));
            return digest;
        }

    private:
        static constexpr uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

        void Compress()
        {
            static constexpr std::array<uint32_t, 64> K =
            {
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
            };
            std::array<uint32_t, 64> w{};
            for (std::size_t i = 0; i < 16; ++i)
                w[i] = uint32_t(_block[i * 4]) << 24 | uint32_t(_block[i * 4 + 1]) << 16
                    | uint32_t(_block[i * 4 + 2]) << 8 | uint32_t(_block[i * 4 + 3]);
            for (std::size_t i = 16; i < 64; ++i)
            {
                uint32_t const s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                uint32_t const s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
            uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];
            for (std::size_t i = 0; i < 64; ++i)
            {
                uint32_t const s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
                uint32_t const ch = (e & f) ^ (~e & g);
                uint32_t const t1 = h + s1 + ch + K[i] + w[i];
                uint32_t const s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
                uint32_t const maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t const t2 = s0 + maj;
                h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }
            _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
            _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
        }

        std::array<uint32_t, 8> _state{};
        std::array<uint8_t, 64> _block{};
        uint64_t _bytes = 0;
        std::size_t _used = 0;
    };

    using Salt = std::array<uint8_t, 32>;

    /// FORMAT §4: the first 8 bytes of SHA-256(salt ‖ tag ‖ guid as 8 bytes little-endian), read little-endian.
    /// Tags: "player" for player characters (companions too), "creature" for every other guid (creatures, pets,
    /// game objects; the guid's own high bits keep them apart), "session" for session ids.
    inline uint64_t PseudoId(Salt const& salt, std::string_view tag, uint64_t guid)
    {
        if (!guid)
            return 0;
        Sha256 hash;
        hash.Update(salt.data(), salt.size());
        hash.Update(tag.data(), tag.size());
        hash.Update(&guid, sizeof(guid));
        std::array<uint8_t, 32> const digest = hash.Final();
        uint64_t id = 0;
        std::memcpy(&id, digest.data(), sizeof(id));
        return id;
    }

    /// FNV-1a over bytes: a companion decision's `obs_hash` (its observation vector's float bytes).
    inline uint32_t Fnv1a(void const* data, std::size_t size)
    {
        uint8_t const* bytes = static_cast<uint8_t const*>(data);
        uint32_t hash = 0x811c9dc5u;
        for (std::size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 0x01000193u;
        }
        return hash;
    }
}

#endif

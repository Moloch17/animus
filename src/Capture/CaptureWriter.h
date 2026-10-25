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

#ifndef ANIMUS_CAPTURE_WRITER_H
#define ANIMUS_CAPTURE_WRITER_H

#include "CaptureConfig.h"
#include "CaptureFormat.h"
#include "Define.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class WorldSession;

namespace Animus::Capture
{
    /// The capture's files (doc/capture-format.md). Hooks on any thread -- the world thread, every map thread --
    /// write a record into their own thread's scratch buffer and Commit it into that thread's ring: no lock, no
    /// allocation, no I/O, and a full ring drops the record and counts it. One writer thread drains the rings,
    /// sorts records into per-stream, per-map shard files, compresses each flush into a gzip member, writes an
    /// index.json per hour as it closes, and watches the disk.
    ///
    /// **Two tiers** (format 4): slot 0 is the automatic tier -- every player, the hourly files under
    /// `<Dir>/<date>/<hh>/`; slots 1..MAX_RECORDINGS are **manual recordings** a player started of their own play,
    /// each its own directory `<Dir>/recordings/<id>/` with every stream's file and a manifest.json. A ring entry
    /// carries its slot, so one ring serves both.
    class CaptureWriter
    {
    public:
        static constexpr uint32 MAP_ALL = 0xFFFFFFFF;    // session and companion files: "<stream>-all"
        static constexpr uint16 MAX_RECORDINGS = 64;

        static CaptureWriter* Instance();

        /// World thread, at every config load. The first load enabling capture with a usable Dir creates the
        /// directory and the salt and starts the writer thread; Dir and BufferRecords are fixed from then on.
        /// Later loads switch the automatic tier on and off and change the streams and rates.
        void Configure(CaptureConfig const& config);

        /// After the last player has logged out (WorldScript::OnAfterUnloadAllMaps): everything buffered is
        /// written, every open hour gets its index.json, every open recording its manifest, and the writer thread
        /// stops.
        void Stop();

        /// Whether records of `stream` are being taken by the automatic tier now: capture on, the stream enabled,
        /// and not a paused snapshot stream. One atomic load; every hook asks it first.
        [[nodiscard]] bool On(Format::Stream stream) const
        {
            return (_mask.load(std::memory_order_acquire) >> (uint16(stream) - 1)) & 1;
        }
        /// ... or by recording `slot` (every stream, while the writer runs): the one question a hook asks.
        [[nodiscard]] bool OnFor(Format::Stream stream, uint16 slot) const
        {
            return slot ? _running.load(std::memory_order_acquire) : On(stream);
        }
        /// Whether any manual recording is open (one atomic load, so a hook with nothing to do returns at once).
        [[nodiscard]] bool AnyRecording() const { return _recordings.load(std::memory_order_acquire) != 0; }

        [[nodiscard]] bool Running() const { return _thread.joinable(); }
        [[nodiscard]] uint32 SnapshotMs() const { return _snapshotMs.load(std::memory_order_relaxed); }
        [[nodiscard]] uint32 IdleSnapshotMs() const { return _idleSnapshotMs.load(std::memory_order_relaxed); }
        /// World thread only (logins, Record), as Configure is.
        [[nodiscard]] std::string const& LoginNotice() const { return _loginNotice; }
        [[nodiscard]] bool AutoTier() const { return _auto; }
        [[nodiscard]] CaptureConfig::RecordingConfig const& Recording() const { return _recording; }
        [[nodiscard]] std::string const& Dir() const { return _dir; }

        /// FORMAT §4 pseudonymous ids: a player character's (companions included), any other guid's, a session's.
        [[nodiscard]] uint64 PlayerId(uint64 guid) const { return Format::PseudoId(_salt, "player", guid); }
        [[nodiscard]] uint64 UnitId(uint64 guid, bool player) const
        {
            return Format::PseudoId(_salt, player ? "player" : "creature", guid);
        }
        [[nodiscard]] uint64 SessionId(uint64 guid, uint64 ms) const
        {
            return Format::PseudoId(_salt, "session", guid ^ (ms * 0x9E3779B97F4A7C15ull));
        }

        /// Server unix time in milliseconds (every record's `ms`).
        [[nodiscard]] static uint64 NowMs();

        /// This thread's record buffer, emptied: write one record into it, then Commit it.
        Format::Out& Scratch();
        /// The record in `out` into this thread's ring, for `stream`'s file of `map` in the hourly tier (slot 0) or
        /// recording `slot`; dropped (and counted) when the ring is full or the record did not fit the scratch
        /// buffer.
        void Commit(Format::Stream stream, uint32 map, Format::Out const& out) { Commit(stream, map, 0, out); }
        void Commit(Format::Stream stream, uint32 map, uint16 slot, Format::Out const& out);

        /// One record of a type with Write(Out&), when its stream is on for the tier.
        template <typename Record>
        void Write(Format::Stream stream, uint32 map, Record const& record)
        {
            Write(stream, map, uint16(0), record);
        }
        template <typename Record>
        void Write(Format::Stream stream, uint32 map, uint16 slot, Record const& record)
        {
            if (!OnFor(stream, slot))
                return;
            Format::Out& out = Scratch();
            record.Write(out);
            Commit(stream, map, slot, out);
        }

        // ---- manual recordings (world thread) ----

        /// What a recording's manifest says of its start.
        struct ManifestStart
        {
            std::string Id;
            std::string Character;
            uint64 Guid = 0;
            uint64 PlayerId = 0;
            uint8 Class = 0;
            uint8 Race = 0;
            uint8 Gender = 0;
            uint8 Level = 0;
            std::array<uint8, 3> TreePoints{};
            uint16 ItemLevel = 0;
            uint32 Map = 0;
            uint32 Instance = 0;
            bool God = false;
            bool GameMaster = false;
            bool PartyFill = true;
            std::vector<std::string> Party;     // "name|class|level|bot"
        };

        /// What its manifest says of its end.
        struct ManifestEnd
        {
            uint8 Reason = 0;               // RecordingEnd `reason`
            uint8 Verdict = 0;              // 0 none, 1 keep, 2 discard
            std::string Title;
            uint32 Notes = 0;
            std::vector<std::pair<uint32, uint32>> Maps;    // (map, instance) in order of entry
        };

        /// A recording opened: its directory made, the manifest written as "recording"; its slot (0 when none is
        /// free or the writer is not running).
        [[nodiscard]] uint16 OpenRecording(ManifestStart const& start);
        /// A recording closed: the writer flushes what is still in flight, writes the manifest and frees the slot.
        void CloseRecording(uint16 slot, ManifestEnd const& end);
        /// The directory of an open recording (empty for a free slot).
        [[nodiscard]] std::string RecordingDir(uint16 slot) const;
        /// The session of a recording player, for the hooks that run on any thread (OnPacketSent): registered at
        /// Record, forgotten at Stop. `SlotOfSession` reads under a shared lock.
        struct SessionSlot
        {
            uint16 Slot = 0;
            uint64 PlayerId = 0;
        };
        void RegisterSession(WorldSession const* session, uint16 slot, uint64 playerId);
        void ForgetSession(WorldSession const* session);
        [[nodiscard]] SessionSlot SlotOfSession(WorldSession const* session) const;
        /// Recordings the writer wants stopped (the disk is short): their slots, taken once, for the world thread
        /// to stop them with a word to their players.
        [[nodiscard]] std::vector<uint16> TakeStopRequests();
        [[nodiscard]] std::string const& ModuleRevision() const { return _moduleRevision; }
        [[nodiscard]] std::string const& RealmBuild() const { return _realmBuild; }

        ~CaptureWriter();

    private:
        CaptureWriter() = default;

        /// One producing thread's ring: a single-producer single-consumer byte ring of 8-aligned entries
        /// (u32 size, u16 stream, u16 slot, u32 map, u32 record length, record), a zero size meaning "wrapped".
        struct Ring
        {
            std::vector<uint8> Data;
            uint64 Mask = 0;
            std::atomic<uint64> Head{ 0 };      // written by the producer
            std::atomic<uint64> Tail{ 0 };      // written by the writer thread
            std::vector<uint8> Scratch;
            std::unique_ptr<Format::Out> Writer;    // over Scratch
        };

        struct Shard
        {
            std::string Name;                   // move-0.bin.gz
            std::vector<uint8> Pending;
            uint64 Records = 0;                 // this run, all flushed and pending
            uint64 Bytes = 0;                   // compressed bytes written this run
            bool Failed = false;
        };

        struct Hour
        {
            uint64 Key = 0;                     // unix ms / 3600000
            std::string Dir;
            std::map<std::pair<uint16, uint32>, Shard> Shards;
            std::unordered_set<uint64> Players;
            std::unordered_set<uint64> Sessions;
            std::array<uint64, Format::STREAM_COUNT> Dropped{};
            bool SnapshotPaused = false;
        };

        /// A manual recording's slot: opened and closed on the world thread, its shards the writer thread's.
        struct RecordingSlot
        {
            std::atomic<bool> Open{ false };
            std::atomic<bool> Closing{ false };
            std::string Dir;
            ManifestStart Start;
            ManifestEnd End;
            uint64 StartMs = 0;
            uint64 EndMs = 0;
            std::array<std::atomic<uint64>, Format::STREAM_COUNT> Dropped{};
            std::map<std::pair<uint16, uint32>, Shard> Shards;
            uint64 Late = 0;                    // records that arrived after the close
            bool Finished = false;              // the writer wrote the final manifest
        };

        Ring* ThreadRing();
        void UpdateMask();
        bool Start(CaptureConfig const& config);
        void Run();
        void Drain();
        void Route(uint16 stream, uint16 slot, uint32 map, uint8 const* record, uint32 size);
        void OpenShard(Shard& shard, uint16 stream, uint32 map) const;
        Hour& HourFor(uint64 key);
        void Flush(Hour& hour, Shard& shard, uint16 stream);
        void FlushRecording(RecordingSlot& recording, Shard& shard, uint16 stream);
        bool Compress(std::vector<uint8> const& pending, std::string const& path, Shard& shard, uint64& lost);
        void FlushAll();
        void CloseHours(uint64 nowMs, bool all);
        void CloseRecordings(uint64 nowMs, bool all);
        void WriteIndex(Hour& hour);
        void WriteManifest(RecordingSlot& recording, bool final);
        void CollectDrops();
        void Watchdog(uint64 nowMs);

        // Settings (Configure, world thread) and what the hooks read of them.
        std::atomic<uint32> _mask{ 0 };
        std::atomic<bool> _running{ false };
        std::atomic<uint32> _recordings{ 0 };
        std::atomic<uint32> _snapshotMs{ 250 };
        std::atomic<uint32> _idleSnapshotMs{ 1000 };
        bool _enabled = false;
        bool _auto = true;
        uint32 _streams = Format::STREAMS_ALL;
        std::atomic<bool> _snapshotPaused{ false };
        std::string _loginNotice;
        CaptureConfig::RecordingConfig _recording;
        std::string _dir;
        Format::Salt _salt{};
        std::size_t _ringBytes = 0;
        std::atomic<uint32> _flushMs{ 1000 };
        std::atomic<float> _diskWarnGB{ 50.0f };
        std::atomic<float> _diskReserveGB{ 10.0f };

        // The rings, one per producing thread, kept for the life of the process.
        std::mutex _ringsLock;
        std::vector<std::unique_ptr<Ring>> _rings;
        std::array<std::atomic<uint64>, Format::STREAM_COUNT> _dropped{};

        // The recordings: the slot table (opened and closed under _slotsLock on the world thread; the writer
        // thread reads Open and Closing and owns the shards), the session registry, and the stop requests.
        std::mutex _slotsLock;
        std::array<RecordingSlot, MAX_RECORDINGS> _slots;
        mutable std::shared_mutex _sessionsLock;
        std::unordered_map<WorldSession const*, SessionSlot> _sessions;
        std::mutex _stopLock;
        std::vector<uint16> _stopRequests;

        // Writer thread.
        std::thread _thread;
        std::mutex _wakeLock;
        std::condition_variable _wake;
        std::atomic<bool> _stop{ false };
        std::map<uint64, Hour> _hours;
        uint64 _closedThrough = 0;              // the last hour key closed
        uint64 _lastFlushMs = 0;
        uint64 _lastWatchMs = 0;
        uint64 _lastDropLogMs = 0;
        uint64 _lastDiskWarnMs = 0;
        std::vector<uint8> _compressed;
        std::string _moduleRevision;
        std::string _realmBuild;
    };
}

#define sCaptureWriter Animus::Capture::CaptureWriter::Instance()

#endif

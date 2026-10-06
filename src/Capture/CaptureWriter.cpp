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

#include "CaptureWriter.h"
#include "CryptoRandom.h"
#include "GitRevision.h"
#include "Log.h"
#include "StringFormat.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <zlib.h>

#ifndef ANIMUS_MODULE_REVISION
#define ANIMUS_MODULE_REVISION "unknown"
#endif

namespace
{
    using namespace Animus::Capture;

    constexpr uint64 HOUR_MS = 3600000;
    /// A record is at most 65535 payload bytes and its 4-byte frame.
    constexpr std::size_t SCRATCH_BYTES = 65536 + 64;
    constexpr std::size_t ENTRY_HEADER = 16;
    /// A shard past this many buffered bytes is flushed before its clock says so.
    constexpr std::size_t FLUSH_BYTES = 8u << 20;
    constexpr uint64 WATCH_EVERY_MS = 10000;
    constexpr uint64 DISK_WARN_EVERY_MS = 600000;
    constexpr double GIB = 1024.0 * 1024.0 * 1024.0;

    template <typename T>
    T Load(uint8 const* bytes)
    {
        T value;
        std::memcpy(&value, bytes, sizeof(T));
        return value;
    }

    std::tm Utc(uint64 ms)
    {
        std::time_t const seconds = std::time_t(ms / 1000);
        std::tm tm{};
        gmtime_r(&seconds, &tm);
        return tm;
    }

    std::string HexOf(Format::Salt const& salt)
    {
        static char const digits[] = "0123456789abcdef";
        std::string text;
        for (uint8 byte : salt)
        {
            text += digits[byte >> 4];
            text += digits[byte & 15];
        }
        return text;
    }

    bool ParseHex(std::string const& text, Format::Salt& salt)
    {
        if (text.size() < salt.size() * 2)
            return false;
        auto const nibble = [](char c) -> int
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        };
        for (std::size_t i = 0; i < salt.size(); ++i)
        {
            int const high = nibble(text[i * 2]);
            int const low = nibble(text[i * 2 + 1]);
            if (high < 0 || low < 0)
                return false;
            salt[i] = uint8(high << 4 | low);
        }
        return true;
    }

    /// <Dir>/salt: read, or made once (32 random bytes, hex) when there is none. Never replaced: every id the
    /// files hold is hashed with it.
    bool LoadSalt(std::filesystem::path const& dir, Format::Salt& salt)
    {
        std::filesystem::path const path = dir / "salt";
        std::error_code error;
        if (std::filesystem::exists(path, error))
        {
            std::ifstream in(path);
            std::string text;
            in >> text;
            if (!ParseHex(text, salt))
            {
                LOG_ERROR("module.animus", "Animus capture: {} is not 64 hex digits; capture stays off (the salt is "
                    "never replaced, since every id already written was hashed with it)", path.string());
                return false;
            }
            return true;
        }

        Acore::Crypto::GetRandomBytes(salt);
        // "x": never overwrite a salt another process made meanwhile.
        FILE* file = std::fopen(path.string().c_str(), "wx");
        if (!file)
        {
            LOG_ERROR("module.animus", "Animus capture: cannot create {}; capture stays off", path.string());
            return false;
        }
        std::string const text = HexOf(salt) + "\n";
        bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
        written &= std::fclose(file) == 0;
        if (!written)
        {
            LOG_ERROR("module.animus", "Animus capture: cannot write {}; capture stays off", path.string());
            return false;
        }
        LOG_INFO("module.animus", "Animus capture: created the id salt {}", path.string());
        return true;
    }

    /// What an index.json written before (by a run that stopped within the same hour) counted, to add to.
    struct PriorIndex
    {
        std::map<std::string, uint64> Records;
        uint64 Players = 0;
        uint64 Sessions = 0;
        std::map<std::string, uint64> Dropped;
        bool SnapshotPaused = false;
    };

    PriorIndex ReadPrior(std::filesystem::path const& path)
    {
        PriorIndex prior;
        std::ifstream in(path);
        if (!in)
            return prior;
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string const text = buffer.str();

        static std::regex const file("\"([a-z]+-[0-9a-z]+\\.bin\\.gz)\": \\{\"records\": ([0-9]+)");
        for (auto itr = std::sregex_iterator(text.begin(), text.end(), file); itr != std::sregex_iterator(); ++itr)
            prior.Records[(*itr)[1]] = std::stoull((*itr)[2]);
        std::smatch match;
        static std::regex const players("\"players\": ([0-9]+)");
        if (std::regex_search(text, match, players))
            prior.Players = std::stoull(match[1]);
        static std::regex const sessions("\"sessions\": ([0-9]+)");
        if (std::regex_search(text, match, sessions))
            prior.Sessions = std::stoull(match[1]);
        static std::regex const dropped("\"dropped\": \\{([^}]*)\\}");
        if (std::regex_search(text, match, dropped))
        {
            std::string const block = match[1];
            static std::regex const entry("\"([a-z]+)\": ([0-9]+)");
            for (auto itr = std::sregex_iterator(block.begin(), block.end(), entry); itr != std::sregex_iterator();
                ++itr)
                prior.Dropped[(*itr)[1]] = std::stoull((*itr)[2]);
        }
        prior.SnapshotPaused = text.find("\"snapshot\": true") != std::string::npos;
        return prior;
    }
}

Animus::Capture::CaptureWriter* Animus::Capture::CaptureWriter::Instance()
{
    static CaptureWriter instance;
    return &instance;
}

Animus::Capture::CaptureWriter::~CaptureWriter()
{
    // A server that never reached OnAfterUnloadAllMaps (a crash path): stop the thread without logging, since the
    // logger may be gone by now.
    if (_thread.joinable())
    {
        _stop = true;
        _wake.notify_all();
        _thread.join();
    }
}

uint64 Animus::Capture::CaptureWriter::NowMs()
{
    return uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void Animus::Capture::CaptureWriter::Configure(CaptureConfig const& config)
{
    _snapshotMs.store(config.SnapshotMs, std::memory_order_relaxed);
    _idleSnapshotMs.store(config.IdleSnapshotMs, std::memory_order_relaxed);
    _flushMs.store(config.FlushMs, std::memory_order_relaxed);
    _diskWarnGB.store(config.DiskWarnGB, std::memory_order_relaxed);
    _diskReserveGB.store(config.DiskReserveGB, std::memory_order_relaxed);
    _loginNotice = config.LoginNotice;

    if (config.Enable && !Running() && !_stop.load())
        Start(config);
    else if (Running() && !config.Dir.empty() && std::filesystem::path(config.Dir).lexically_normal().string() != _dir)
        LOG_WARN("module.animus", "Animus.Capture.Dir changed to {}; capture keeps writing to {} until the server "
            "restarts", config.Dir, _dir);

    bool const on = config.Enable && Running();
    uint32 const mask = on ? config.Streams : 0;
    if (mask != _mask.load(std::memory_order_relaxed))
        LOG_INFO("module.animus", "Animus capture {} (streams mask {})", on ? "on" : "off", mask);
    _mask.store(mask, std::memory_order_release);
}

bool Animus::Capture::CaptureWriter::Start(CaptureConfig const& config)
{
    if (config.Dir.empty())
    {
        LOG_ERROR("module.animus", "Animus.Capture.Enable = 1, but Animus.Capture.Dir is not set: capture stays off. "
            "Set it to a writable directory with room to grow (it is never cleaned up).");
        return false;
    }

    std::filesystem::path const dir = std::filesystem::path(config.Dir).lexically_normal();
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error || !std::filesystem::is_directory(dir, error))
    {
        LOG_ERROR("module.animus", "Animus capture: cannot create {} ({}); capture stays off", dir.string(),
            error.message());
        return false;
    }

    if (!LoadSalt(dir, _salt))
        return false;

    _dir = dir.string();
    _ringBytes = std::bit_ceil(std::size_t(config.BufferRecords) * 128);
    _moduleRevision = ANIMUS_MODULE_REVISION;
    _realmBuild = GitRevision::GetHash();
    _thread = std::thread(&CaptureWriter::Run, this);
    LOG_INFO("module.animus", "Animus capture: recording to {} ({} KiB of buffer per thread)", _dir,
        _ringBytes / 1024);
    return true;
}

void Animus::Capture::CaptureWriter::Stop()
{
    if (!_thread.joinable())
        return;

    _mask.store(0, std::memory_order_release);
    _stop = true;
    _wake.notify_all();
    _thread.join();
    LOG_INFO("module.animus", "Animus capture: stopped, everything written to {}", _dir);
}

Animus::Capture::CaptureWriter::Ring* Animus::Capture::CaptureWriter::ThreadRing()
{
    // Once per thread for the life of the process: the ring is the thread's until exit, and the writer's after.
    thread_local Ring* ring = nullptr;
    if (!ring)
    {
        std::size_t const bytes = _ringBytes ? _ringBytes : std::size_t(4) << 20;
        auto owned = std::make_unique<Ring>();
        owned->Data.resize(bytes);
        owned->Mask = bytes - 1;
        owned->Scratch.resize(SCRATCH_BYTES);
        owned->Writer = std::make_unique<Format::Out>(owned->Scratch.data(), owned->Scratch.size());
        ring = owned.get();
        std::lock_guard<std::mutex> guard(_ringsLock);
        _rings.push_back(std::move(owned));
    }
    return ring;
}

Animus::Capture::Format::Out& Animus::Capture::CaptureWriter::Scratch()
{
    Format::Out& out = *ThreadRing()->Writer;
    out.Reset();
    return out;
}

void Animus::Capture::CaptureWriter::Commit(Format::Stream stream, uint32 map, Format::Out const& out)
{
    uint32 const index = uint32(stream) - 1;
    if (!out.Ok() || out.Size() < 12)
    {
        _dropped[index].fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Ring& ring = *ThreadRing();
    uint64 const capacity = ring.Data.size();
    uint64 const need = (ENTRY_HEADER + out.Size() + 7) & ~uint64(7);
    uint64 head = ring.Head.load(std::memory_order_relaxed);
    uint64 const tail = ring.Tail.load(std::memory_order_acquire);
    uint64 index_ = head & ring.Mask;
    uint64 const toEnd = capacity - index_;
    uint64 const total = need + (toEnd < need ? toEnd : 0);
    if (need > capacity / 2 || capacity - (head - tail) < total)
    {
        _dropped[index].fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (toEnd < need)
    {
        // The rest of the ring is skipped: a zero size tells the writer to go back to its start.
        uint32 const wrap = 0;
        std::memcpy(ring.Data.data() + index_, &wrap, sizeof(wrap));
        head += toEnd;
        index_ = 0;
    }

    uint8* entry = ring.Data.data() + index_;
    uint32 const size = uint32(need);
    uint16 const streamId = uint16(stream);
    uint16 const zero = 0;
    uint32 const length = uint32(out.Size());
    std::memcpy(entry, &size, 4);
    std::memcpy(entry + 4, &streamId, 2);
    std::memcpy(entry + 6, &zero, 2);
    std::memcpy(entry + 8, &map, 4);
    std::memcpy(entry + 12, &length, 4);
    std::memcpy(entry + ENTRY_HEADER, out.Data(), out.Size());
    ring.Head.store(head + need, std::memory_order_release);
}

void Animus::Capture::CaptureWriter::Run()
{
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(_wakeLock);
            _wake.wait_for(lock, std::chrono::milliseconds(50), [this] { return _stop.load(); });
        }
        if (_stop)
            break;

        Drain();
        uint64 const now = NowMs();
        CollectDrops();
        if (now - _lastFlushMs >= _flushMs.load(std::memory_order_relaxed))
        {
            FlushAll();
            _lastFlushMs = now;
        }
        CloseHours(now, false);
        Watchdog(now);
    }

    // Stopping: whatever the rings hold, every shard flushed, every open hour indexed.
    Drain();
    CollectDrops();
    CloseHours(NowMs(), true);
}

void Animus::Capture::CaptureWriter::Drain()
{
    std::vector<Ring*> rings;
    {
        std::lock_guard<std::mutex> guard(_ringsLock);
        rings.reserve(_rings.size());
        for (std::unique_ptr<Ring> const& ring : _rings)
            rings.push_back(ring.get());
    }

    for (Ring* ring : rings)
    {
        uint64 tail = ring->Tail.load(std::memory_order_relaxed);
        uint64 const head = ring->Head.load(std::memory_order_acquire);
        while (tail < head)
        {
            uint64 const index = tail & ring->Mask;
            uint8 const* entry = ring->Data.data() + index;
            uint32 const size = Load<uint32>(entry);
            if (!size)
            {
                tail += ring->Data.size() - index;
                continue;
            }
            Route(Load<uint16>(entry + 4), Load<uint32>(entry + 8), entry + ENTRY_HEADER, Load<uint32>(entry + 12));
            tail += size;
        }
        ring->Tail.store(tail, std::memory_order_release);
    }
}

Animus::Capture::CaptureWriter::Hour& Animus::Capture::CaptureWriter::HourFor(uint64 key)
{
    auto itr = _hours.find(key);
    if (itr != _hours.end())
        return itr->second;

    Hour& hour = _hours[key];
    hour.Key = key;
    std::tm const tm = Utc(key * HOUR_MS);
    hour.Dir = (std::filesystem::path(_dir) / Acore::StringFormat("{:04}-{:02}-{:02}", tm.tm_year + 1900,
        tm.tm_mon + 1, tm.tm_mday) / Acore::StringFormat("{:02}", tm.tm_hour)).string();
    return hour;
}

void Animus::Capture::CaptureWriter::Route(uint16 stream, uint32 map, uint8 const* record, uint32 size)
{
    if (stream < 1 || stream > Format::STREAM_COUNT || size < 12)
        return;

    // Every record's payload starts with its server time: it goes to the hour it happened in, unless that hour
    // is already closed (a record late past the grace), which sends it to the oldest hour still open.
    uint64 key = Load<uint64>(record + 4) / HOUR_MS;
    if (key <= _closedThrough)
        key = std::max(_closedThrough + 1, NowMs() / HOUR_MS);
    Hour& hour = HourFor(key);

    if (stream == uint16(Format::Stream::Session) || stream == uint16(Format::Stream::Companion))
        map = MAP_ALL;
    auto [itr, created] = hour.Shards.try_emplace({ stream, map });
    Shard& shard = itr->second;
    if (created)
    {
        shard.Name = Acore::StringFormat("{}-{}.bin.gz", Format::StreamName(Format::Stream(stream)),
            map == MAP_ALL ? std::string("all") : std::to_string(map));
        // Every file this run opens starts with a header (a run restarted within the hour appends a second one).
        std::array<uint8, 128> bytes{};
        Format::Out out(bytes.data(), bytes.size());
        Format::FileHeader header;
        header.StreamId = Format::Stream(stream);
        header.OpenedMs = NowMs();
        header.ModuleRevision = _moduleRevision;
        header.RealmBuild = _realmBuild;
        header.Write(out);
        shard.Pending.insert(shard.Pending.end(), out.Data(), out.Data() + out.Size());
    }
    shard.Pending.insert(shard.Pending.end(), record, record + size);
    ++shard.Records;

    // Who was there: the players of the session records and of every client movement packet, and the sessions.
    uint16 const type = Load<uint16>(record);
    if (size >= 4 + 16 && (type == uint16(Format::Type::SessionStart) || type == uint16(Format::Type::SessionContext)
        || type == uint16(Format::Type::SessionEnd) || type == uint16(Format::Type::Move)))
        hour.Players.insert(Load<uint64>(record + 4 + 8));
    if (size >= 4 + 24 && (type == uint16(Format::Type::SessionStart) || type == uint16(Format::Type::SessionContext)
        || type == uint16(Format::Type::SessionEnd)))
        hour.Sessions.insert(Load<uint64>(record + 4 + 16));

    if (shard.Pending.size() >= FLUSH_BYTES)
        Flush(hour, shard, stream);
}

void Animus::Capture::CaptureWriter::Flush(Hour& hour, Shard& shard, uint16 stream)
{
    if (shard.Pending.empty())
        return;

    // One gzip member per flush: concatenated members are one gzip stream, and a file cut short by a crash loses
    // at most its last member.
    z_stream zs{};
    bool ok = deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK;
    if (ok)
    {
        _compressed.resize(deflateBound(&zs, uLong(shard.Pending.size())) + 64);
        zs.next_in = shard.Pending.data();
        zs.avail_in = uInt(shard.Pending.size());
        zs.next_out = _compressed.data();
        zs.avail_out = uInt(_compressed.size());
        ok = deflate(&zs, Z_FINISH) == Z_STREAM_END;
        _compressed.resize(zs.total_out);
        deflateEnd(&zs);
    }

    if (ok)
    {
        std::error_code error;
        std::filesystem::create_directories(hour.Dir, error);
        std::string const path = (std::filesystem::path(hour.Dir) / shard.Name).string();
        FILE* file = std::fopen(path.c_str(), "ab");
        ok = file && std::fwrite(_compressed.data(), 1, _compressed.size(), file) == _compressed.size();
        if (file)
            ok &= std::fclose(file) == 0;
        if (!ok && !shard.Failed)
            LOG_ERROR("module.animus", "Animus capture: cannot write {}; its records are dropped (and counted)", path);
        shard.Failed = !ok;
    }

    if (!ok)
    {
        // Lost: counted in the hour's dropped, as a full buffer's are. The header goes too, and is not counted.
        uint64 lost = 0;
        for (std::size_t offset = 0; offset + 4 <= shard.Pending.size();)
        {
            uint16 const type = Load<uint16>(shard.Pending.data() + offset);
            if (type != uint16(Format::Type::FileHeader))
                ++lost;
            offset += 4 + Load<uint16>(shard.Pending.data() + offset + 2);
        }
        hour.Dropped[stream - 1] += lost;
        shard.Records -= std::min(shard.Records, lost);
    }
    shard.Pending.clear();
}

void Animus::Capture::CaptureWriter::FlushAll()
{
    for (auto& [key, hour] : _hours)
        for (auto& [id, shard] : hour.Shards)
            Flush(hour, shard, id.first);
}

void Animus::Capture::CaptureWriter::CloseHours(uint64 nowMs, bool all)
{
    uint64 const grace = std::max<uint64>(10000, 3 * uint64(_flushMs.load(std::memory_order_relaxed)));
    for (auto itr = _hours.begin(); itr != _hours.end();)
    {
        Hour& hour = itr->second;
        if (!all && (hour.Key + 1) * HOUR_MS + grace > nowMs)
        {
            ++itr;
            continue;
        }
        for (auto& [id, shard] : hour.Shards)
            Flush(hour, shard, id.first);
        WriteIndex(hour);
        _closedThrough = std::max(_closedThrough, hour.Key);
        itr = _hours.erase(itr);
    }
}

void Animus::Capture::CaptureWriter::WriteIndex(Hour& hour)
{
    bool const anything = !hour.Shards.empty() || hour.SnapshotPaused
        || std::any_of(hour.Dropped.begin(), hour.Dropped.end(), [](uint64 n) { return n != 0; });
    if (!anything)
        return;

    std::error_code error;
    std::filesystem::create_directories(hour.Dir, error);
    std::filesystem::path const path = std::filesystem::path(hour.Dir) / "index.json";
    PriorIndex const prior = ReadPrior(path);

    std::map<std::string, uint64> records = prior.Records;
    for (auto const& [id, shard] : hour.Shards)
        records[shard.Name] += shard.Records;

    std::tm const tm = Utc(hour.Key * HOUR_MS);
    std::string text = Acore::StringFormat("{{\"format\": {}, \"hour\": \"{:04}-{:02}-{:02}T{:02}\", "
        "\"module_revision\": \"{}\", \"realm_build\": \"{}\",\n \"files\": {{", Format::FORMAT_VERSION,
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, _moduleRevision, _realmBuild);
    bool first = true;
    for (auto const& [name, count] : records)
    {
        uintmax_t const bytes = std::filesystem::file_size(std::filesystem::path(hour.Dir) / name, error);
        text += Acore::StringFormat("{}\n  \"{}\": {{\"records\": {}, \"bytes\": {}}}", first ? "" : ",", name, count,
            error ? 0 : bytes);
        first = false;
    }
    // A run restarted within the hour adds its sessions to the last run's; its players may be the same people, so
    // the larger count is kept.
    text += Acore::StringFormat("}},\n \"players\": {}, \"sessions\": {},\n \"dropped\": {{",
        std::max<uint64>(prior.Players, hour.Players.size()), prior.Sessions + hour.Sessions.size());
    for (uint32 i = 0; i < Format::STREAM_COUNT; ++i)
    {
        std::string const name(Format::StreamName(Format::Stream(i + 1)));
        uint64 const before = prior.Dropped.contains(name) ? prior.Dropped.at(name) : 0;
        text += Acore::StringFormat("{}\"{}\": {}", i ? ", " : "", name, before + hour.Dropped[i]);
    }
    text += Acore::StringFormat("}}, \"paused\": {{\"snapshot\": {}}}}}\n",
        prior.SnapshotPaused || hour.SnapshotPaused ? "true" : "false");

    std::filesystem::path const temporary = std::filesystem::path(hour.Dir) / "index.json.tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << text;
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
        LOG_ERROR("module.animus", "Animus capture: cannot write {} ({})", path.string(), error.message());
}

void Animus::Capture::CaptureWriter::CollectDrops()
{
    uint64 const now = NowMs();
    Hour* current = nullptr;
    uint64 total = 0;
    std::string counts;
    for (uint32 i = 0; i < Format::STREAM_COUNT; ++i)
    {
        uint64 const dropped = _dropped[i].exchange(0, std::memory_order_relaxed);
        if (!dropped)
            continue;
        if (!current)
            current = &HourFor(std::max(_closedThrough + 1, now / HOUR_MS));
        current->Dropped[i] += dropped;
        total += dropped;
        counts += Acore::StringFormat("{}{} {}", counts.empty() ? "" : ", ", dropped,
            Format::StreamName(Format::Stream(i + 1)));
    }

    if (_snapshotPaused.load(std::memory_order_relaxed))
        HourFor(std::max(_closedThrough + 1, now / HOUR_MS)).SnapshotPaused = true;

    if (total && now - _lastDropLogMs >= 60000)
    {
        LOG_WARN("module.animus", "Animus capture dropped records (a full thread buffer): {}. Raise "
            "Animus.Capture.BufferRecords if this repeats; the counts are in the hour's index.json.", counts);
        _lastDropLogMs = now;
    }
}

void Animus::Capture::CaptureWriter::Watchdog(uint64 nowMs)
{
    if (nowMs - _lastWatchMs < WATCH_EVERY_MS)
        return;
    _lastWatchMs = nowMs;

    std::error_code error;
    std::filesystem::space_info const space = std::filesystem::space(_dir, error);
    if (error)
        return;

    double const freeGB = double(space.available) / GIB;
    float const reserve = _diskReserveGB.load(std::memory_order_relaxed);
    float const warn = _diskWarnGB.load(std::memory_order_relaxed);
    bool const paused = _snapshotPaused.load(std::memory_order_relaxed);
    if (!paused && freeGB < reserve)
    {
        _snapshotPaused.store(true, std::memory_order_relaxed);
        LOG_ERROR("module.animus", "ANIMUS CAPTURE: ONLY {:.1f} GB FREE UNDER {} (Animus.Capture.DiskReserveGB = "
            "{}): THE SNAPSHOT STREAM IS PAUSED. Movement, actions, outcomes and sessions are still written. Free "
            "space to resume it.", freeGB, _dir, reserve);
    }
    else if (paused && freeGB >= double(reserve) + 1.0)
    {
        _snapshotPaused.store(false, std::memory_order_relaxed);
        LOG_WARN("module.animus", "Animus capture: {:.1f} GB free under {}; the snapshot stream resumes", freeGB,
            _dir);
    }

    if (freeGB < warn && nowMs - _lastDiskWarnMs >= DISK_WARN_EVERY_MS)
    {
        LOG_WARN("module.animus", "Animus capture: {:.1f} GB free under {} (Animus.Capture.DiskWarnGB = {}); the "
            "files are kept indefinitely, so this only shrinks", freeGB, _dir, warn);
        _lastDiskWarnMs = nowMs;
    }
}

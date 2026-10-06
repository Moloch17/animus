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

// One of every capture record type, with known values, for testing a reader (the forge's animus.human) against what
// the module writes. Standard library only; not part of the module build:
//
//     g++ -std=c++20 -Wall -Wextra -Werror -I src/Capture tools/capture-sample.cpp -o /tmp/capture-sample
//     /tmp/capture-sample tools/capture-sample.bin tools/capture-sample.json
//
// The .bin is uncompressed (a capture file's gzip members, inflated, are exactly this). The .json lists every record
// in file order: its type, name and payload fields by FORMAT name. Floats are exact binary fractions, so the json's
// decimals are the stored values. It also carries a worked §4 identity example.

#include "CaptureFormat.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace Animus::Capture::Format;

namespace
{
    /// The json, built field by field beside the records.
    class Json
    {
    public:
        void Record(Type type, char const* name)
        {
            Close();
            _text += _records ? ",\n" : "";
            _text += "    {\"type\": " + std::to_string(uint16_t(type)) + ", \"name\": \"" + name + "\", \"fields\": {";
            _fields = 0;
            ++_records;
            _open = true;
        }

        void Field(char const* name, std::string const& value)
        {
            _text += _fields++ ? ", " : "";
            _text += std::string("\"") + name + "\": " + value;
        }

        void Close()
        {
            if (_open)
                _text += "}}";
            _open = false;
        }

        std::string Finish(std::string const& identity)
        {
            Close();
            return "{\n  \"format\": " + std::to_string(FORMAT_VERSION) + ",\n  \"identity\": " + identity
                + ",\n  \"records\": [\n" + _text + "\n  ]\n}\n";
        }

    private:
        std::string _text;
        uint32_t _records = 0;
        uint32_t _fields = 0;
        bool _open = false;
    };

    std::string U(uint64_t value) { return std::to_string(value); }
    std::string I(int64_t value) { return std::to_string(value); }

    std::string F(float value)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.9g", double(value));
        return buffer;
    }

    std::string S(std::string_view text) { return "\"" + std::string(text) + "\""; }

    template <typename T>
    std::string List(std::vector<T> const& values)
    {
        std::string text = "[";
        for (std::size_t i = 0; i < values.size(); ++i)
            text += (i ? ", " : "") + std::to_string(values[i]);
        return text + "]";
    }

    std::string Hex(uint8_t const* bytes, std::size_t size)
    {
        static char const digits[] = "0123456789abcdef";
        std::string text;
        for (std::size_t i = 0; i < size; ++i)
        {
            text += digits[bytes[i] >> 4];
            text += digits[bytes[i] & 15];
        }
        return text;
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s <out.bin> <out.json>\n", argv[0]);
        return 2;
    }

    std::vector<uint8_t> buffer(1 << 16);
    Out out(buffer.data(), buffer.size());
    Json json;

    uint64_t const ms = 1791216000123ull;      // 2026-10-05T14:40:00.123Z
    uint64_t const player = 0x1122334455667788ull;
    uint64_t const session = 0x0102030405060708ull;

    FileHeader header;
    header.StreamId = Stream::Move;
    header.OpenedMs = ms;
    header.ModuleRevision = "0123456789abcdef0123456789abcdef01234567";
    header.RealmBuild = "5c87f702f";
    header.Write(out);
    json.Record(Type::FileHeader, "FileHeader");
    json.Field("magic", S("ANCAP"));
    json.Field("format", U(header.Format));
    json.Field("stream", U(uint16_t(header.StreamId)));
    json.Field("opened_ms", U(header.OpenedMs));
    json.Field("module_revision", S(header.ModuleRevision));
    json.Field("realm_build", S(header.RealmBuild));

    SessionStart start;
    start.Ms = ms;
    start.Player = player;
    start.Session = session;
    start.Class = 8;
    start.Race = 7;
    start.Gender = 1;
    start.Level = 42;
    start.TreePoints = { 31, 5, 0 };
    start.ItemLevel = 187;
    start.Map = 1;
    start.Zone = 17;
    start.Area = 380;
    start.LatencyMs = 87;
    start.ClientBuild = 12340;
    start.Kind = 0;
    for (bool context : { false, true })
    {
        if (context)
        {
            start.Ms += 1000;
            start.Level = 43;
            start.Area = 381;
        }
        start.Write(out, context);
        json.Record(context ? Type::SessionContext : Type::SessionStart, context ? "SessionContext" : "SessionStart");
        json.Field("ms", U(start.Ms));
        json.Field("player", U(start.Player));
        json.Field("session", U(start.Session));
        json.Field("class", U(start.Class));
        json.Field("race", U(start.Race));
        json.Field("gender", U(start.Gender));
        json.Field("level", U(start.Level));
        json.Field("tree_points", List(std::vector<uint32_t>{ start.TreePoints[0], start.TreePoints[1],
            start.TreePoints[2] }));
        json.Field("item_level", U(start.ItemLevel));
        json.Field("map", U(start.Map));
        json.Field("zone", U(start.Zone));
        json.Field("area", U(start.Area));
        json.Field("latency_ms", U(start.LatencyMs));
        json.Field("client_build", U(start.ClientBuild));
        json.Field("kind", U(start.Kind));
    }

    SessionEnd end{ ms + 2000, player, session, 1 };
    end.Write(out);
    json.Record(Type::SessionEnd, "SessionEnd");
    json.Field("ms", U(end.Ms));
    json.Field("player", U(end.Player));
    json.Field("session", U(end.Session));
    json.Field("reason", U(end.Reason));

    GroupMember const members[2] = { { 0xAAAA000000000001ull, 1, 42, 1, 0 }, { 0xBBBB000000000002ull, 5, 41, 2, 1 } };
    GroupState group{ ms + 3000, player, 1, members, 2 };
    group.Write(out);
    json.Record(Type::GroupState, "GroupState");
    json.Field("ms", U(group.Ms));
    json.Field("player", U(group.Player));
    json.Field("kind", U(group.Kind));
    json.Field("count", U(group.Count));
    {
        std::string list = "[";
        for (uint8_t i = 0; i < group.Count; ++i)
            list += std::string(i ? ", " : "") + "{\"unit\": " + U(members[i].Unit) + ", \"class\": "
                + U(members[i].Class) + ", \"level\": " + U(members[i].Level) + ", \"role\": " + U(members[i].Role)
                + ", \"is_companion\": " + U(members[i].IsCompanion) + "}";
        json.Field("members", list + "]");
    }

    uint32_t const spells[3] = { 133, 116, 2136 };
    KnownSpells known{ ms + 4000, player, spells, 3 };
    known.Write(out);
    json.Record(Type::KnownSpells, "KnownSpells");
    json.Field("ms", U(known.Ms));
    json.Field("player", U(known.Player));
    json.Field("count", U(known.Count));
    json.Field("spells", List(std::vector<uint32_t>(spells, spells + 3)));

    Latency latency{ ms + 5000, player, 95 };
    latency.Write(out);
    json.Record(Type::Latency, "Latency");
    json.Field("ms", U(latency.Ms));
    json.Field("player", U(latency.Player));
    json.Field("latency_ms", U(latency.LatencyMs));

    Move move;
    move.Ms = ms + 6000;
    move.Player = player;
    move.ClientMs = 123456789;
    move.Opcode = 0x0B5;                        // MSG_MOVE_START_FORWARD
    move.MoveFlags = 0x00000001 | 0x00001000;   // forward, falling
    move.MoveFlags2 = 0x0020;
    move.X = -8913.25f;
    move.Y = 554.5f;
    move.Z = 93.125f;
    move.O = 1.5f;
    move.Pitch = -0.25f;
    move.FallMs = 350;
    move.JumpZSpeed = -7.5f;
    move.JumpSin = 0.5f;
    move.JumpCos = -0.75f;
    move.JumpXYSpeed = 7.0f;
    move.Map = 0;
    move.Source = 0;
    move.Write(out);
    json.Record(Type::Move, "Move");
    json.Field("ms", U(move.Ms));
    json.Field("player", U(move.Player));
    json.Field("client_ms", U(move.ClientMs));
    json.Field("opcode", U(move.Opcode));
    json.Field("move_flags", U(move.MoveFlags));
    json.Field("move_flags2", U(move.MoveFlags2));
    json.Field("x", F(move.X));
    json.Field("y", F(move.Y));
    json.Field("z", F(move.Z));
    json.Field("o", F(move.O));
    json.Field("pitch", F(move.Pitch));
    json.Field("fall_ms", U(move.FallMs));
    json.Field("jump_zspeed", F(move.JumpZSpeed));
    json.Field("jump_sin", F(move.JumpSin));
    json.Field("jump_cos", F(move.JumpCos));
    json.Field("jump_xyspeed", F(move.JumpXYSpeed));
    json.Field("map", U(move.Map));
    json.Field("source", U(move.Source));

    // A companion's packet (format 2): its player controller's, through its session's movement handlers, recorded
    // at the same point with the same fields as the player's above -- only `source` and its own clock differ.
    uint64_t const companion = 0x0123456789ABCDEFull;
    Move controller = move;
    controller.Ms = ms + 6100;
    controller.Player = companion;
    controller.ClientMs = 1006100;
    controller.Opcode = 0x0EE;                  // MSG_MOVE_HEARTBEAT
    controller.MoveFlags = 0x00000001;          // forward
    controller.MoveFlags2 = 0;
    controller.X = -8910.5f;
    controller.Pitch = 0.0f;
    controller.FallMs = 0;
    controller.JumpZSpeed = 0.0f;
    controller.JumpSin = 0.0f;
    controller.JumpCos = 0.0f;
    controller.JumpXYSpeed = 0.0f;
    controller.Source = uint8_t(MoveSource::ControllerPacket);
    controller.Write(out);
    json.Record(Type::Move, "Move");
    json.Field("ms", U(controller.Ms));
    json.Field("player", U(controller.Player));
    json.Field("client_ms", U(controller.ClientMs));
    json.Field("opcode", U(controller.Opcode));
    json.Field("move_flags", U(controller.MoveFlags));
    json.Field("move_flags2", U(controller.MoveFlags2));
    json.Field("x", F(controller.X));
    json.Field("y", F(controller.Y));
    json.Field("z", F(controller.Z));
    json.Field("o", F(controller.O));
    json.Field("pitch", F(controller.Pitch));
    json.Field("fall_ms", U(controller.FallMs));
    json.Field("jump_zspeed", F(controller.JumpZSpeed));
    json.Field("jump_sin", F(controller.JumpSin));
    json.Field("jump_cos", F(controller.JumpCos));
    json.Field("jump_xyspeed", F(controller.JumpXYSpeed));
    json.Field("map", U(controller.Map));
    json.Field("source", U(controller.Source));

    // Who moves and under what, for each of them: the player mounted and in combat, the companion on foot in a form
    // with its model and move block revision.
    MoverState const movers[2] = {
        { ms + 6200, player, 0, 1, 1, 60, 0, 12, 23229, 0, 1, 0, {} },
        { ms + 6300, companion, 1, 11, 4, 60, 0, 12, 0, 5, 0, 2, "druid_travel" },
    };
    for (MoverState const& mover : movers)
    {
        mover.Write(out);
        json.Record(Type::MoverState, "MoverState");
        json.Field("ms", U(mover.Ms));
        json.Field("player", U(mover.Player));
        json.Field("kind", U(mover.Kind));
        json.Field("class", U(mover.Class));
        json.Field("race", U(mover.Race));
        json.Field("level", U(mover.Level));
        json.Field("map", U(mover.Map));
        json.Field("zone", U(mover.Zone));
        json.Field("mount", U(mover.Mount));
        json.Field("form", U(mover.Form));
        json.Field("in_combat", U(mover.InCombat));
        json.Field("move_revision", U(mover.MoveRevision));
        json.Field("model", S(mover.Model));
    }

    Speeds speeds{ ms + 7000, player, 2.5f, 7.0f, 4.5f, 4.75f, 2.5f, 7.0f, 4.5f, 3.140625f, 3.140625f };
    speeds.Write(out);
    json.Record(Type::Speeds, "Speeds");
    json.Field("ms", U(speeds.Ms));
    json.Field("player", U(speeds.Player));
    json.Field("walk", F(speeds.Walk));
    json.Field("run", F(speeds.Run));
    json.Field("run_back", F(speeds.RunBack));
    json.Field("swim", F(speeds.Swim));
    json.Field("swim_back", F(speeds.SwimBack));
    json.Field("flight", F(speeds.Flight));
    json.Field("flight_back", F(speeds.FlightBack));
    json.Field("turn_rate", F(speeds.TurnRate));
    json.Field("pitch_rate", F(speeds.PitchRate));

    MotionEvent motion{ ms + 8000, player, Motion::Mount, 458, -8913.25f, 554.5f, 93.125f, 0 };
    motion.Write(out);
    json.Record(Type::MotionEvent, "MotionEvent");
    json.Field("ms", U(motion.Ms));
    json.Field("player", U(motion.Player));
    json.Field("event", U(uint8_t(motion.Event)));
    json.Field("arg", U(motion.Arg));
    json.Field("x", F(motion.X));
    json.Field("y", F(motion.Y));
    json.Field("z", F(motion.Z));
    json.Field("map", U(motion.Map));

    CastRequest cast;
    cast.Ms = ms + 9000;
    cast.Player = player;
    cast.Spell = 133;
    cast.Target = 0xCCCC000000000003ull;
    cast.Kind = TargetKind::HostileCreature;
    cast.TX = 1.25f;
    cast.TY = -2.5f;
    cast.TZ = 3.75f;
    cast.GcdActive = 1;
    cast.Casting = 0;
    cast.Power = 1234;
    cast.PowerType = 0;
    cast.Write(out);
    json.Record(Type::CastRequest, "CastRequest");
    json.Field("ms", U(cast.Ms));
    json.Field("player", U(cast.Player));
    json.Field("spell", U(cast.Spell));
    json.Field("target", U(cast.Target));
    json.Field("target_kind", U(uint8_t(cast.Kind)));
    json.Field("tx", F(cast.TX));
    json.Field("ty", F(cast.TY));
    json.Field("tz", F(cast.TZ));
    json.Field("gcd_active", U(cast.GcdActive));
    json.Field("casting", U(cast.Casting));
    json.Field("power", U(cast.Power));
    json.Field("power_type", U(cast.PowerType));

    CastOutcome result{ ms + 9100, player, 133, CAST_GO };
    result.Write(out, Type::CastResult);
    json.Record(Type::CastResult, "CastResult");
    json.Field("ms", U(result.Ms));
    json.Field("player", U(result.Player));
    json.Field("spell", U(result.Spell));
    json.Field("result", U(result.Value));

    CastOutcome castEnd{ ms + 9200, player, 133, 1 };
    castEnd.Write(out, Type::CastEnd);
    json.Record(Type::CastEnd, "CastEnd");
    json.Field("ms", U(castEnd.Ms));
    json.Field("player", U(castEnd.Player));
    json.Field("spell", U(castEnd.Spell));
    json.Field("how", U(castEnd.Value));

    Select select{ ms + 10000, player, 0xCCCC000000000003ull, TargetKind::HostileCreature, 12.5f };
    select.Write(out);
    json.Record(Type::Select, "Select");
    json.Field("ms", U(select.Ms));
    json.Field("player", U(select.Player));
    json.Field("target", U(select.Target));
    json.Field("target_kind", U(uint8_t(select.Kind)));
    json.Field("distance", F(select.Distance));

    ItemUse item{ ms + 11000, player, 6948, 8690, 0 };
    item.Write(out);
    json.Record(Type::ItemUse, "ItemUse");
    json.Field("ms", U(item.Ms));
    json.Field("player", U(item.Player));
    json.Field("item", U(item.Item));
    json.Field("spell", U(item.Spell));
    json.Field("target", U(item.Target));

    Attack attack{ ms + 12000, player, 0xCCCC000000000003ull, 1 };
    attack.Write(out);
    json.Record(Type::Attack, "Attack");
    json.Field("ms", U(attack.Ms));
    json.Field("player", U(attack.Player));
    json.Field("target", U(attack.Target));
    json.Field("start", U(attack.Start));

    Interact interact{ ms + 13000, player, Interaction::QuestTurnIn, 197, 0xDDDD000000000004ull, 33 };
    interact.Write(out);
    json.Record(Type::Interact, "Interact");
    json.Field("ms", U(interact.Ms));
    json.Field("player", U(interact.Player));
    json.Field("what", U(uint8_t(interact.What)));
    json.Field("entry", U(interact.Entry));
    json.Field("target", U(interact.Target));
    json.Field("arg", U(interact.Arg));

    SnapshotSelf self{ -8913.25f, 554.5f, 93.125f, 1.5f, -0.25f, 0, 87.5f, 50.0f, 0, 1 | 16, 133, 1.0f,
        0xCCCC000000000003ull, 0 };
    SnapshotUnit const units[2] =
    {
        { 0xCCCC000000000003ull, 299, TargetKind::HostileCreature, -8910.5f, 556.25f, 93.0f, 3.0f, 0.5f, -0.5f, 0.0f,
            62.5f, 0.0f, 10, 0, 1, 0, player, 140.5f },
        { 0xAAAA000000000001ull, 0, TargetKind::FriendlyPlayer, -8915.0f, 550.0f, 93.5f, 0.75f, 0.0f, 0.0f, 0.0f,
            100.0f, 25.0f, 42, 2, 4 | 2, 2050, 0, 0.0f },
    };
    SnapshotAura const auras[2] = { { 1459, 1, 1800000, 1 }, { 12654, 3, 4000, 0 } };
    SnapshotCooldown const cooldowns[1] = { { 2136, 6500 } };
    {
        SnapshotBuilder snapshot(out, ms + 14000, player, self);
        for (SnapshotUnit const& unit : units)
            snapshot.Unit(unit);
        for (SnapshotAura const& aura : auras)
            snapshot.Aura(aura);
        for (SnapshotCooldown const& cooldown : cooldowns)
            snapshot.Cooldown(cooldown);
        snapshot.Finish();
    }
    json.Record(Type::Snapshot, "Snapshot");
    json.Field("ms", U(ms + 14000));
    json.Field("player", U(player));
    json.Field("self", "{\"x\": " + F(self.X) + ", \"y\": " + F(self.Y) + ", \"z\": " + F(self.Z) + ", \"o\": "
        + F(self.O) + ", \"pitch\": " + F(self.Pitch) + ", \"map\": " + U(self.Map) + ", \"health_pct\": "
        + F(self.HealthPct) + ", \"power_pct\": " + F(self.PowerPct) + ", \"power_type\": " + U(self.PowerType)
        + ", \"flags\": " + U(self.Flags) + ", \"casting_spell\": " + U(self.CastingSpell) + ", \"breath_pct\": "
        + F(self.BreathPct) + ", \"target\": " + U(self.Target) + ", \"shapeshift_form\": "
        + U(self.ShapeshiftForm) + "}");
    {
        std::string list = "[";
        for (std::size_t i = 0; i < 2; ++i)
        {
            SnapshotUnit const& u = units[i];
            list += std::string(i ? ", " : "") + "{\"unit\": " + U(u.Unit) + ", \"entry\": " + U(u.Entry)
                + ", \"kind\": " + U(uint8_t(u.Kind)) + ", \"x\": " + F(u.X) + ", \"y\": " + F(u.Y) + ", \"z\": "
                + F(u.Z) + ", \"o\": " + F(u.O) + ", \"vx\": " + F(u.VX) + ", \"vy\": " + F(u.VY) + ", \"vz\": "
                + F(u.VZ) + ", \"health_pct\": " + F(u.HealthPct) + ", \"power_pct\": " + F(u.PowerPct)
                + ", \"level\": " + U(u.Level) + ", \"reaction\": " + U(u.Reaction) + ", \"flags\": " + U(u.Flags)
                + ", \"casting_spell\": " + U(u.CastingSpell) + ", \"target\": " + U(u.Target)
                + ", \"threat_on_player\": " + F(u.ThreatOnPlayer) + "}";
        }
        json.Field("units", list + "]");
        list = "[";
        for (std::size_t i = 0; i < 2; ++i)
            list += std::string(i ? ", " : "") + "{\"spell\": " + U(auras[i].Spell) + ", \"stacks\": "
                + U(auras[i].Stacks) + ", \"remaining_ms\": " + I(auras[i].RemainingMs) + ", \"positive\": "
                + U(auras[i].Positive) + "}";
        json.Field("auras", list + "]");
        json.Field("cooldowns", "[{\"spell\": " + U(cooldowns[0].Spell) + ", \"remaining_ms\": "
            + U(cooldowns[0].RemainingMs) + "}]");
    }

    Damage damage{ ms + 15000, player, 0xCCCC000000000003ull, 133, 412, 20, 0, 4, 1 };
    damage.Write(out);
    json.Record(Type::Damage, "Damage");
    json.Field("ms", U(damage.Ms));
    json.Field("source", U(damage.Source));
    json.Field("target", U(damage.Target));
    json.Field("spell", U(damage.Spell));
    json.Field("amount", U(damage.Amount));
    json.Field("absorbed", U(damage.Absorbed));
    json.Field("overkill", U(damage.Overkill));
    json.Field("school", U(damage.School));
    json.Field("flags", U(damage.Flags));

    Heal heal{ ms + 16000, 0xBBBB000000000002ull, player, 2050, 300, 45, 2 };
    heal.Write(out);
    json.Record(Type::Heal, "Heal");
    json.Field("ms", U(heal.Ms));
    json.Field("source", U(heal.Source));
    json.Field("target", U(heal.Target));
    json.Field("spell", U(heal.Spell));
    json.Field("amount", U(heal.Amount));
    json.Field("overheal", U(heal.Overheal));
    json.Field("flags", U(heal.Flags));

    Kill kill{ ms + 17000, player, 0xCCCC000000000003ull, 299, TargetKind::HostileCreature };
    kill.Write(out);
    json.Record(Type::Kill, "Kill");
    json.Field("ms", U(kill.Ms));
    json.Field("killer", U(kill.Killer));
    json.Field("victim", U(kill.Victim));
    json.Field("victim_entry", U(kill.VictimEntry));
    json.Field("victim_kind", U(uint8_t(kill.VictimKind)));

    Death death{ ms + 18000, player, 0, 2, -8900.5f, 560.25f, 60.0f, 0 };
    death.Write(out);
    json.Record(Type::Death, "Death");
    json.Field("ms", U(death.Ms));
    json.Field("player", U(death.Player));
    json.Field("killer", U(death.Killer));
    json.Field("cause", U(death.Cause));
    json.Field("x", F(death.X));
    json.Field("y", F(death.Y));
    json.Field("z", F(death.Z));
    json.Field("map", U(death.Map));

    Quest quest{ ms + 19000, player, 33, 2 };
    quest.Write(out);
    json.Record(Type::Quest, "Quest");
    json.Field("ms", U(quest.Ms));
    json.Field("player", U(quest.Player));
    json.Field("quest", U(quest.QuestId));
    json.Field("event", U(quest.Event));

    Encounter encounter{ ms + 20000, player, 36, 7, 3, 1 };
    encounter.Write(out);
    json.Record(Type::Encounter, "Encounter");
    json.Field("ms", U(encounter.Ms));
    json.Field("player", U(encounter.Player));
    json.Field("map", U(encounter.Map));
    json.Field("instance", U(encounter.Instance));
    json.Field("boss_entry", U(encounter.BossEntry));
    json.Field("event", U(encounter.Event));

    PvP pvp{ ms + 21000, player, 0xEEEE000000000005ull, 2 };
    pvp.Write(out);
    json.Record(Type::PvP, "PvP");
    json.Field("ms", U(pvp.Ms));
    json.Field("player", U(pvp.Player));
    json.Field("other", U(pvp.Other));
    json.Field("event", U(pvp.Event));

    Area area{ ms + 22000, player, 0, 12, 87 };
    area.Write(out);
    json.Record(Type::Area, "Area");
    json.Field("ms", U(area.Ms));
    json.Field("player", U(area.Player));
    json.Field("map", U(area.Map));
    json.Field("zone", U(area.Zone));
    json.Field("area", U(area.AreaId));

    uint16_t const actions[2] = { 7, 65535 };
    CompanionDecision decision{ ms + 23000, 0xBBBB000000000002ull, player, "priest_deadmines", 0xDEADBEEFu, actions,
        2, 4, 65535 };
    decision.Write(out);
    json.Record(Type::CompanionDecision, "CompanionDecision");
    json.Field("ms", U(decision.Ms));
    json.Field("companion", U(decision.Companion));
    json.Field("owner", U(decision.Owner));
    json.Field("model", S(decision.Model));
    json.Field("obs_hash", U(decision.ObsHash));
    json.Field("n_actions", U(decision.ActionCount));
    json.Field("actions", List(std::vector<uint32_t>(actions, actions + 2)));
    json.Field("goal", U(decision.Goal));
    json.Field("goal2", U(decision.Goal2));

    CompanionCommand command{ ms + 24000, player, 0xBBBB000000000002ull, 1, 0 };
    command.Write(out);
    json.Record(Type::CompanionCommand, "CompanionCommand");
    json.Field("ms", U(command.Ms));
    json.Field("owner", U(command.Owner));
    json.Field("companion", U(command.Companion));
    json.Field("command", U(command.Command));
    json.Field("arg", U(command.Arg));

    CompanionRating rating{ ms + 25000, player, 0xBBBB000000000002ull, -1, 5 };
    rating.Write(out);
    json.Record(Type::CompanionRating, "CompanionRating");
    json.Field("ms", U(rating.Ms));
    json.Field("owner", U(rating.Owner));
    json.Field("companion", U(rating.Companion));
    json.Field("rating", I(rating.Rating));
    json.Field("reason", U(rating.Reason));

    if (!out.Ok())
    {
        std::fprintf(stderr, "the sample did not fit its buffer\n");
        return 1;
    }

    // §4: salt bytes 0x00..0x1f, the guid of player 1 (high guid 0, counter 1).
    Salt salt{};
    for (std::size_t i = 0; i < salt.size(); ++i)
        salt[i] = uint8_t(i);
    uint64_t const guid = 1;
    uint64_t const id = PseudoId(salt, "player", guid);
    Sha256 empty;
    std::array<uint8_t, 32> const emptyDigest = empty.Final();
    std::string const identity = "{\"salt_hex\": \"" + Hex(salt.data(), salt.size()) + "\", \"tag\": \"player\", "
        "\"guid\": " + U(guid) + ", \"hashed_bytes\": \"salt || tag (ascii, no terminator) || guid (u64 little-endian)\", "
        "\"id\": " + U(id) + ", \"sha256_of_empty\": \"" + Hex(emptyDigest.data(), emptyDigest.size()) + "\"}";

    FILE* bin = std::fopen(argv[1], "wb");
    FILE* text = std::fopen(argv[2], "wb");
    if (!bin || !text)
    {
        std::fprintf(stderr, "cannot write %s or %s\n", argv[1], argv[2]);
        return 1;
    }
    std::fwrite(out.Data(), 1, out.Size(), bin);
    std::string const document = json.Finish(identity);
    std::fwrite(document.data(), 1, document.size(), text);
    std::fclose(bin);
    std::fclose(text);
    std::printf("%zu bytes of records, player-1 id %llu\n", out.Size(), static_cast<unsigned long long>(id));
    return 0;
}

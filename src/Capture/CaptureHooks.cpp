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

// The capture's hooks: every stream of doc/capture-format.md, from the stock core's script hooks. Each hook first
// asks the writer whether its stream is on (one atomic load), then copies a few fields into one record in its
// thread's buffer (CaptureWriter::Commit). Per-player state lives in the player's own CustomData and is only
// touched by the thread updating that player: its map thread, or the world thread while no map updates.

#include "Capture.h"
#include "AllSpellScript.h"
#include "AnimusMod.h"
#include "CaptureConfig.h"
#include "CaptureWriter.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Creature.h"
#include "GameObject.h"
#include "GlobalScript.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Item.h"
#include "LFG.h"
#include "Map.h"
#include "MovementHandlerScript.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ServerScript.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "GameTime.h"
#include "Transport.h"
#include "UnitScript.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace Animus::Capture;
    namespace F = Animus::Capture::Format;

    /// The only client build an AzerothCore 3.3.5a realm accepts; the world session does not keep the client's.
    constexpr uint32 CLIENT_BUILD = 12340;
    constexpr uint64 LATENCY_EVERY_MS = 60000;
    constexpr uint64 SPELLS_EVERY_MS = 1000;
    constexpr float UNIT_RANGE = 40.0f;
    constexpr uint32 MAX_HOSTILE = 24;
    constexpr uint32 MAX_FRIENDLY = 10;
    /// Base breath under water (Player::getMaxTimer(BREATH_TIMER) without modifiers).
    constexpr float BREATH_MS = 60000.0f;

    CaptureWriter& Writer() { return *sCaptureWriter; }

    /// A player's capture state, in its CustomData under a short key (no allocation on lookup).
    struct PlayerState : DataMap::Base
    {
        bool Started = false;
        uint64 Id = 0;
        uint64 Session = 0;
        uint8 Kind = 0;                     // 0 human, 1 companion

        // Context (SessionContext on change).
        uint8 Level = 0;
        std::array<uint8, 3> Tree{};
        uint16 ItemLevel = 0;
        uint32 Map = 0;
        uint32 Zone = 0;
        uint32 Area = 0;
        uint64 GroupSig = 0;
        bool TalentsDirty = true;
        bool GearDirty = true;
        bool SpellsDirty = false;

        uint64 LastSnapshotMs = 0;
        uint64 LastLatencyMs = 0;
        uint64 LastSpellsMs = 0;

        std::array<float, 9> Speeds{};

        // What the motion events are edges of.
        uint32 Mount = 0;
        bool Taxi = false;
        bool Rooted = false;
        bool Stunned = false;
        bool Feared = false;
        uint32 Form = 0;
        uint32 Vehicle = 0;
        uint32 Transport = 0;
        bool Loading = false;

        // The MoverState last written (WriteMoverState on change).
        bool MoverWritten = false;
        uint32 MoverMount = 0;
        uint32 MoverForm = 0;
        bool MoverCombat = false;
        uint8 MoverLevel = 0;
        uint32 MoverMap = 0;
        uint32 MoverZone = 0;
        uint8 MoverKind = 0;
        // A companion's model and its move block revision (CompanionModel, world thread while maps are idle).
        std::string Model;
        uint8 MoveRevision = 0;
        std::string MoverModel;
        uint8 MoverRevision = 0;

        // Movement packets that reached the server's handlers, and those it kept (MoveTally).
        uint32 MovesSent = 0;
        uint32 MovesKept = 0;
        uint32 TallySent = 0;
        uint32 TallyKept = 0;
        uint64 LastTallyMs = 0;

        uint64 SubmergedSinceMs = 0;
        uint8 LastEnvType = 0xFF;           // the last environmental damage (EnviromentalDamage), for a death's cause
        uint64 LastEnvMs = 0;
        uint32 CancelSpell = 0;             // the last cast the client asked to cancel, for a CastEnd's `how`
        uint64 CancelMs = 0;
    };

    std::string const STATE_KEY = "ancap";

    PlayerState* StateOf(Player* player) { return player->CustomData.Get<PlayerState>(STATE_KEY); }

    uint64 IdOf(ObjectGuid guid) { return guid ? Writer().UnitId(guid.GetRawValue(), guid.IsPlayer()) : 0; }
    uint64 IdOf(WorldObject const* object) { return object ? IdOf(object->GetGUID()) : 0; }

    uint64 PlayerIdOf(Player* player)
    {
        if (PlayerState const* state = StateOf(player))
            return state->Id;
        return IdOf(player);
    }

    F::TargetKind KindOf(Player const* player, WorldObject const* object)
    {
        if (!object)
            return F::TargetKind::None;
        if (object == player)
            return F::TargetKind::Self;
        if (object->IsGameObject())
            return F::TargetKind::GameObject;
        Unit const* unit = object->ToUnit();
        if (!unit)
            return F::TargetKind::None;
        bool const hostile = player->IsHostileTo(unit);
        if (unit->IsPlayer())
            return hostile ? F::TargetKind::HostilePlayer : F::TargetKind::FriendlyPlayer;
        if (hostile)
            return F::TargetKind::HostileCreature;
        return player->IsFriendlyTo(unit) ? F::TargetKind::FriendlyCreature : F::TargetKind::Neutral;
    }

    uint32 CastingSpell(Unit const* unit)
    {
        for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
            if (Spell const* spell = unit->GetCurrentSpell(type))
                return spell->GetSpellInfo()->Id;
        return 0;
    }

    /// A unit's velocity from what it is doing now: the speed of its mode along its movement direction (facing,
    /// and for players the forward/back/strafe keys), and a jump's or fall's vertical speed. An estimate; the
    /// server keeps no velocity.
    void VelocityOf(Unit const* unit, float& vx, float& vy, float& vz)
    {
        vx = vy = vz = 0.0f;
        MovementInfo const& info = unit->m_movementInfo;
        if (info.HasMovementFlag(MOVEMENTFLAG_FALLING))
            vz = -info.jump.zspeed;
        if (!unit->isMoving())
            return;

        bool const back = info.HasMovementFlag(MOVEMENTFLAG_BACKWARD);
        bool const swim = info.HasMovementFlag(MOVEMENTFLAG_SWIMMING);
        bool const fly = info.HasMovementFlag(MOVEMENTFLAG_FLYING);
        UnitMoveType type = MOVE_RUN;
        if (fly)
            type = back ? MOVE_FLIGHT_BACK : MOVE_FLIGHT;
        else if (swim)
            type = back ? MOVE_SWIM_BACK : MOVE_SWIM;
        else if (unit->IsWalking())
            type = MOVE_WALK;
        else if (back)
            type = MOVE_RUN_BACK;

        float angle = 0.0f;
        bool const left = info.HasMovementFlag(MOVEMENTFLAG_STRAFE_LEFT);
        bool const right = info.HasMovementFlag(MOVEMENTFLAG_STRAFE_RIGHT);
        bool const forward = info.HasMovementFlag(MOVEMENTFLAG_FORWARD);
        if (back)
            angle = float(M_PI) + (left ? -float(M_PI) / 4 : right ? float(M_PI) / 4 : 0.0f);
        else if (left)
            angle = forward ? float(M_PI) / 4 : float(M_PI) / 2;
        else if (right)
            angle = forward ? -float(M_PI) / 4 : -float(M_PI) / 2;

        float const speed = unit->GetSpeed(type);
        float const heading = unit->GetOrientation() + angle;
        vx = speed * std::cos(heading);
        vy = speed * std::sin(heading);
    }

    std::array<float, 9> SpeedsOf(Player const* player)
    {
        return { player->GetSpeed(MOVE_WALK), player->GetSpeed(MOVE_RUN), player->GetSpeed(MOVE_RUN_BACK),
            player->GetSpeed(MOVE_SWIM), player->GetSpeed(MOVE_SWIM_BACK), player->GetSpeed(MOVE_FLIGHT),
            player->GetSpeed(MOVE_FLIGHT_BACK), player->GetSpeed(MOVE_TURN_RATE), player->GetSpeed(MOVE_PITCH_RATE) };
    }

    void WriteSpeeds(Player* player, PlayerState const& state, uint64 now)
    {
        F::Speeds record;
        record.Ms = now;
        record.Player = state.Id;
        record.Walk = state.Speeds[0];
        record.Run = state.Speeds[1];
        record.RunBack = state.Speeds[2];
        record.Swim = state.Speeds[3];
        record.SwimBack = state.Speeds[4];
        record.Flight = state.Speeds[5];
        record.FlightBack = state.Speeds[6];
        record.TurnRate = state.Speeds[7];
        record.PitchRate = state.Speeds[8];
        Writer().Write(F::Stream::Move, player->GetMapId(), record);
    }

    void WriteMotion(Player* /*player*/, uint64 id, F::Motion event, uint32 arg, float x, float y, float z,
        uint32 map)
    {
        F::MotionEvent record{ CaptureWriter::NowMs(), id, event, arg, x, y, z, map };
        Writer().Write(F::Stream::Move, map, record);
    }

    void WriteMotion(Player* player, uint64 id, F::Motion event, uint32 arg = 0)
    {
        WriteMotion(player, id, event, arg, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            player->GetMapId());
    }

    /// The context fields as they are now; true when any changed.
    bool RefreshContext(Player* player, PlayerState& state)
    {
        bool changed = false;
        auto const set = [&changed](auto& field, auto value)
        {
            if (field != value)
            {
                field = value;
                changed = true;
            }
        };
        set(state.Level, player->GetLevel());
        set(state.Map, player->GetMapId());
        set(state.Zone, player->GetZoneId());
        set(state.Area, player->GetAreaId());
        if (state.TalentsDirty)
        {
            uint8 points[3] = { 0, 0, 0 };
            player->GetTalentTreePoints(points);
            set(state.Tree, std::array<uint8, 3>{ points[0], points[1], points[2] });
            state.TalentsDirty = false;
        }
        if (state.GearDirty)
        {
            set(state.ItemLevel, uint16(std::lround(std::max(0.0f, player->GetAverageItemLevel()))));
            state.GearDirty = false;
        }
        return changed;
    }

    uint64 GroupSignature(Player const* player)
    {
        Group const* group = player->GetGroup();
        if (!group)
            return 0;
        return group->GetGUID().GetRawValue() * 131 + group->GetMembersCount() * 2 + (group->isRaidGroup() ? 1 : 0);
    }

    void WriteSession(Player* player, PlayerState const& state, uint64 now, bool context)
    {
        F::SessionStart record;
        record.Ms = now;
        record.Player = state.Id;
        record.Session = state.Session;
        record.Class = player->getClass();
        record.Race = player->getRace();
        record.Gender = player->getGender();
        record.Level = state.Level;
        record.TreePoints = state.Tree;
        record.ItemLevel = state.ItemLevel;
        record.Map = state.Map;
        record.Zone = state.Zone;
        record.Area = state.Area;
        record.LatencyMs = uint16(std::min<uint32>(0xFFFF, player->GetSession() ? player->GetSession()->GetLatency()
            : 0));
        record.ClientBuild = state.Kind ? 0 : CLIENT_BUILD;
        record.Kind = state.Kind;
        if (!Writer().On(F::Stream::Session))
            return;
        F::Out& out = Writer().Scratch();
        record.Write(out, context);
        Writer().Commit(F::Stream::Session, CaptureWriter::MAP_ALL, out);
    }

    /// The group as this player's map thread can see it: every member's id, class and role; the level of those on
    /// this map (another map's players are another thread's, so theirs is written 0).
    void WriteGroup(Player* player, PlayerState const& state, uint64 now)
    {
        if (!Writer().On(F::Stream::Session))
            return;
        std::array<F::GroupMember, 40> members{};
        F::GroupState record;
        record.Ms = now;
        record.Player = state.Id;
        record.Members = members.data();
        if (Group const* group = player->GetGroup())
        {
            record.Kind = group->isRaidGroup() ? 2 : 1;
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
            {
                if (record.Count == members.size())
                    break;
                F::GroupMember& member = members[record.Count++];
                member.Unit = IdOf(slot.guid);
                if (Player const* other = ObjectAccessor::GetPlayer(*player, slot.guid))
                {
                    member.Class = other->getClass();
                    member.Level = other->GetLevel();
                }
                else if (CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(slot.guid))
                    member.Class = entry->Class;
                member.Role = (slot.roles & lfg::PLAYER_ROLE_TANK) ? 1 : (slot.roles & lfg::PLAYER_ROLE_HEALER) ? 2
                    : (slot.roles & lfg::PLAYER_ROLE_DAMAGE) ? 0 : 3;
                member.IsCompanion = sAnimusMod->IsBot(slot.guid) ? 1 : 0;
            }
        }
        Writer().Write(F::Stream::Session, CaptureWriter::MAP_ALL, record);
    }

    void WriteKnownSpells(Player* player, PlayerState const& state, uint64 now)
    {
        if (!Writer().On(F::Stream::Session))
            return;
        thread_local std::vector<uint32> spells;
        spells.clear();
        uint8 const spec = player->GetActiveSpecMask();
        for (auto const& [spellId, spell] : player->GetSpellMap())
            if (spell->State != PLAYERSPELL_REMOVED && spell->Active && (spell->specMask & spec)
                && spells.size() < 16000)
                spells.push_back(spellId);
        std::sort(spells.begin(), spells.end());
        F::KnownSpells record{ now, state.Id, spells.data(), uint16(spells.size()) };
        Writer().Write(F::Stream::Session, CaptureWriter::MAP_ALL, record);
    }

    /// The player's session, started the first time a hook sees it with capture on: at login for players, at
    /// their first update for companions (whose loading need not pass through a login).
    PlayerState* Begin(Player* player)
    {
        PlayerState* state = player->CustomData.GetDefault<PlayerState>(STATE_KEY);
        if (state->Started)
            return state;

        uint64 const now = CaptureWriter::NowMs();
        state->Started = true;
        state->Id = IdOf(player);
        state->Session = Writer().SessionId(player->GetGUID().GetRawValue(), now);
        state->Kind = sAnimusMod->IsBot(player->GetGUID()) ? 1 : 0;
        RefreshContext(player, *state);
        state->GroupSig = GroupSignature(player);
        state->LastLatencyMs = now;
        state->LastSpellsMs = now;
        state->Speeds = SpeedsOf(player);

        WriteSession(player, *state, now, false);
        WriteKnownSpells(player, *state, now);
        WriteGroup(player, *state, now);
        WriteSpeeds(player, *state, now);
        return state;
    }

    uint32 MountSpellOf(Player const* player)
    {
        if (!player->IsMounted())
            return 0;
        Unit::AuraEffectList const& mounts = player->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
        return mounts.empty() ? 1 : mounts.front()->GetId();
    }

    /// The MoverState record, when anything it carries changed since the last one (or there was none): who moves
    /// (kind, class, race, level), where (map, zone), and under what (mount, form, combat; a companion's model and
    /// move block revision). Players and companions alike, so their motion is compared like with like.
    void PollMover(Player* player, PlayerState& state, uint64 now)
    {
        uint32 const mount = MountSpellOf(player);
        uint32 const form = player->GetShapeshiftForm();
        bool const combat = player->IsInCombat();
        if (state.MoverWritten && mount == state.MoverMount && form == state.MoverForm && combat == state.MoverCombat
            && state.Level == state.MoverLevel && state.Map == state.MoverMap && state.Zone == state.MoverZone
            && state.Model == state.MoverModel && state.MoveRevision == state.MoverRevision && state.Kind
            == state.MoverKind)
            return;
        state.MoverWritten = true;
        state.MoverMount = mount;
        state.MoverForm = form;
        state.MoverCombat = combat;
        state.MoverLevel = state.Level;
        state.MoverMap = state.Map;
        state.MoverZone = state.Zone;
        state.MoverModel = state.Model;
        state.MoverRevision = state.MoveRevision;
        state.MoverKind = state.Kind;

        F::MoverState record;
        record.Ms = now;
        record.Player = state.Id;
        record.Kind = state.Kind;
        record.Class = player->getClass();
        record.Race = player->getRace();
        record.Level = state.Level;
        record.Map = state.Map;
        record.Zone = state.Zone;
        record.Mount = mount;
        record.Form = form;
        record.InCombat = combat ? 1 : 0;
        record.MoveRevision = state.Kind ? state.MoveRevision : 0;
        record.Model = state.Kind ? std::string_view(state.Model) : std::string_view();
        Writer().Write(F::Stream::Move, player->GetMapId(), record);
    }

    constexpr uint64 TALLY_EVERY_MS = 60000;

    /// The MoveTally record when the counts moved since the last one (or `force`, at the session's end).
    void WriteTally(Player* player, PlayerState& state, uint64 now, bool force)
    {
        if (!force && (now - state.LastTallyMs < TALLY_EVERY_MS
            || (state.MovesSent == state.TallySent && state.MovesKept == state.TallyKept)))
            return;
        if (force && !state.MovesSent && !state.MovesKept)
            return;
        state.LastTallyMs = now;
        state.TallySent = state.MovesSent;
        state.TallyKept = state.MovesKept;
        Writer().Write(F::Stream::Move, player->GetMapId(),
            F::MoveTally{ now, state.Id, state.Kind, state.MovesSent, state.MovesKept });
    }

    /// A MapUpdate for the map instance `player` is on, the first time one of its movers updates in this world tick
    /// (each map's players update on its own thread, every world tick, with the same diff). Keyed per thread by the
    /// map and the tick's game time, so a map is written once a tick whichever thread updates it.
    void WriteMapUpdate(Player* player, uint32 diff)
    {
        if (!Writer().On(F::Stream::Move) || !player->IsInWorld())
            return;
        Map const* map = player->FindMap();
        if (!map)
            return;
        struct Last
        {
            uint64 Tick = 0;
            uint32 Instance = 0;
        };
        thread_local std::unordered_map<Map const*, Last> written;
        uint64 const tick = uint64(GameTime::GetGameTimeMS().count());
        Last& last = written[map];
        if (last.Tick == tick && last.Instance == map->GetInstanceId())
            return;
        last.Tick = tick;
        last.Instance = map->GetInstanceId();
        if (written.size() > 4096)
        {
            // Maps come and go (instances): keep only this tick's.
            std::erase_if(written, [tick](auto const& entry) { return entry.second.Tick != tick; });
        }
        Writer().Write(F::Stream::Move, map->GetId(),
            F::MapUpdate{ CaptureWriter::NowMs(), map->GetId(), map->GetInstanceId(), diff });
    }

    /// Edges of what moves the player without its keys, and of its mode, polled each update.
    void PollMotion(Player* player, PlayerState& state)
    {
        uint64 const id = state.Id;

        uint32 mount = 0;
        if (player->IsMounted())
        {
            Unit::AuraEffectList const& mounts = player->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
            mount = mounts.empty() ? 1 : mounts.front()->GetId();
        }
        if (mount != state.Mount)
        {
            if (state.Mount)
                WriteMotion(player, id, F::Motion::Dismount, state.Mount);
            if (mount)
                WriteMotion(player, id, F::Motion::Mount, mount);
            state.Mount = mount;
        }

        bool const taxi = player->IsInFlight();
        if (taxi != state.Taxi)
        {
            WriteMotion(player, id, taxi ? F::Motion::TaxiStart : F::Motion::TaxiEnd,
                taxi ? player->m_taxi.GetCurrentTaxiPath() : 0);
            state.Taxi = taxi;
        }

        auto const edge = [&](bool now, bool& was, F::Motion on, F::Motion off)
        {
            if (now != was)
            {
                WriteMotion(player, id, now ? on : off);
                was = now;
            }
        };
        edge(player->HasUnitState(UNIT_STATE_ROOT), state.Rooted, F::Motion::Root, F::Motion::Unroot);
        edge(player->HasUnitState(UNIT_STATE_STUNNED), state.Stunned, F::Motion::StunStart, F::Motion::StunEnd);
        edge(player->HasUnitState(UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED), state.Feared, F::Motion::FearStart,
            F::Motion::FearEnd);

        uint32 const form = player->GetShapeshiftForm();
        if (form != state.Form)
        {
            WriteMotion(player, id, F::Motion::Shapeshift, form);
            state.Form = form;
        }

        Unit const* vehicleBase = player->GetVehicleBase();
        uint32 const vehicle = vehicleBase ? std::max<uint32>(1, vehicleBase->GetEntry()) : 0;
        if (vehicle != state.Vehicle)
        {
            if (state.Vehicle)
                WriteMotion(player, id, F::Motion::VehicleExit, state.Vehicle);
            if (vehicle)
                WriteMotion(player, id, F::Motion::VehicleEnter, vehicle);
            state.Vehicle = vehicle;
        }

        Transport const* transport = player->GetTransport();
        uint32 const transportEntry = transport ? std::max<uint32>(1, transport->GetEntry()) : 0;
        if (transportEntry != state.Transport)
        {
            if (state.Transport)
                WriteMotion(player, id, F::Motion::TransportLeave, state.Transport);
            if (transportEntry)
                WriteMotion(player, id, F::Motion::TransportBoard, transportEntry);
            state.Transport = transportEntry;
        }
    }

    float BreathOf(Player* player, PlayerState& state, uint64 now)
    {
        if (!player->IsUnderWater() || player->HasAuraType(SPELL_AURA_WATER_BREATHING))
        {
            state.SubmergedSinceMs = 0;
            return 1.0f;
        }
        if (!state.SubmergedSinceMs)
            state.SubmergedSinceMs = now;
        return std::max(0.0f, 1.0f - float(now - state.SubmergedSinceMs) / BREATH_MS);
    }

    struct Candidate
    {
        Unit* U = nullptr;
        float Distance = 0.0f;
        bool Always = false;                // a party member or the player's pet: always listed
        bool Hostile = false;               // hostile or neutral
    };

    F::SnapshotUnit UnitOf(Player* player, Unit* unit, uint8 extraFlags)
    {
        F::SnapshotUnit record;
        record.Unit = IdOf(unit);
        record.Entry = unit->IsPlayer() ? 0 : unit->GetEntry();
        record.Kind = KindOf(player, unit);
        record.X = unit->GetPositionX();
        record.Y = unit->GetPositionY();
        record.Z = unit->GetPositionZ();
        record.O = unit->GetOrientation();
        VelocityOf(unit, record.VX, record.VY, record.VZ);
        record.HealthPct = unit->GetHealthPct();
        record.PowerPct = unit->GetPowerPct(unit->getPowerType());
        record.Level = unit->GetLevel();
        bool const hostile = player->IsHostileTo(unit);
        record.Reaction = hostile ? 0 : player->IsFriendlyTo(unit) ? 2 : 1;
        record.Flags = extraFlags;
        if (unit->IsInCombat())
            record.Flags |= 1;
        if (unit->IsNonMeleeSpellCast(false))
            record.Flags |= 2;
        if (unit->IsPlayer() && sAnimusMod->IsBot(unit->GetGUID()))
            record.Flags |= 16;
        record.CastingSpell = CastingSpell(unit);
        record.Target = IdOf(unit->GetTarget());
        if (unit->IsCreature() && unit->CanHaveThreatList())
            record.ThreatOnPlayer = unit->GetThreatMgr().GetThreat(player);
        return record;
    }

    void WriteSnapshot(Player* player, PlayerState& state, uint64 now)
    {
        F::SnapshotSelf self;
        self.X = player->GetPositionX();
        self.Y = player->GetPositionY();
        self.Z = player->GetPositionZ();
        self.O = player->GetOrientation();
        self.Pitch = player->m_movementInfo.pitch;
        self.Map = player->GetMapId();
        self.HealthPct = player->GetHealthPct();
        self.PowerPct = player->GetPowerPct(player->getPowerType());
        self.PowerType = uint8(player->getPowerType());
        if (player->IsInCombat())
            self.Flags |= 1;
        if (player->HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING))
            self.Flags |= 2;
        if (player->IsFlying())
            self.Flags |= 4;
        if (player->IsMounted())
            self.Flags |= 8;
        if (player->IsNonMeleeSpellCast(false))
            self.Flags |= 16;
        if (!player->IsAlive())
            self.Flags |= 32;
        if (player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING))
            self.Flags |= 64;
        self.CastingSpell = CastingSpell(player);
        self.BreathPct = BreathOf(player, state, now);
        self.Target = IdOf(player->GetTarget());
        self.ShapeshiftForm = player->GetShapeshiftForm();

        // Party members on this map and the pet always; then the nearest 24 hostile (or neutral) and 10 friendly
        // units the client has within 40 yards. Party members on other maps are another thread's to read.
        thread_local std::vector<Candidate> candidates;
        candidates.clear();
        Group const* group = player->GetGroup();
        if (group)
            for (Group::MemberSlot const& slot : group->GetMemberSlots())
                if (Player* member = ObjectAccessor::GetPlayer(*player, slot.guid); member && member != player)
                    candidates.push_back({ member, player->GetExactDist(member), true, false });
        if (Pet* pet = player->GetPet())
            candidates.push_back({ pet, player->GetExactDist(pet), true, false });

        ObjectGuid const playerGuid = player->GetGUID();
        player->DoForAllVisibleWorldObjects([&](WorldObject* object)
        {
            Unit* unit = object ? object->ToUnit() : nullptr;
            if (!unit || unit == player || !unit->IsInWorld())
                return;
            if (unit->GetOwnerGUID() == playerGuid && unit->IsPet())
                return;                     // the pet, already listed
            if (group && unit->IsPlayer() && player->IsInSameGroupWith(unit->ToPlayer()))
                return;                     // a party member, already listed
            float const distance = player->GetExactDist(unit);
            if (distance > UNIT_RANGE)
                return;
            candidates.push_back({ unit, distance, false, !player->IsFriendlyTo(unit) });
        });
        std::sort(candidates.begin(), candidates.end(),
            [](Candidate const& a, Candidate const& b) { return a.Distance < b.Distance; });

        if (!Writer().On(F::Stream::Snapshot))
            return;
        F::Out& out = Writer().Scratch();
        F::SnapshotBuilder snapshot(out, now, state.Id, self);
        uint32 hostile = 0;
        uint32 friendly = 0;
        for (Candidate const& candidate : candidates)
        {
            uint8 flags = 0;
            if (candidate.Always)
                flags = candidate.U->IsPlayer() ? 4 : 8;
            else if (candidate.Hostile ? hostile++ >= MAX_HOSTILE : friendly++ >= MAX_FRIENDLY)
                continue;
            snapshot.Unit(UnitOf(player, candidate.U, flags));
        }

        for (auto const& [spellId, application] : player->GetAppliedAuras())
        {
            Aura const* aura = application->GetBase();
            snapshot.Aura({ aura->GetId(), uint8(std::min<uint32>(255, aura->GetStackAmount())), aura->GetDuration(),
                uint8(application->IsPositive() ? 1 : 0) });
        }

        uint32 const msNow = getMSTime();
        for (auto const& [spellId, cooldown] : player->GetSpellCooldownMap())
            if (cooldown.end > msNow)
                snapshot.Cooldown({ spellId, cooldown.end - msNow });

        snapshot.Finish();
        Writer().Commit(F::Stream::Snapshot, player->GetMapId(), out);
    }

    /// Everything polled at a player's update: its session's context, latency and spells, its speeds and motion
    /// edges, and its snapshot when due.
    void Update(Player* player)
    {
        if (!player->IsInWorld())
            return;
        PlayerState* state = Begin(player);
        uint64 const now = CaptureWriter::NowMs();

        if (!state->Kind && sAnimusMod->IsBot(player->GetGUID()))
        {
            // A companion whose login came before its party knew it.
            state->Kind = 1;
            WriteSession(player, *state, now, true);
        }

        uint64 const groupSig = GroupSignature(player);
        bool const groupChanged = groupSig != state->GroupSig;
        state->GroupSig = groupSig;
        if (RefreshContext(player, *state) || groupChanged)
            WriteSession(player, *state, now, true);
        if (groupChanged)
            WriteGroup(player, *state, now);

        if (now - state->LastLatencyMs >= LATENCY_EVERY_MS)
        {
            state->LastLatencyMs = now;
            if (!state->Kind && player->GetSession())
                Writer().Write(F::Stream::Session, CaptureWriter::MAP_ALL, F::Latency{ now, state->Id,
                    uint16(std::min<uint32>(0xFFFF, player->GetSession()->GetLatency())) });
        }

        if (state->SpellsDirty && now - state->LastSpellsMs >= SPELLS_EVERY_MS)
        {
            state->SpellsDirty = false;
            state->LastSpellsMs = now;
            WriteKnownSpells(player, *state, now);
        }

        if (Writer().On(F::Stream::Move))
        {
            std::array<float, 9> const speeds = SpeedsOf(player);
            if (speeds != state->Speeds)
            {
                state->Speeds = speeds;
                WriteSpeeds(player, *state, now);
            }
            PollMotion(player, *state);
            PollMover(player, *state, now);
            WriteTally(player, *state, now, false);
        }

        // Companions' own decisions and motion are the companion stream's; snapshots are of the players.
        if (state->Kind || !Writer().On(F::Stream::Snapshot))
            return;
        bool const active = player->IsInCombat() || player->isMoving();
        uint32 const interval = active ? Writer().SnapshotMs() : Writer().IdleSnapshotMs();
        if (now - state->LastSnapshotMs >= interval)
        {
            state->LastSnapshotMs = now;
            WriteSnapshot(player, *state, now);
        }
    }

    bool AnyOn()
    {
        for (F::Stream stream : { F::Stream::Session, F::Stream::Move, F::Stream::Action, F::Stream::Snapshot,
            F::Stream::Outcome, F::Stream::Companion })
            if (Writer().On(stream))
                return true;
        return false;
    }

    /// A packet's payload, read without moving its read position (the hooks get it const, before its handler).
    class PacketReader
    {
    public:
        explicit PacketReader(WorldPacket const& packet)
            : _data(packet.size() ? packet.contents() : nullptr), _size(packet.size()) { }

        template <typename T>
        bool Read(T& value)
        {
            if (_offset + sizeof(T) > _size)
                return false;
            std::memcpy(&value, _data + _offset, sizeof(T));
            _offset += sizeof(T);
            return true;
        }

        bool ReadPacked(ObjectGuid& guid)
        {
            uint8 mask = 0;
            if (!Read(mask))
                return false;
            uint64 raw = 0;
            for (uint32 i = 0; i < 8; ++i)
            {
                if (!(mask & (1 << i)))
                    continue;
                uint8 byte = 0;
                if (!Read(byte))
                    return false;
                raw |= uint64(byte) << (i * 8);
            }
            guid = ObjectGuid(raw);
            return true;
        }

        bool Skip(std::size_t bytes)
        {
            if (_offset + bytes > _size)
                return false;
            _offset += bytes;
            return true;
        }

    private:
        uint8 const* _data;
        std::size_t _size;
        std::size_t _offset = 0;
    };

    /// SpellCastTargets as the client sends them (SpellCastTargets::Read): the object target and the destination.
    void ReadTargets(PacketReader& in, ObjectGuid& object, bool& hasDest, float& x, float& y, float& z)
    {
        uint32 mask = 0;
        if (!in.Read(mask) || !mask)
            return;
        if (mask & (TARGET_FLAG_UNIT | TARGET_FLAG_UNIT_MINIPET | TARGET_FLAG_GAMEOBJECT | TARGET_FLAG_CORPSE_ENEMY
            | TARGET_FLAG_CORPSE_ALLY))
            if (!in.ReadPacked(object))
                return;
        ObjectGuid ignored;
        if (mask & (TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM))
            if (!in.ReadPacked(ignored))
                return;
        if (mask & TARGET_FLAG_SOURCE_LOCATION)
            if (!in.ReadPacked(ignored) || !in.Skip(12))
                return;
        if (mask & TARGET_FLAG_DEST_LOCATION)
            hasDest = in.ReadPacked(ignored) && in.Read(x) && in.Read(y) && in.Read(z);
    }

    void WriteInteract(Player* player, F::Interaction what, ObjectGuid target, uint32 arg)
    {
        WorldObject const* object = target ? ObjectAccessor::GetWorldObject(*player, target) : nullptr;
        F::Interact record{ CaptureWriter::NowMs(), PlayerIdOf(player), what, object ? object->GetEntry() : 0,
            IdOf(target), arg };
        Writer().Write(F::Stream::Action, player->GetMapId(), record);
    }

    void HandleClientPacket(Player* player, WorldPacket const& packet)
    {
        PacketReader in(packet);
        uint64 const now = CaptureWriter::NowMs();
        uint32 const map = player->GetMapId();
        switch (packet.GetOpcode())
        {
            case CMSG_CAST_SPELL:
            {
                uint8 castCount = 0;
                uint8 castFlags = 0;
                F::CastRequest record;
                if (!in.Read(castCount) || !in.Read(record.Spell) || !in.Read(castFlags))
                    return;
                ObjectGuid target;
                bool hasDest = false;
                ReadTargets(in, target, hasDest, record.TX, record.TY, record.TZ);
                record.Ms = now;
                record.Player = PlayerIdOf(player);
                record.Target = IdOf(target);
                if (target)
                    record.Kind = KindOf(player, ObjectAccessor::GetWorldObject(*player, target));
                else if (hasDest)
                    record.Kind = F::TargetKind::Ground;
                if (target && !hasDest)
                    if (WorldObject const* object = ObjectAccessor::GetWorldObject(*player, target))
                    {
                        record.TX = object->GetPositionX();
                        record.TY = object->GetPositionY();
                        record.TZ = object->GetPositionZ();
                    }
                SpellInfo const* info = sSpellMgr->GetSpellInfo(record.Spell);
                record.GcdActive = info && player->GetGlobalCooldownMgr().HasGlobalCooldown(info) ? 1 : 0;
                record.Casting = player->IsNonMeleeSpellCast(false) ? 1 : 0;
                record.Power = player->GetPower(player->getPowerType());
                record.PowerType = uint8(player->getPowerType());
                Writer().Write(F::Stream::Action, map, record);
                return;
            }
            case CMSG_CANCEL_CAST:
            case CMSG_CANCEL_CHANNELLING:
            {
                uint8 counter = 0;
                uint32 spell = 0;
                if ((packet.GetOpcode() == CMSG_CANCEL_CAST && !in.Read(counter)) || !in.Read(spell))
                    return;
                if (PlayerState* state = StateOf(player))
                {
                    state->CancelSpell = spell;
                    state->CancelMs = now;
                }
                return;
            }
            case CMSG_SET_SELECTION:
            {
                uint64 raw = 0;
                if (!in.Read(raw))
                    return;
                ObjectGuid const target(raw);
                WorldObject const* object = target ? ObjectAccessor::GetWorldObject(*player, target) : nullptr;
                F::Select record{ now, PlayerIdOf(player), IdOf(target), KindOf(player, object),
                    object ? player->GetExactDist(object) : 0.0f };
                Writer().Write(F::Stream::Action, map, record);
                return;
            }
            case CMSG_USE_ITEM:
            {
                uint8 bag = 0;
                uint8 slot = 0;
                uint8 castCount = 0;
                uint32 spell = 0;
                uint64 itemGuid = 0;
                uint32 glyph = 0;
                uint8 castFlags = 0;
                if (!in.Read(bag) || !in.Read(slot) || !in.Read(castCount) || !in.Read(spell) || !in.Read(itemGuid)
                    || !in.Read(glyph) || !in.Read(castFlags))
                    return;
                ObjectGuid target;
                bool hasDest = false;
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                ReadTargets(in, target, hasDest, x, y, z);
                Item const* item = player->GetItemByPos(bag, slot);
                F::ItemUse record{ now, PlayerIdOf(player), item ? item->GetEntry() : 0, spell, IdOf(target) };
                Writer().Write(F::Stream::Action, map, record);
                return;
            }
            case CMSG_ATTACKSWING:
            {
                uint64 raw = 0;
                if (!in.Read(raw))
                    return;
                Writer().Write(F::Stream::Action, map, F::Attack{ now, PlayerIdOf(player), IdOf(ObjectGuid(raw)), 1 });
                return;
            }
            case CMSG_ATTACKSTOP:
            {
                Unit const* victim = player->GetVictim();
                Writer().Write(F::Stream::Action, map, F::Attack{ now, PlayerIdOf(player), IdOf(victim), 0 });
                return;
            }
            default:
                break;
        }

        // Interactions: every one of these starts with the npc's or object's guid.
        uint64 raw = 0;
        if (!in.Read(raw))
            return;
        ObjectGuid const target(raw);
        uint32 arg = 0;
        switch (packet.GetOpcode())
        {
            case CMSG_GOSSIP_HELLO: WriteInteract(player, F::Interaction::Gossip, target, 0); return;
            case CMSG_LOOT: WriteInteract(player, F::Interaction::Loot, target, 0); return;
            case CMSG_QUESTGIVER_ACCEPT_QUEST:
                in.Read(arg);
                WriteInteract(player, F::Interaction::QuestAccept, target, arg);
                return;
            case CMSG_QUESTGIVER_CHOOSE_REWARD:
                in.Read(arg);
                WriteInteract(player, F::Interaction::QuestTurnIn, target, arg);
                return;
            case CMSG_GAMEOBJ_USE: WriteInteract(player, F::Interaction::GameObjectUse, target, 0); return;
            case CMSG_LIST_INVENTORY: WriteInteract(player, F::Interaction::Vendor, target, 0); return;
            case CMSG_TAXIQUERYAVAILABLENODES: WriteInteract(player, F::Interaction::FlightMaster, target, 0); return;
            case CMSG_GET_MAIL_LIST: WriteInteract(player, F::Interaction::Mailbox, target, 0); return;
            case MSG_AUCTION_HELLO: WriteInteract(player, F::Interaction::AuctionHouse, target, 0); return;
            case CMSG_TRAINER_LIST: WriteInteract(player, F::Interaction::Trainer, target, 0); return;
            case CMSG_BANKER_ACTIVATE: WriteInteract(player, F::Interaction::Bank, target, 0); return;
            case CMSG_BINDER_ACTIVATE: WriteInteract(player, F::Interaction::Innkeeper, target, 0); return;
            default: return;
        }
    }

    bool IsCapturedOpcode(uint16 opcode)
    {
        switch (opcode)
        {
            case CMSG_CAST_SPELL:
            case CMSG_CANCEL_CAST:
            case CMSG_CANCEL_CHANNELLING:
            case CMSG_SET_SELECTION:
            case CMSG_USE_ITEM:
            case CMSG_ATTACKSWING:
            case CMSG_ATTACKSTOP:
            case CMSG_GOSSIP_HELLO:
            case CMSG_LOOT:
            case CMSG_QUESTGIVER_ACCEPT_QUEST:
            case CMSG_QUESTGIVER_CHOOSE_REWARD:
            case CMSG_GAMEOBJ_USE:
            case CMSG_LIST_INVENTORY:
            case CMSG_TAXIQUERYAVAILABLENODES:
            case CMSG_GET_MAIL_LIST:
            case MSG_AUCTION_HELLO:
            case CMSG_TRAINER_LIST:
            case CMSG_BANKER_ACTIVATE:
            case CMSG_BINDER_ACTIVATE:
                return true;
            default:
                return false;
        }
    }

    // ---- scripts --------------------------------------------------------------------------------------------------

    class CaptureWorldScript : public WorldScript
    {
    public:
        CaptureWorldScript() : WorldScript("AnimusCaptureWorldScript",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_AFTER_UNLOAD_ALL_MAPS }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            CaptureConfig config;
            config.Load();
            sCaptureWriter->Configure(config);
        }

        /// After every player's logout (KickAll runs before the maps unload) and every map thread has stopped.
        void OnAfterUnloadAllMaps() override { sCaptureWriter->Stop(); }
    };

    class CapturePlayerScript : public PlayerScript
    {
    public:
        CapturePlayerScript() : PlayerScript("AnimusCapturePlayerScript",
            { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_UPDATE,
                PLAYERHOOK_ON_PLAYER_LEARN_TALENTS, PLAYERHOOK_ON_TALENTS_RESET, PLAYERHOOK_ON_AFTER_SPEC_SLOT_CHANGED,
                PLAYERHOOK_ON_EQUIP, PLAYERHOOK_ON_UNEQUIP_ITEM, PLAYERHOOK_ON_LEARN_SPELL, PLAYERHOOK_ON_UPDATE_ZONE,
                PLAYERHOOK_ON_UPDATE_AREA, PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_BEFORE_TELEPORT,
                PLAYERHOOK_ON_PLAYER_RESURRECT, PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_DUEL_END,
                PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT, PLAYERHOOK_ON_BEFORE_QUEST_COMPLETE,
                PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_QUEST_ABANDON }) { }

        void OnPlayerLogin(Player* player) override
        {
            if (!AnyOn())
                return;
            PlayerState const* state = Begin(player);
            if (!state->Kind && player->GetSession() && !sCaptureWriter->LoginNotice().empty())
                ChatHandler(player->GetSession()).SendSysMessage(sCaptureWriter->LoginNotice());
        }

        void OnPlayerLogout(Player* player) override
        {
            PlayerState* state = StateOf(player);
            if (!state || !state->Started)
                return;
            if (sCaptureWriter->On(F::Stream::Move))
                WriteTally(player, *state, CaptureWriter::NowMs(), true);
            if (!sCaptureWriter->On(F::Stream::Session))
                return;
            uint8 reason = 0;
            if (World::IsStopped())
                reason = 2;
            else if (!state->Kind && player->GetSession() && player->GetSession()->IsSocketClosed())
                reason = 1;
            sCaptureWriter->Write(F::Stream::Session, CaptureWriter::MAP_ALL,
                F::SessionEnd{ CaptureWriter::NowMs(), state->Id, state->Session, reason });
        }

        void OnPlayerUpdate(Player* player, uint32 diff) override
        {
            if (!AnyOn())
                return;
            WriteMapUpdate(player, diff);
            Update(player);
        }

        void OnPlayerLearnTalents(Player* player, uint32 /*talentId*/, uint32 /*rank*/, uint32 /*spell*/) override
        {
            if (PlayerState* state = StateOf(player))
                state->TalentsDirty = true;
        }

        void OnPlayerTalentsReset(Player* player, bool /*noCost*/) override
        {
            if (PlayerState* state = StateOf(player))
                state->TalentsDirty = true;
        }

        void OnPlayerAfterSpecSlotChanged(Player* player, uint8 /*slot*/) override
        {
            if (PlayerState* state = StateOf(player))
            {
                state->TalentsDirty = true;
                state->SpellsDirty = true;
            }
        }

        void OnPlayerEquip(Player* player, Item* /*item*/, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
        {
            if (PlayerState* state = StateOf(player))
                state->GearDirty = true;
        }

        void OnPlayerUnequip(Player* player, Item* /*item*/) override
        {
            if (PlayerState* state = StateOf(player))
                state->GearDirty = true;
        }

        void OnPlayerLearnSpell(Player* player, uint32 /*spell*/) override
        {
            if (PlayerState* state = StateOf(player))
                state->SpellsDirty = true;
        }

        void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override
        {
            WriteArea(player, newZone, newArea);
        }

        void OnPlayerUpdateArea(Player* player, uint32 /*oldArea*/, uint32 newArea) override
        {
            WriteArea(player, player->GetZoneId(), newArea);
        }

        void OnPlayerMapChanged(Player* player) override
        {
            PlayerState* state = StateOf(player);
            if (!state || !sCaptureWriter->On(F::Stream::Move))
                return;
            if (state->Loading)
            {
                state->Loading = false;
                WriteMotion(player, state->Id, F::Motion::LoadingEnd, player->GetMapId());
            }
        }

        bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float x, float y, float z, float /*o*/,
            uint32 /*options*/, Unit* /*target*/) override
        {
            if (!sCaptureWriter->On(F::Stream::Move))
                return true;
            PlayerState* state = StateOf(player);
            uint64 const id = state ? state->Id : IdOf(player);
            if (mapId != player->GetMapId())
            {
                WriteMotion(player, id, F::Motion::LoadingStart, mapId);
                if (state)
                    state->Loading = true;
            }
            WriteMotion(player, id, F::Motion::Teleport, mapId, x, y, z, mapId);
            return true;
        }

        void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/) override
        {
            if (sCaptureWriter->On(F::Stream::Move))
                WriteMotion(player, PlayerIdOf(player), F::Motion::Resurrect);
        }

        void OnPlayerPVPKill(Player* killer, Player* killed) override
        {
            if (!sCaptureWriter->On(F::Stream::Outcome) || !killer || !killed)
                return;
            uint64 const now = CaptureWriter::NowMs();
            sCaptureWriter->Write(F::Stream::Outcome, killer->GetMapId(),
                F::PvP{ now, PlayerIdOf(killer), PlayerIdOf(killed), 0 });
            sCaptureWriter->Write(F::Stream::Outcome, killed->GetMapId(),
                F::PvP{ now, PlayerIdOf(killed), PlayerIdOf(killer), 1 });
        }

        void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
        {
            if (type != DUEL_WON || !winner || !loser || !sCaptureWriter->On(F::Stream::Outcome))
                return;
            uint64 const now = CaptureWriter::NowMs();
            sCaptureWriter->Write(F::Stream::Outcome, winner->GetMapId(),
                F::PvP{ now, PlayerIdOf(winner), PlayerIdOf(loser), 2 });
            sCaptureWriter->Write(F::Stream::Outcome, loser->GetMapId(),
                F::PvP{ now, PlayerIdOf(loser), PlayerIdOf(winner), 3 });
        }

        void OnPlayerQuestAccept(Player* player, Quest const* quest) override
        {
            WriteQuest(player, quest->GetQuestId(), 0);
        }

        bool OnPlayerBeforeQuestComplete(Player* player, uint32 questId) override
        {
            WriteQuest(player, questId, 1);
            return true;
        }

        /// Called from Player::RewardQuest: the quest was handed in and rewarded.
        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            WriteQuest(player, quest->GetQuestId(), 2);
        }

        void OnPlayerQuestAbandon(Player* player, uint32 questId) override { WriteQuest(player, questId, 3); }

    private:
        static void WriteArea(Player* player, uint32 zone, uint32 area)
        {
            if (sCaptureWriter->On(F::Stream::Outcome))
                sCaptureWriter->Write(F::Stream::Outcome, player->GetMapId(),
                    F::Area{ CaptureWriter::NowMs(), PlayerIdOf(player), player->GetMapId(), zone, area });
        }

        static void WriteQuest(Player* player, uint32 questId, uint8 event)
        {
            if (sCaptureWriter->On(F::Stream::Outcome))
                sCaptureWriter->Write(F::Stream::Outcome, player->GetMapId(),
                    F::Quest{ CaptureWriter::NowMs(), PlayerIdOf(player), questId, event });
        }
    };

    class CaptureMovementScript : public MovementHandlerScript
    {
    public:
        CaptureMovementScript() : MovementHandlerScript("AnimusCaptureMovementScript",
            { MOVEMENTHOOK_ON_PLAYER_MOVE }) { }

        /// Every movement packet the client sends, before the server adjusts its time (MovementHandler.cpp):
        /// `client_ms` is the client's own clock.
        void OnPlayerMove(Player* player, MovementInfo info, uint32 opcode) override
        {
            if (!sCaptureWriter->On(F::Stream::Move))
                return;
            uint64 const id = PlayerIdOf(player);
            F::Move record;
            record.Ms = CaptureWriter::NowMs();
            record.Player = id;
            record.ClientMs = info.time;
            record.Opcode = uint16(opcode);
            record.MoveFlags = info.GetMovementFlags();
            record.MoveFlags2 = info.GetExtraMovementFlags();
            record.X = info.pos.GetPositionX();
            record.Y = info.pos.GetPositionY();
            record.Z = info.pos.GetPositionZ();
            record.O = info.pos.GetOrientation();
            record.Pitch = info.pitch;
            record.FallMs = info.fallTime;
            record.JumpZSpeed = info.jump.zspeed;
            record.JumpSin = info.jump.sinAngle;
            record.JumpCos = info.jump.cosAngle;
            record.JumpXYSpeed = info.jump.xyspeed;
            record.Map = player->GetMapId();
            record.ServerMs = getMSTime();
            // The same handler, the same point and the same fields for both: a companion's packet is its player
            // controller's, reported through its session as a client's is (CompanionClient), with its own clock.
            PlayerState const* state = StateOf(player);
            bool const companion = state ? state->Kind != 0 : sAnimusMod->IsBot(player->GetGUID());
            record.Source = uint8(companion ? F::MoveSource::ControllerPacket : F::MoveSource::ClientPacket);
            sCaptureWriter->Write(F::Stream::Move, record.Map, record);
            if (PlayerState* kept = StateOf(player))
                ++kept->MovesKept;

            if (opcode == CMSG_MOVE_KNOCK_BACK_ACK)
                WriteMotion(player, id, F::Motion::Knockback, 0, record.X, record.Y, record.Z, record.Map);
        }
    };

    class CaptureServerScript : public ServerScript
    {
    public:
        CaptureServerScript() : ServerScript("AnimusCaptureServerScript",
            { SERVERHOOK_CAN_PACKET_RECEIVE, SERVERHOOK_ON_PACKET_SENT }) { }

        /// Never refuses a packet: it only looks.
        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            // A player's movement packet about to reach its handler (MoveTally's `sent`). A companion's never come
            // this way: its client calls the handlers itself and counts there (Capture::MovementSent).
            if (Animus::Capture::ReachesMoveHook(packet.GetOpcode()) && sCaptureWriter->On(F::Stream::Move))
                if (Player* player = session ? session->GetPlayer() : nullptr; player && player->IsInWorld())
                    if (PlayerState* state = StateOf(player))
                        ++state->MovesSent;

            if (!IsCapturedOpcode(packet.GetOpcode()) || !sCaptureWriter->On(F::Stream::Action))
                return true;
            if (Player* player = session ? session->GetPlayer() : nullptr; player && player->IsInWorld())
                HandleClientPacket(player, packet);
            return true;
        }

        /// A cast refused (SMSG_CAST_FAILED: u8 count, u32 spell, u8 result), and the environmental damage a
        /// player takes (SMSG_ENVIRONMENTAL_DAMAGE_LOG: u64 victim, u8 type), kept for its death's cause. Before the
        /// socket check, so companions' casts count too.
        void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
        {
            uint16 const opcode = packet.GetOpcode();
            if (opcode != SMSG_CAST_FAILED && opcode != SMSG_ENVIRONMENTAL_DAMAGE_LOG)
                return;
            Player* player = session ? session->GetPlayer() : nullptr;
            if (!player || !player->IsInWorld())
                return;
            PacketReader in(packet);
            if (opcode == SMSG_CAST_FAILED)
            {
                uint8 count = 0;
                F::CastOutcome record;
                if (!sCaptureWriter->On(F::Stream::Action) || !in.Read(count) || !in.Read(record.Spell)
                    || !in.Read(record.Value))
                    return;
                record.Ms = CaptureWriter::NowMs();
                record.Player = PlayerIdOf(player);
                F::Out& out = sCaptureWriter->Scratch();
                record.Write(out, F::Type::CastResult);
                sCaptureWriter->Commit(F::Stream::Action, player->GetMapId(), out);
                return;
            }

            uint64 victim = 0;
            uint8 type = 0;
            if (!in.Read(victim) || !in.Read(type) || victim != player->GetGUID().GetRawValue())
                return;
            if (PlayerState* state = StateOf(player))
            {
                state->LastEnvType = type;
                state->LastEnvMs = CaptureWriter::NowMs();
            }
        }
    };

    class CaptureSpellScript : public AllSpellScript
    {
    public:
        CaptureSpellScript() : AllSpellScript("AnimusCaptureSpellScript",
            { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL }) { }

        /// A player's cast went off: CastResult "go", and the end of a cast that took time.
        void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool /*skipCheck*/) override
        {
            if (!caster || !caster->IsPlayer() || spell->IsTriggered() || !sCaptureWriter->On(F::Stream::Action))
                return;
            Player* player = caster->ToPlayer();
            F::CastOutcome record{ CaptureWriter::NowMs(), PlayerIdOf(player), info->Id, F::CAST_GO };
            Write(player, record, F::Type::CastResult);
            if (spell->GetCastTime() > 0 || info->IsChanneled())
            {
                record.Value = 0;
                Write(player, record, F::Type::CastEnd);
            }
        }

        /// A cast stopped before it went off (or a channel cut short): moved, cancelled by the client, or
        /// interrupted by anything else.
        void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* info, bool /*bySelf*/) override
        {
            if (!caster || !caster->IsPlayer() || spell->IsTriggered() || !sCaptureWriter->On(F::Stream::Action))
                return;
            Player* player = caster->ToPlayer();
            uint64 const now = CaptureWriter::NowMs();
            uint8 how = 3;
            PlayerState const* state = StateOf(player);
            if (caster->isMoving() && (info->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT))
                how = 1;
            else if (state && state->CancelSpell == info->Id && now - state->CancelMs < 2000)
                how = 2;
            Write(player, F::CastOutcome{ now, PlayerIdOf(player), info->Id, how }, F::Type::CastEnd);
        }

    private:
        static void Write(Player* player, F::CastOutcome const& record, F::Type type)
        {
            F::Out& out = sCaptureWriter->Scratch();
            record.Write(out, type);
            sCaptureWriter->Commit(F::Stream::Action, player->GetMapId(), out);
        }
    };

    class CaptureUnitScript : public UnitScript
    {
    public:
        CaptureUnitScript() : UnitScript("AnimusCaptureUnitScript", true,
            { UNITHOOK_ON_HEAL, UNITHOOK_ON_UNIT_DEATH }) { }

        /// Every damage event (map threads), when either side is a player or a companion. The hook carries no
        /// spell, school, absorb or crit: those are written 0; a DoT tick is flagged periodic.
        uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType type) override
        {
            if (!damage || !victim || !sCaptureWriter->On(F::Stream::Outcome)
                || (!victim->IsPlayer() && !(attacker && attacker->IsPlayer())))
                return damage;
            uint32 const health = victim->GetHealth();
            F::Damage record{ CaptureWriter::NowMs(), IdOf(attacker), IdOf(victim), 0, damage, 0,
                damage > health ? damage - health : 0, 0, uint8(type == DOT ? 2 : 0) };
            sCaptureWriter->Write(F::Stream::Outcome, victim->GetMapId(), record);
            return damage;
        }

        /// Health actually restored (Unit::DealHeal), when either side is a player or a companion; spell and
        /// overheal are not in the hook and are written 0.
        void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override
        {
            if (!gain || !receiver || !sCaptureWriter->On(F::Stream::Outcome)
                || (!receiver->IsPlayer() && !(healer && healer->IsPlayer())))
                return;
            F::Heal record{ CaptureWriter::NowMs(), IdOf(healer), IdOf(receiver), 0, gain, 0, 0 };
            sCaptureWriter->Write(F::Stream::Outcome, receiver->GetMapId(), record);
        }

        void OnUnitDeath(Unit* unit, Unit* killer) override
        {
            if (!unit)
                return;
            uint64 const now = CaptureWriter::NowMs();
            Player* owner = killer ? killer->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            if (sCaptureWriter->On(F::Stream::Outcome) && (owner || unit->IsPlayer()))
            {
                F::Kill record{ now, IdOf(killer), IdOf(unit), unit->IsPlayer() ? 0 : unit->GetEntry(),
                    owner ? KindOf(owner, unit) : F::TargetKind::None };
                sCaptureWriter->Write(F::Stream::Outcome, unit->GetMapId(), record);
            }

            Player* player = unit->ToPlayer();
            if (!player)
                return;
            PlayerState const* state = StateOf(player);
            uint64 const id = state ? state->Id : IdOf(player);
            if (sCaptureWriter->On(F::Stream::Outcome))
            {
                uint8 cause = 5;
                if (killer && killer != unit)
                    cause = killer->GetCharmerOrOwnerOrOwnGUID().IsPlayer() ? 1 : 0;
                else if (state && state->LastEnvType != 0xFF && now - state->LastEnvMs < 2000)
                {
                    switch (state->LastEnvType)
                    {
                        case DAMAGE_FALL:
                        case DAMAGE_FALL_TO_VOID: cause = 2; break;
                        case DAMAGE_DROWNING: cause = 3; break;
                        case DAMAGE_LAVA:
                        case DAMAGE_FIRE: cause = 4; break;
                        default: cause = 5; break;
                    }
                }
                F::Death record{ now, id, killer && killer != unit ? IdOf(killer) : 0, cause, player->GetPositionX(),
                    player->GetPositionY(), player->GetPositionZ(), player->GetMapId() };
                sCaptureWriter->Write(F::Stream::Outcome, player->GetMapId(), record);
            }
            if (sCaptureWriter->On(F::Stream::Move))
                WriteMotion(player, id, F::Motion::Death);
        }
    };

    class CaptureGlobalScript : public GlobalScript
    {
    public:
        CaptureGlobalScript() : GlobalScript("AnimusCaptureGlobalScript", { GLOBALHOOK_ON_BEFORE_SET_BOSS_STATE }) { }

        /// An instance boss's state, for every player in the instance. `boss_entry` is the instance script's boss
        /// index (the hook has no creature).
        void OnBeforeSetBossState(uint32 id, EncounterState newState, EncounterState oldState, Map* instance) override
        {
            if (!instance || newState == oldState || oldState == TO_BE_DECIDED
                || !sCaptureWriter->On(F::Stream::Outcome))
                return;
            uint8 event = 0;
            if (newState == IN_PROGRESS)
                event = 0;
            else if (newState == DONE)
                event = 1;
            else if (newState == FAIL || (newState == NOT_STARTED && oldState == IN_PROGRESS))
                event = 2;
            else
                return;
            uint64 const now = CaptureWriter::NowMs();
            for (MapReference const& reference : instance->GetPlayers())
                if (Player* player = reference.GetSource())
                    sCaptureWriter->Write(F::Stream::Outcome, instance->GetId(), F::Encounter{ now, PlayerIdOf(player),
                        instance->GetId(), instance->GetInstanceId(), id, event });
        }
    };
}

void Animus::Capture::CompanionDecision(Player* bot, ObjectGuid owner, std::string_view model, float const* obs,
    std::size_t obsCount, int32 action, int32 goal, int32 goal2)
{
    if (!bot || !sCaptureWriter->On(F::Stream::Companion))
        return;
    uint16 const actions[1] = { uint16(action < 0 ? 0xFFFF : action) };
    F::CompanionDecision record{ CaptureWriter::NowMs(), PlayerIdOf(bot), IdOf(owner), model,
        F::Fnv1a(obs, obsCount * sizeof(float)), actions, 1, uint16(goal < 0 ? 0xFFFF : goal),
        uint16(goal2 < 0 ? 0xFFFF : goal2) };
    sCaptureWriter->Write(F::Stream::Companion, CaptureWriter::MAP_ALL, record);
}

bool Animus::Capture::ReachesMoveHook(uint16 opcode)
{
    // Every opcode whose handler runs ProcessMovementInfo, which calls OnPlayerMove when it keeps the packet
    // (Opcodes.cpp: HandleMovementOpcodes, the speed, root, knockback and flag acks, HandleDismissControlledVehicle).
    switch (opcode)
    {
        case MSG_MOVE_START_FORWARD: case MSG_MOVE_START_BACKWARD: case MSG_MOVE_STOP:
        case MSG_MOVE_START_STRAFE_LEFT: case MSG_MOVE_START_STRAFE_RIGHT: case MSG_MOVE_STOP_STRAFE:
        case MSG_MOVE_JUMP: case MSG_MOVE_START_TURN_LEFT: case MSG_MOVE_START_TURN_RIGHT: case MSG_MOVE_STOP_TURN:
        case MSG_MOVE_START_PITCH_UP: case MSG_MOVE_START_PITCH_DOWN: case MSG_MOVE_STOP_PITCH:
        case MSG_MOVE_SET_RUN_MODE: case MSG_MOVE_SET_WALK_MODE: case MSG_MOVE_FALL_LAND: case MSG_MOVE_START_SWIM:
        case MSG_MOVE_STOP_SWIM: case MSG_MOVE_SET_FACING: case MSG_MOVE_SET_PITCH: case MSG_MOVE_HEARTBEAT:
        case CMSG_MOVE_FALL_RESET: case CMSG_MOVE_SET_FLY: case MSG_MOVE_START_ASCEND: case MSG_MOVE_STOP_ASCEND:
        case MSG_MOVE_START_DESCEND: case CMSG_MOVE_CHNG_TRANSPORT:
        case CMSG_FORCE_RUN_SPEED_CHANGE_ACK: case CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK:
        case CMSG_FORCE_SWIM_SPEED_CHANGE_ACK: case CMSG_FORCE_WALK_SPEED_CHANGE_ACK:
        case CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK: case CMSG_FORCE_TURN_RATE_CHANGE_ACK:
        case CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK: case CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK:
        case CMSG_FORCE_PITCH_RATE_CHANGE_ACK: case CMSG_MOVE_SET_COLLISION_HGT_ACK:
        case CMSG_FORCE_MOVE_ROOT_ACK: case CMSG_FORCE_MOVE_UNROOT_ACK: case CMSG_MOVE_KNOCK_BACK_ACK:
        case CMSG_MOVE_HOVER_ACK: case CMSG_MOVE_FEATHER_FALL_ACK: case CMSG_MOVE_WATER_WALK_ACK:
        case CMSG_MOVE_SET_CAN_FLY_ACK: case CMSG_MOVE_GRAVITY_DISABLE_ACK: case CMSG_MOVE_GRAVITY_ENABLE_ACK:
        case CMSG_DISMISS_CONTROLLED_VEHICLE:
            return true;
        default:
            return false;
    }
}

void Animus::Capture::MovementSent(Player* bot, uint16 opcode)
{
    // The companion's map thread, as a player's packets are counted on theirs.
    if (!bot || !ReachesMoveHook(opcode) || !sCaptureWriter->On(F::Stream::Move))
        return;
    if (PlayerState* state = StateOf(bot))
        ++state->MovesSent;
}

void Animus::Capture::CompanionModel(Player* bot, std::string_view model, uint8 moveRevision)
{
    // World thread, while maps are idle (CompanionParty::Decide): the map thread's PollMover reads these.
    if (!bot || !bot->IsInWorld() || !sCaptureWriter->On(F::Stream::Move))
        return;
    PlayerState* state = bot->CustomData.GetDefault<PlayerState>(STATE_KEY);
    if (state->Model != model)
        state->Model.assign(model.substr(0, 32));
    state->MoveRevision = moveRevision;
}

void Animus::Capture::CompanionCommand(ObjectGuid owner, ObjectGuid companion, Command command, uint32 arg)
{
    if (!sCaptureWriter->On(F::Stream::Companion))
        return;
    sCaptureWriter->Write(F::Stream::Companion, CaptureWriter::MAP_ALL,
        F::CompanionCommand{ CaptureWriter::NowMs(), IdOf(owner), IdOf(companion), uint8(command), arg });
}

bool Animus::Capture::CompanionRating(ObjectGuid owner, ObjectGuid companion, std::string_view sign,
    std::string_view reason, std::string& message)
{
    int8 rating = 0;
    if (sign == "+" || sign == "up" || sign == "good")
        rating = 1;
    else if (sign == "-" || sign == "down" || sign == "bad")
        rating = -1;
    else
    {
        message = "Rate your companion with + or -, then optionally why: movement, combat, healing, tanking, stuck "
            "or other.";
        return false;
    }

    static constexpr std::array<std::string_view, 6> REASONS =
        { "movement", "combat", "healing", "tanking", "stuck", "other" };
    uint8 reasonId = 0;
    if (!reason.empty())
    {
        auto const found = std::find(REASONS.begin(), REASONS.end(), reason);
        if (found == REASONS.end())
        {
            message = "The reason is one of movement, combat, healing, tanking, stuck or other.";
            return false;
        }
        reasonId = uint8(found - REASONS.begin() + 1);
    }

    if (!sCaptureWriter->On(F::Stream::Companion))
    {
        message = "Thanks, but this realm is not recording companion feedback right now.";
        return true;
    }
    sCaptureWriter->Write(F::Stream::Companion, CaptureWriter::MAP_ALL,
        F::CompanionRating{ CaptureWriter::NowMs(), IdOf(owner), IdOf(companion), rating, reasonId });
    message = rating > 0 ? "Thanks: your companion's play was rated good." : "Thanks: your companion's play was rated "
        "bad; it helps train the next ones.";
    return true;
}

void AddSC_animus_capture()
{
    new CaptureWorldScript();
    new CapturePlayerScript();
    new CaptureMovementScript();
    new CaptureServerScript();
    new CaptureSpellScript();
    new CaptureUnitScript();
    new CaptureGlobalScript();
}

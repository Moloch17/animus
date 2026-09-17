/*
 * This file is part of the Animus Forge project, based on AzerothCore.
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

#include "TravelBlock.h"
#include "EncoderSupport.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "Player.h"
#include "SeatView.h"
#include "Spell.h"
#include "SpellChecks.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    constexpr uint32 TRAVEL_MOVE_POINT_ID = 6;
    constexpr float BASE_RUN_SPEED = 7.0f;
    constexpr float MAX_GROUND_SEARCH = 200.0f;
    constexpr float AIRBORNE_ABOVE = 2.0f;      // higher than this without flight is falling

    struct MountSpell
    {
        uint8 Level;        // a player of this level has it
        uint32 Alliance;
        uint32 Horde;
        bool Flying;
    };

    /// Riding as players learn it in 3.3.5: skill spells by level, then the side's mounts of each speed.
    constexpr std::array<std::pair<uint8, uint32>, 5> RIDING_SKILLS = { {
        { 20, 33388 },      // Apprentice Riding: 60% ground mounts
        { 40, 33391 },      // Journeyman Riding: 100%
        { 60, 34090 },      // Expert Riding: 150% flying mounts
        { 68, 54197 },      // Cold Weather Flying: flying in Northrend (trainable at 68 since patch 3.2)
        { 70, 34091 },      // Artisan Riding: 280% flying
    } };

    constexpr std::array<MountSpell, 4> MOUNTS = { {
        { 20, 458, 580, false },            // Brown Horse, Timber Wolf
        { 40, 23229, 23250, false },        // Swift Brown Steed, Swift Brown Wolf
        { 60, 32235, 32243, true },         // Golden Gryphon, Tawny Wind Rider
        { 70, 32242, 32246, true },         // Swift Blue Gryphon, Swift Red Wind Rider
    } };

    SpellInfo const* FastestMount(Player const* bot, bool flying)
    {
        SpellInfo const* best = nullptr;
        for (MountSpell const& mount : MOUNTS)
        {
            uint32 const spellId = bot->GetTeamId() == TEAM_HORDE ? mount.Horde : mount.Alliance;
            if (mount.Flying == flying && bot->HasSpell(spellId))
                best = sSpellMgr->GetSpellInfo(spellId);
        }

        return best;
    }

    bool CanSummon(Player* bot, SpellInfo const* mount)
    {
        if (!mount || bot->IsMounted() || !bot->IsAlive() || Encoding::CastInProgress(bot)
            || bot->GetGlobalCooldownMgr().HasGlobalCooldown(mount) || !bot->movespline->Finalized())
            return false;

        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        return Animus::SpellChecks::CheckCast(bot, mount, targets, nullptr);
    }

    /// Fly along a straight spline (flight needs no path and no ground).
    void FlyTo(Player* bot, float x, float y, float z)
    {
        bot->GetMotionMaster()->Clear();
        Movement::MoveSplineInit init(bot);
        init.MoveTo(x, y, z, false, true);
        init.SetFly();
        init.Launch();
    }

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        Player* bot = view.Bot;
        if (!bot->IsAlive())
            return false;

        bool const busy = Encoding::CastInProgress(bot) || bot->HasUnitState(Encoding::IMMOBILE_STATES);
        switch (action)
        {
            case TravelBlock::ACTION_MOUNT_GROUND:
                return CanSummon(bot, TravelBlock::GroundMount(bot));
            case TravelBlock::ACTION_MOUNT_FLYING:
                return CanSummon(bot, TravelBlock::FlyingMount(bot));
            case TravelBlock::ACTION_DISMOUNT:
                return bot->IsMounted();
            case TravelBlock::ACTION_MOVE_TO_OBJECTIVE:
                return view.HasObjective && !busy && !TravelBlock::AtObjective(bot, view.Objective);
            case TravelBlock::ACTION_ASCEND:
                return bot->CanFly() && !busy && TravelBlock::HeightAboveGround(bot) < TravelBlock::MAX_ALTITUDE;
            case TravelBlock::ACTION_DESCEND:
                return bot->CanFly() && !busy && TravelBlock::HeightAboveGround(bot) > 0.5f;
            default:
                return false;
        }
    }
}

SpellInfo const* Animus::Curriculum::TravelBlock::GroundMount(Player const* bot)
{
    return FastestMount(bot, false);
}

SpellInfo const* Animus::Curriculum::TravelBlock::FlyingMount(Player const* bot)
{
    return FastestMount(bot, true);
}

float Animus::Curriculum::TravelBlock::HeightAboveGround(Player const* bot)
{
    float const ground = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), true,
        MAX_GROUND_SEARCH);
    return ground > INVALID_HEIGHT ? std::max(0.0f, bot->GetPositionZ() - ground) : 0.0f;
}

bool Animus::Curriculum::TravelBlock::AtObjective(Player const* bot, Position const& objective)
{
    return bot->GetExactDist2d(&objective) <= ARRIVE_DISTANCE && HeightAboveGround(bot) <= AIRBORNE_ABOVE;
}

void Animus::Curriculum::TravelBlock::FallIfAirborne(Player* bot)
{
    if (!bot->IsAlive() || bot->CanFly() || !bot->movespline->Finalized() || HeightAboveGround(bot) <= AIRBORNE_ABOVE)
        return;

    float const ground = bot->GetMapHeight(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), true,
        MAX_GROUND_SEARCH);

    // MoveFall remembers where the fall started (SetFallInformation); landing is Player::HandleFall, as for a client.
    bot->GetMotionMaster()->Clear();
    bot->GetMotionMaster()->MoveFall();

    MovementInfo landing = bot->m_movementInfo;
    landing.pos.Relocate(bot->GetPositionX(), bot->GetPositionY(), ground);
    bot->HandleFall(landing);
}

void Animus::Curriculum::TravelBlock::LearnRiding(Player* bot)
{
    uint8 const level = bot->GetLevel();
    for (auto const& [requiredLevel, spellId] : RIDING_SKILLS)
        if (level >= requiredLevel && !bot->HasSpell(spellId))
            bot->learnSpell(spellId);

    for (MountSpell const& mount : MOUNTS)
    {
        uint32 const spellId = bot->GetTeamId() == TEAM_HORDE ? mount.Horde : mount.Alliance;
        if (level >= mount.Level && !bot->HasSpell(spellId))
            bot->learnSpell(spellId);
    }
}

Animus::Curriculum::BlockSize Animus::Curriculum::TravelBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

void Animus::Curriculum::TravelBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;

    obs[OBS_MOUNTED] = bot->IsMounted() ? 1.0f : 0.0f;
    obs[OBS_FLYING_MOUNT] = bot->IsMounted() && bot->CanFly() ? 1.0f : 0.0f;
    obs[OBS_RIDING_SKILL] = std::min(1.0f, float(bot->GetSkillValue(SKILL_RIDING)) / 300.0f);
    obs[OBS_INDOORS] = bot->IsOutdoors() ? 0.0f : 1.0f;
    obs[OBS_HEIGHT] = std::min(1.0f, HeightAboveGround(bot) / 50.0f);
    obs[OBS_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
    obs[OBS_MOVING] = bot->movespline->Finalized() ? 0.0f : 1.0f;

    UnitMoveType const moveType = bot->CanFly() ? MOVE_FLIGHT : MOVE_RUN;
    obs[OBS_SPEED] = std::min(1.0f, bot->GetSpeed(moveType) / BASE_RUN_SPEED / 4.0f);

    if (view.HasObjective)
    {
        float const bearing = bot->GetRelativeAngle(&view.Objective);
        obs[OBS_OBJECTIVE] = 1.0f;
        obs[OBS_OBJECTIVE_DISTANCE] = std::min(1.0f, bot->GetExactDist2d(&view.Objective) / 500.0f);
        obs[OBS_OBJECTIVE_BEARING_SIN] = std::sin(bearing);
        obs[OBS_OBJECTIVE_BEARING_COS] = std::cos(bearing);
        obs[OBS_OBJECTIVE_HEIGHT] = std::clamp((view.Objective.GetPositionZ() - bot->GetPositionZ()) / 50.0f, -1.0f,
            1.0f);
        obs[OBS_AT_OBJECTIVE] = AtObjective(bot, view.Objective) ? 1.0f : 0.0f;
    }

    // Asked here rather than of the mask: the policy sees whether mounting is possible even when no mask is wanted.
    obs[OBS_CAN_MOUNT] = IsAllowed(view, ACTION_MOUNT_GROUND) ? 1.0f : 0.0f;
    obs[OBS_CAN_FLY] = IsAllowed(view, ACTION_MOUNT_FLYING) ? 1.0f : 0.0f;

    for (uint32 action = 0; mask && action < ACTION_COUNT; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::TravelBlock::BeforeApply(SeatView& view, SeatActionResult& /*result*/) const
{
    // A cast, a dismount or a lost flying mount leaves no one hanging in the air.
    FallIfAirborne(view.Bot);
}

void Animus::Curriculum::TravelBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!IsAllowed(view, local))
        return;

    Player* bot = view.Bot;
    float const x = bot->GetPositionX();
    float const y = bot->GetPositionY();
    float const z = bot->GetPositionZ();

    switch (local)
    {
        case ACTION_MOUNT_GROUND:
        case ACTION_MOUNT_FLYING:
        {
            SpellInfo const* mount = local == ACTION_MOUNT_GROUND ? GroundMount(bot) : FlyingMount(bot);
            SpellCastTargets targets;
            targets.SetUnitTarget(bot);
            Spell* spell = new Spell(bot, mount, TRIGGERED_NONE);
            if (spell->prepare(&targets) == SPELL_CAST_OK)
                ++result.SpellCasts;
            return;
        }
        case ACTION_DISMOUNT:
            // As CMSG_CANCEL_MOUNT_AURA.
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
            FallIfAirborne(bot);
            return;
        case ACTION_MOVE_TO_OBJECTIVE:
        {
            Position const& objective = view.Objective;
            if (bot->CanFly())
            {
                // Straight at it, never lower than now: climbing over what is in the way is the seat's call.
                float const ground = bot->GetMapHeight(objective.GetPositionX(), objective.GetPositionY(),
                    objective.GetPositionZ(), true, MAX_GROUND_SEARCH);
                float const landing = (ground > INVALID_HEIGHT ? ground : objective.GetPositionZ()) + 1.0f;
                FlyTo(bot, objective.GetPositionX(), objective.GetPositionY(),
                    std::max(landing, HeightAboveGround(bot) > AIRBORNE_ABOVE ? z : landing));
            }
            else
                Encoding::MoveTo(bot, TRAVEL_MOVE_POINT_ID, objective.GetPositionX(), objective.GetPositionY(),
                    objective.GetPositionZ());
            return;
        }
        case ACTION_ASCEND:
            FlyTo(bot, x, y, z + CLIMB_STEP);
            return;
        case ACTION_DESCEND:
        {
            float const ground = z - HeightAboveGround(bot);
            FlyTo(bot, x, y, std::max(ground + 0.5f, z - CLIMB_STEP));
            return;
        }
        default:
            return;
    }
}

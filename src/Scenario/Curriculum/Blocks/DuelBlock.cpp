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

#include "DuelBlock.h"
#include "DBCStores.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include "Map.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Supplies.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
    using namespace Animus::Curriculum;

    enum DuelSpells : uint32
    {
        SPELL_CALL_PET          = 883,      // its GCD is applied to calling a stabled beast
        SPELL_RECENTLY_BANDAGED = 11196,
    };

    constexpr uint32 DUEL_MOVE_POINT_ID = 1;

    /// A seat this far along DuelBlock::MOVE_TO_RANGE_DISTANCE is at its casting range already: running there again
    /// would not move it.
    constexpr float AT_RANGE_FRACTION = 0.6f;

    uint32 StableSlots(Layout const& layout)
    {
        return layout.Profile->Class == CLASS_HUNTER ? STABLE_SLOTS : 0;
    }

    bool IsAllowed(SeatView const& view, uint32 action)
    {
        Player* bot = view.Bot;
        Unit* target = view.Target;

        // Dead: only its own resurrection (Soulstone, Reincarnation), as the release dialog offers it.
        if (action == DuelBlock::ACTION_SELF_RESURRECT)
            return view.SelfResurrectAllowed && !bot->IsAlive() && bot->GetUInt32Value(PLAYER_SELF_RES_SPELL)
                && !bot->HasPreventResurectionAura();

        if (!bot->IsAlive())
            return false;

        bool const casting = bot->IsNonMeleeSpellCast(false, false, true);

        // No target needed (between gauntlet pulls too).
        switch (action)
        {
            case DuelBlock::ACTION_STOP_CASTING:
                return casting;
            case DuelBlock::ACTION_CANCEL_FORM:
                return Encoding::CancellableForm(bot) != nullptr;
            case DuelBlock::ACTION_HEALTH_POTION:
                return Encoding::CanUseItemOn(bot, view.Supplies.HealthPotion, bot);
            case DuelBlock::ACTION_MANA_POTION:
                return Encoding::CanUseItemOn(bot, view.Supplies.ManaPotion, bot);
            case DuelBlock::ACTION_HEALTHSTONE:
                return Encoding::CanUseItemOn(bot, view.Supplies.Healthstone, bot);
            case DuelBlock::ACTION_BANDAGE:
                return Encoding::CanUseItemOn(bot, view.Supplies.Bandage, bot);
            case DuelBlock::ACTION_SOULSTONE_SELF:
            {
                uint32 const soulstone = view.Supplies.Soulstone;
                SpellInfo const* info = soulstone ? Encoding::UseSpell(soulstone) : nullptr;
                return info && !bot->HasAura(info->Id) && Encoding::CanUseItemOn(bot, soulstone, bot);
            }
            default:
                break;
        }

        bool const canMove = !casting && !bot->HasUnitState(Encoding::IMMOBILE_STATES);

        // A target it cannot see: all it can do about it is go look where it was last seen.
        if (!target && view.HiddenTarget && view.TargetSeen)
        {
            if (action == DuelBlock::ACTION_MOVE_TO_TARGET)
                return canMove;
            return action == DuelBlock::ACTION_STOP && !bot->movespline->Finalized();
        }

        // Calling a stable beast needs no target: a hunter calls its pet before a fight, or between pulls. Over a dead
        // pet too (CallHunterBeast dismisses the corpse).
        if (action >= DuelBlock::ACTION_CALL_BEAST_FIRST)
        {
            uint32 const slot = action - DuelBlock::ACTION_CALL_BEAST_FIRST;
            if (slot >= view.StableCount || casting || !CanCallHunterBeast(bot))
                return false;

            SpellInfo const* callPet = sSpellMgr->GetSpellInfo(SPELL_CALL_PET);
            return !callPet || !bot->GetGlobalCooldownMgr().HasGlobalCooldown(callPet);
        }

        if (!target || !target->IsAlive())
            return false;

        // Standing still where the order would take it: the order is done, and offering it again is what a
        // jittering policy presses (stage1_duel 2026-09-17: the rogue pressed a movement order every 0.39 s while
        // it stood in melee reach 96% of the time). Moving, it is offered: the target may have moved on.
        bool const standing = bot->movespline->Finalized();
        bool const inMelee = target->IsWithinMeleeRange(bot);
        switch (action)
        {
            case DuelBlock::ACTION_MOVE_TO_TARGET:
                return canMove && !(standing && inMelee);
            case DuelBlock::ACTION_MOVE_BEHIND:
                // Behind is the target's back arc, which is where the order would have put it.
                return canMove && !(standing && inMelee && !target->HasInArc(float(M_PI), bot));
            case DuelBlock::ACTION_MOVE_TO_RANGE:
                return canMove && !(standing && !inMelee
                    && bot->GetDistance(target) >= AT_RANGE_FRACTION * DuelBlock::MOVE_TO_RANGE_DISTANCE
                    && bot->GetDistance(target) <= DuelBlock::MOVE_TO_RANGE_DISTANCE);
            case DuelBlock::ACTION_BACK_OFF:
                return canMove;
            case DuelBlock::ACTION_STOP:
                return !bot->movespline->Finalized();
            case DuelBlock::ACTION_BREAK_LINE_OF_SIGHT:
                return canMove && bot->IsWithinLOSInMap(target);
            case DuelBlock::ACTION_START_ATTACK:
                return bot->GetVictim() != target && bot->IsValidAttackTarget(target);
            case DuelBlock::ACTION_KEEP_RANGE:
                // A ranged spec only, and not while it is already keeping range.
                return canMove && view.Option && !view.Option->Running(SeatOptionKind::KeepRange, view.NowMs)
                    && view.L->Profile->Specs[view.Spec].Range != RangeBand::Melee;
            case DuelBlock::ACTION_STAY_ON_TARGET:
                // The melee mirror of keeping range.
                return canMove && view.Option && !view.Option->Running(SeatOptionKind::StayOnTarget, view.NowMs)
                    && view.L->Profile->Specs[view.Spec].Range == RangeBand::Melee;
            case DuelBlock::ACTION_PET_ATTACK:
                return std::any_of(bot->m_Controlled.begin(), bot->m_Controlled.end(), [target](Unit* pet)
                {
                    return pet->IsAlive() && pet->IsCreature() && pet->GetVictim() != target;
                });
            default:
                break;
        }

        return false;
    }
}

bool Animus::Curriculum::DuelBlock::FindCover(Player* bot, Unit* target, Position& cover)
{
    Map const* map = bot->FindMap();
    if (!map || !target)
        return false;

    // Eye height: what the target sees over, and what the bot hides behind.
    constexpr float EYE = 2.0f;
    constexpr float MAX_STEP = 6.0f;

    // Nearest ring first; on a ring, the bearings pointing away from the target first.
    float const away = target->GetAngle(bot);
    for (float distance : COVER_DISTANCES)
    {
        for (uint32 step = 0; step < COVER_BEARINGS; ++step)
        {
            // 0, +1, -1, +2, -2, ... bearings from straight away.
            int32 const offset = int32((step + 1) / 2) * (step % 2 ? 1 : -1);
            float const angle = away + float(offset) * 2.0f * float(M_PI) / float(COVER_BEARINGS);
            float const x = bot->GetPositionX() + distance * std::cos(angle);
            float const y = bot->GetPositionY() + distance * std::sin(angle);
            float const z = map->GetHeight(bot->GetPhaseMask(), x, y, bot->GetPositionZ() + MAX_STEP, true,
                MAX_STEP * 2.0f);
            if (z <= INVALID_HEIGHT || std::fabs(z - bot->GetPositionZ()) > MAX_STEP)
                continue;

            if (map->isInLineOfSight(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ() + EYE,
                x, y, z + EYE, bot->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                continue;

            cover.Relocate(x, y, z);
            return true;
        }
    }

    return false;
}

Animus::Curriculum::BlockSize Animus::Curriculum::DuelBlock::Size(Layout const& layout) const
{
    uint32 const stable = StableSlots(layout);
    return { OBS_COUNT_WITHOUT_STABLE + stable * STABLE_FEATURES, ACTION_COUNT_WITHOUT_STABLE + stable };
}

std::string Animus::Curriculum::DuelBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    static constexpr std::array<char const*, ACTION_COUNT_WITHOUT_STABLE> NAMES =
    {
        "move_to_target", "move_behind", "move_to_range", "back_off", "stop", "start_attack", "pet_attack",
        "stop_casting", "cancel_form", "health_potion", "mana_potion", "healthstone", "bandage", "soulstone_self",
        "self_resurrect", "break_line_of_sight", "keep_range", "stay_on_target"
    };
    static_assert(NAMES.size() == ACTION_COUNT_WITHOUT_STABLE, "every duel action needs a name");

    if (local < NAMES.size())
        return NAMES[local];
    return Acore::StringFormat("call_beast_{}", local - ACTION_CALL_BEAST_FIRST);
}

void Animus::Curriculum::DuelBlock::DescribeManifest(Layout const& layout, boost::json::object& block) const
{
    block["stable_slots"] = StableSlots(layout);
    block["stable_features"] = STABLE_FEATURES;
}

void Animus::Curriculum::DuelBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    Player* bot = view.Bot;
    Unit* target = view.Target;

    // The bot's own casting and form, with or without a target.
    if (Spell* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        cast && cast->getState() == SPELL_STATE_PREPARING && cast->GetCastTime() > 0)
    {
        float const total = float(cast->GetCastTime());
        float const left = std::clamp(float(cast->GetCastTimeRemaining()), 0.0f, total);
        obs[OBS_CAST_PROGRESS] = 1.0f - left / total;
        obs[OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
    }
    else if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        channel && channel->getState() == SPELL_STATE_CASTING)
    {
        float const left = float(std::max(0, channel->GetCastTimeRemaining()));
        float const total = std::max(left, float(std::max(1, channel->m_spellInfo->GetMaxDuration())));
        obs[OBS_CAST_PROGRESS] = 1.0f - left / total;
        obs[OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
    }

    obs[OBS_SHAPESHIFTED] = Encoding::CancellableForm(bot) ? 1.0f : 0.0f;
    obs[OBS_COMBAT_TIME] = view.CombatTime;

    obs[OBS_BOT_STUNNED] = bot->HasUnitState(UNIT_STATE_STUNNED) ? 1.0f : 0.0f;
    obs[OBS_BOT_FEARED] = bot->HasAuraType(SPELL_AURA_MOD_FEAR) || bot->HasAuraType(SPELL_AURA_MOD_CONFUSE)
        ? 1.0f : 0.0f;
    obs[OBS_BOT_ROOTED] = bot->HasAuraType(SPELL_AURA_MOD_ROOT) ? 1.0f : 0.0f;
    obs[OBS_BOT_SILENCED] = bot->HasAuraType(SPELL_AURA_MOD_SILENCE) || bot->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE)
        ? 1.0f : 0.0f;
    obs[OBS_BOT_SNARED] = bot->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ? 1.0f : 0.0f;

    // What it carries, with or without a target.
    BattleSupplies const& supplies = view.Supplies;
    auto const carried = [bot](uint32 entry, float full)
    {
        return entry ? std::min(1.0f, float(bot->GetItemCount(entry)) / full) : 0.0f;
    };
    float const stack = float(CONSUMABLE_COUNT);
    obs[OBS_HEALTH_POTIONS] = carried(supplies.HealthPotion, stack);
    obs[OBS_MANA_POTIONS] = carried(supplies.ManaPotion, stack);
    obs[OBS_HEALTHSTONES] = carried(supplies.Healthstone, 1.0f);
    obs[OBS_BANDAGES] = carried(supplies.Bandage, stack);
    obs[OBS_POTION_COOLDOWN] = std::max(Encoding::ItemCooldownFraction(bot, supplies.HealthPotion),
        Encoding::ItemCooldownFraction(bot, supplies.ManaPotion));
    obs[OBS_HEALTHSTONE_COOLDOWN] = Encoding::ItemCooldownFraction(bot, supplies.Healthstone);
    obs[OBS_RECENTLY_BANDAGED] = bot->HasAura(SPELL_RECENTLY_BANDAGED) ? 1.0f : 0.0f;
    obs[OBS_SOULSTONE_ON_BOT] = view.SelfResurrectAllowed && bot->GetResurrectionSpellId() ? 1.0f : 0.0f;

    // Hunters: what each stable slot offers, so the policy can find the pet it prefers.
    for (uint32 slot = 0; slot < view.StableCount && slot < STABLE_SLOTS; ++slot)
    {
        CreatureTemplate const* beast = sObjectMgr->GetCreatureTemplate(view.Stable[slot]);
        if (!beast)
            continue;

        float* features = obs + OBS_STABLE_FIRST + slot * STABLE_FEATURES;
        features[0] = 1.0f;
        features[1] = float(beast->family) / 50.0f;

        if (CreatureFamilyEntry const* family = sCreatureFamilyStore.LookupEntry(beast->family))
            if (family->petTalentType >= 0 && family->petTalentType < 3)
                features[2 + family->petTalentType] = 1.0f;     // ferocity, tenacity, cunning
    }

    if (!target && view.HiddenTarget)
    {
        obs[OBS_TARGET_HIDDEN] = 1.0f;
        if (view.TargetSeen)
        {
            float const bearing = bot->GetRelativeAngle(&view.LastSeen);
            obs[OBS_TARGET_UNSEEN_TIME] = view.TargetUnseenTime;
            obs[OBS_LAST_SEEN_DISTANCE] = std::min(1.0f, bot->GetExactDist(&view.LastSeen) / 60.0f);
            obs[OBS_LAST_SEEN_BEARING_SIN] = std::sin(bearing);
            obs[OBS_LAST_SEEN_BEARING_COS] = std::cos(bearing);
        }

        // Its own state it still knows.
        obs[OBS_BOT_MOVING] = bot->movespline->Finalized() ? 0.0f : 1.0f;
        obs[OBS_BOT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
        obs[OBS_BOT_STEALTHED] = bot->HasStealthAura() ? 1.0f : 0.0f;
        obs[OBS_DAMAGE_TAKEN] = view.LastStepDamageTaken;
        if (Unit* pet = Encoding::FirstPet(bot))
        {
            obs[OBS_PET_OUT] = 1.0f;
            obs[OBS_PET_HEALTH] = pet->GetHealthPct() / 100.0f;
        }
    }

    if (target)
    {
        float const bearing = bot->GetRelativeAngle(target);
        obs[OBS_DISTANCE] = std::min(1.0f, bot->GetDistance(target) / 60.0f);
        obs[OBS_BEARING_SIN] = std::sin(bearing);
        obs[OBS_BEARING_COS] = std::cos(bearing);
        obs[OBS_BEHIND_TARGET] = target->isInBack(bot) ? 1.0f : 0.0f;
        obs[OBS_TARGET_FACING_BOT] = target->HasInArc(float(M_PI), bot) ? 1.0f : 0.0f;
        obs[OBS_TARGET_IN_COMBAT] = target->IsInCombat() ? 1.0f : 0.0f;
        obs[OBS_TARGET_ATTACKS_BOT] = target->GetVictim() == bot ? 1.0f : 0.0f;
        obs[OBS_TARGET_CASTING] = target->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        obs[OBS_TARGET_IN_LINE_OF_SIGHT] = bot->IsWithinLOSInMap(target) ? 1.0f : 0.0f;
        obs[OBS_BOT_MOVING] = bot->movespline->Finalized() ? 0.0f : 1.0f;
        obs[OBS_BOT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
        obs[OBS_BOT_STEALTHED] = bot->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;
        obs[OBS_BOT_AUTO_ATTACKING] = bot->GetVictim() == target && bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING)
            ? 1.0f : 0.0f;
        obs[OBS_DAMAGE_TAKEN] = view.LastStepDamageTaken;

        if (Unit* pet = Encoding::FirstPet(bot))
        {
            obs[OBS_PET_OUT] = 1.0f;
            obs[OBS_PET_HEALTH] = pet->GetHealthPct() / 100.0f;
            obs[OBS_PET_ATTACKING] = pet->GetVictim() == target ? 1.0f : 0.0f;
        }

        Encoding::WriteOpponentType(target, obs + OBS_TARGET_TYPE_FIRST);
        obs[OBS_TARGET_MAX_HEALTH] = std::min(1.0f,
            float(target->GetMaxHealth()) / float(std::max<uint32>(1, bot->GetMaxHealth())) / 4.0f);
        obs[OBS_TARGET_DAMAGE_MODIFIER] = Encoding::DamageModifier(target) / 2.0f;
        obs[OBS_TARGET_ARMOR] = Encoding::ArmorReduction(target, bot->GetLevel());
        obs[OBS_TARGET_RUN_SPEED] = target->GetSpeedRate(MOVE_RUN) / 2.0f;
        obs[OBS_TARGET_LEVEL_DIFFERENCE] = std::clamp((float(target->GetLevel()) - float(bot->GetLevel())) / 5.0f,
            -1.0f, 1.0f);
        for (uint32 i = 0; i < Encoding::OBSERVED_SCHOOLS.size(); ++i)
            obs[OBS_TARGET_IMMUNE_SCHOOL_FIRST + i] = Encoding::IsImmuneToSchool(target, Encoding::OBSERVED_SCHOOLS[i])
                ? 1.0f : 0.0f;
        for (uint32 i = 0; i < Encoding::OBSERVED_MECHANICS.size(); ++i)
            obs[OBS_TARGET_IMMUNE_MECHANIC_FIRST + i] =
                Encoding::IsImmuneToMechanic(target, Encoding::OBSERVED_MECHANICS[i]) ? 1.0f : 0.0f;
    }

    uint32 const actions = view.L->Slice(BlockId::Duel).ActionCount;
    for (uint32 action = 0; mask && action < actions; ++action)
        mask[action] = IsAllowed(view, action) ? 1 : 0;
}

void Animus::Curriculum::DuelBlock::ObserveDead(SeatView const& view, float* obs, uint8* mask)
{
    obs[OBS_DEAD] = 1.0f;
    bool const selfResurrect = IsAllowed(view, ACTION_SELF_RESURRECT);
    obs[OBS_SELF_RESURRECT] = selfResurrect ? 1.0f : 0.0f;
    if (mask)
        mask[ACTION_SELF_RESURRECT] = selfResurrect ? 1 : 0;
}

void Animus::Curriculum::DuelBlock::BeforeApply(SeatView& view, SeatActionResult& result) const
{
    // Face the target whenever not running somewhere: casts and swings need it, and turning is not a decision worth
    // learning.
    Player* bot = view.Bot;
    Unit* target = view.Target;
    if (target && bot->IsAlive() && bot->movespline->Finalized() && !bot->HasInArc(float(M_PI) / 2, target))
        bot->SetFacingToObject(target);

    if (!view.Option || !IsPositioning(view.Option->Kind) || view.NowMs >= view.Option->UntilMs)
        return;

    // A positioning option is over once there is nothing to position against.
    if (!target || !target->IsAlive() || !bot->IsAlive())
    {
        *view.Option = SeatOption();
        return;
    }

    if (!bot->movespline->Finalized())
        return;     // already running somewhere: let the step finish

    // Keeping range: the target in melee reach is what it runs back out from. Staying on the target: out of melee
    // reach is what it closes again.
    if (view.Option->Kind == SeatOptionKind::KeepRange && target->IsWithinMeleeRange(bot))
        Apply(view, ACTION_MOVE_TO_RANGE, result);
    else if (view.Option->Kind == SeatOptionKind::StayOnTarget && !target->IsWithinMeleeRange(bot))
        Apply(view, ACTION_MOVE_TO_TARGET, result);
}

void Animus::Curriculum::DuelBlock::Apply(SeatView& view, uint32 local, SeatActionResult& result) const
{
    if (!IsAllowed(view, local))
        return;

    Player* bot = view.Bot;
    Unit* target = view.Target;

    auto const useItem = [bot, &result](uint32 entry)
    {
        if (Encoding::UseItemOn(bot, entry, bot))
            ++result.ConsumablesUsed;
    };

    switch (local)
    {
        case ACTION_SELF_RESURRECT:
            // As CMSG_SELF_RES.
            bot->CastSpell(bot, bot->GetUInt32Value(PLAYER_SELF_RES_SPELL));
            bot->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);
            result.SelfResurrected = bot->IsAlive();
            return;
        case ACTION_HEALTH_POTION:
            useItem(view.Supplies.HealthPotion);
            return;
        case ACTION_MANA_POTION:
            useItem(view.Supplies.ManaPotion);
            return;
        case ACTION_HEALTHSTONE:
            useItem(view.Supplies.Healthstone);
            return;
        case ACTION_BANDAGE:
            useItem(view.Supplies.Bandage);
            return;
        case ACTION_SOULSTONE_SELF:
            useItem(view.Supplies.Soulstone);
            return;
        default:
            break;
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    // Keeping range is a durative action: it starts here and runs in BeforeApply until the policy does something else.
    if (local == ACTION_KEEP_RANGE)
    {
        view.Option->Kind = SeatOptionKind::KeepRange;
        view.Option->UntilMs = view.NowMs + view.Options.KeepRangeMs;
        if (target && target->IsWithinMeleeRange(bot))
            Apply(view, ACTION_MOVE_TO_RANGE, result);
        return;
    }

    if (local == ACTION_STAY_ON_TARGET)
    {
        view.Option->Kind = SeatOptionKind::StayOnTarget;
        view.Option->UntilMs = view.NowMs + view.Options.StayOnTargetMs;
        if (target && !target->IsWithinMeleeRange(bot))
            Apply(view, ACTION_MOVE_TO_TARGET, result);
        return;
    }

    if (!target)
    {
        // Allowed without a target only to search for a hidden one, or to stop.
        if (local == ACTION_MOVE_TO_TARGET)
            Encoding::MoveTo(bot, DUEL_MOVE_POINT_ID, view.LastSeen.GetPositionX(), view.LastSeen.GetPositionY(),
                view.LastSeen.GetPositionZ());
        else if (local == ACTION_STOP)
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }
        return;
    }

    switch (local)
    {
        case ACTION_MOVE_TO_TARGET:
            target->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), 0.5f, target->GetAngle(bot));
            break;
        case ACTION_MOVE_BEHIND:
            target->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), 0.5f,
                Position::NormalizeOrientation(target->GetOrientation() + float(M_PI)));
            break;
        case ACTION_MOVE_TO_RANGE:
            target->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), MOVE_TO_RANGE_DISTANCE, target->GetAngle(bot));
            break;
        case ACTION_BACK_OFF:
            target->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), bot->GetDistance(target) + BACK_OFF_DISTANCE,
                target->GetAngle(bot));
            break;
        case ACTION_STOP:
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            return;
        case ACTION_BREAK_LINE_OF_SIGHT:
        {
            Position cover;
            if (!FindCover(bot, target, cover))
                return;
            x = cover.GetPositionX();
            y = cover.GetPositionY();
            z = cover.GetPositionZ();
            break;
        }
        case ACTION_START_ATTACK:
            bot->Attack(target, true);
            return;
        case ACTION_PET_ATTACK:
            if (Encoding::PetAttack(bot, target))
            {
                ++result.PetOrders;
                result.PetOrderGiven = PetOrder::Attack;
            }
            return;
        case ACTION_STOP_CASTING:
            // As CMSG_CANCEL_CAST / CMSG_CANCEL_CHANNELLING: the current cast or channel, cancelled by the caster.
            bot->InterruptNonMeleeSpells(false, 0, false, true);
            return;
        case ACTION_CANCEL_FORM:
            // As CMSG_CANCEL_AURA.
            if (SpellInfo const* form = Encoding::CancellableForm(bot))
                bot->RemoveOwnedAura(form->Id, ObjectGuid::Empty, 0, AURA_REMOVE_BY_CANCEL);
            return;
        default:
            result.CallBeast = view.Stable[local - ACTION_CALL_BEAST_FIRST];
            return;
    }

    Encoding::MoveTo(bot, DUEL_MOVE_POINT_ID, x, y, z);
}

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

#include "EncoderSupport.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GearStats.h"
#include "SpellChecks.h"
#include "Item.h"
#include "Layout.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>

namespace
{
    enum EncoderSpells : uint32
    {
        SPELL_CALL_PET          = 883,      // its GCD is applied to calling a stabled beast
    };

    constexpr uint32 CALL_BEAST_GCD_MS = 1500;
    constexpr float POSITION_SCALE = 40.0f;
}

namespace Animus::Curriculum::Encoding
{
    using SpellChecks::CheckCast;
    using SpellChecks::CooldownFraction;

    float RelativePosition(float coordinate, float origin)
    {
        return std::clamp((coordinate - origin) / POSITION_SCALE, -2.0f, 2.0f);
    }

    SpellCastTargets TargetsFor(SpellInfo const* info, Player* bot, Unit* target)
    {
        SpellCastTargets targets;

        // No target (between gauntlet pulls): only self-cast spells can succeed.
        if (target && (info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION))
            targets.SetDst(*target);

        if (info->NeedsExplicitUnitTarget() && target && !info->IsPositive())
            targets.SetUnitTarget(target);
        else
            targets.SetUnitTarget(bot);

        return targets;
    }

    void WriteKnownCooldowns(Player const* bot, std::vector<ActionCatalog::Action> const& actions, float* out)
    {
        for (std::size_t i = 0; i < actions.size(); ++i)
        {
            if (SpellInfo const* info = ActionCatalog::KnownRank(bot, actions[i].FirstRank))
            {
                out[i * 2] = 1.0f;
                out[i * 2 + 1] = CooldownFraction(bot, info);
            }
        }
    }

    bool CastInProgress(Player const* bot)
    {
        return bot->IsNonMeleeSpellCast(false, true, true);
    }

    bool CanCast(Player* bot, SpellInfo const* info, Unit* target, Item* castItem)
    {
        return CheckCast(bot, info, TargetsFor(info, bot, target), castItem);
    }

    bool CanHeal(Player* bot, ActionCatalog::Action const& heal, Unit* ally)
    {
        SpellInfo const* info = ActionCatalog::KnownRank(bot, heal.FirstRank);
        if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id)
            || bot->GetGlobalCooldownMgr().HasGlobalCooldown(info) || CastInProgress(bot))
            return false;

        if (!bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
            return false;

        SpellCastTargets targets;
        targets.SetUnitTarget(ally);
        return CheckCast(bot, info, targets, nullptr);
    }

    void Heal(Player* bot, ActionCatalog::Action const& heal, Unit* ally, SeatActionResult& result)
    {
        SpellInfo const* info = ActionCatalog::KnownRank(bot, heal.FirstRank);
        if (!info)
            return;

        SpellCastTargets targets;
        targets.SetUnitTarget(ally);
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        if (spell->prepare(&targets) == SPELL_CAST_OK)
        {
            ++result.SpellCasts;
            ++result.SustainCasts;
        }
    }

    SpellInfo const* KnownRank(SeatView const& view, ActionCatalog::Action const& def)
    {
        if (view.KnownRanks && def.Index < view.KnownRanks->size())
            return (*view.KnownRanks)[def.Index];

        return ActionCatalog::KnownRank(view.Bot, def.FirstRank);
    }

    bool IsSpellActionAllowed(SeatView const& view, Unit* target, ActionCatalog::Action const& def)
    {
        Player* bot = view.Bot;

        // Cheap rejections before the full cast check.
        SpellInfo const* info = KnownRank(view, def);
        if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id) || CastInProgress(bot))
            return false;

        if (def.NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
            return false;

        if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
            return false;

        // With movement actions (duel block on): server-driven movement does not set the movement flags CheckCast
        // looks at, so no cast-time or channeled spell while running.
        if (view.L->Has(BlockId::Duel) && !bot->movespline->Finalized()
            && (info->CalcCastTime(bot) || info->IsChanneled()))
            return false;

        return CanCast(bot, info, target);
    }

    bool ApplySpellAction(SeatView const& view, Unit* target, ActionCatalog::Action const& def,
        SeatActionResult& result)
    {
        Player* bot = view.Bot;
        if (def.NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
            return false;

        SpellInfo const* info = KnownRank(view, def);
        if (!info || !bot->HasActiveSpell(info->Id) || CastInProgress(bot))
            return false;

        // Same path as CMSG_CAST_SPELL. prepare() runs the full cast validation again, so a masked action
        // from a misbehaving client simply fails. The spell owns and frees itself.
        SpellCastTargets targets = TargetsFor(info, bot, target);
        bool const stealthed = bot->HasStealthAura();
        bool const targetCasting = target && target->IsNonMeleeSpellCast(false);
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        if (spell->prepare(&targets) != SPELL_CAST_OK)
            return false;

        ++result.SpellCasts;

        // A harmful spell from stealth. One that breaks it commits to the fight (Ambush, Garrote, Cheap Shot, Pounce,
        // an Aimed Shot out of Shadowmeld) and cannot be repeated without earning stealth back; one that keeps it (Sap,
        // Distract, Premeditation) sets the fight up and is paid once per target per stealth, so it cannot be farmed.
        bool const atEnemy = !info->IsPositive() || info->HasEffect(SPELL_EFFECT_DISTRACT);
        if (view.L->Has(BlockId::Duel) && stealthed && target && target != bot && atEnemy)
        {
            if (info->HasAttribute(SPELL_ATTR1_ALLOW_WHILE_STEALTHED))
                result.StealthUtilityTarget = target->GetGUID();
            else
                result.StealthOpener = true;
        }

        // An interrupt attempt on a casting enemy; the scenario checks next decision whether the cast stopped.
        if (view.L->Has(BlockId::Pack) && targetCasting && target != bot && ActionCatalog::IsInterruptingSpell(info))
            result.PendingInterrupt = target->GetGUID();

        return true;
    }

    SpellInfo const* TrinketSpell(Item const* item)
    {
        return item ? GearStats::ItemUseSpell(item->GetTemplate()) : nullptr;
    }

    SpellInfo const* UseSpell(uint32 itemEntry)
    {
        return GearStats::ItemUseSpell(sObjectMgr->GetItemTemplate(itemEntry));
    }

    bool CanUseItemOn(Player* bot, uint32 entry, Unit* target)
    {
        SpellInfo const* info = entry ? UseSpell(entry) : nullptr;
        Item* item = info ? bot->GetItemByEntry(entry) : nullptr;
        if (!item || !target || !bot->IsAlive() || bot->HasSpellCooldown(info->Id) || CastInProgress(bot))
            return false;

        // Server-driven movement does not set the movement flags CheckCast looks at.
        if (!bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
            return false;

        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        return CheckCast(bot, info, targets, item);
    }

    bool UseItemOn(Player* bot, uint32 entry, Unit* target)
    {
        Item* item = entry ? bot->GetItemByEntry(entry) : nullptr;
        if (!item || !target)
            return false;

        uint32 const before = bot->GetItemCount(entry);
        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        bot->CastItemUseSpell(item, targets, 1, 0);
        return bot->GetItemCount(entry) < before;
    }

    float ItemCooldownFraction(Player const* bot, uint32 entry)
    {
        SpellInfo const* info = entry ? UseSpell(entry) : nullptr;
        return info ? CooldownFraction(bot, info) : 0.0f;
    }

    bool CanRevive(SeatView const& view, ActionCatalog::Action const& revive, Player* ally)
    {
        Player* bot = view.Bot;
        if (!ally || !bot->IsAlive() || !ally->IsInMap(bot))
            return false;

        if (revive.Type == ActionCatalog::Kind::Soulstone)
        {
            SpellInfo const* info = view.Supplies.Soulstone ? UseSpell(view.Supplies.Soulstone) : nullptr;
            return info && ally->IsAlive() && !ally->HasAura(info->Id)
                && CanUseItemOn(bot, view.Supplies.Soulstone, ally);
        }

        return !ally->IsAlive() && !ally->isResurrectRequested() && CanHeal(bot, revive, ally);
    }

    void Revive(SeatView const& view, ActionCatalog::Action const& revive, Player* ally, SeatActionResult& result)
    {
        if (!CanRevive(view, revive, ally))
            return;

        if (revive.Type == ActionCatalog::Kind::Soulstone)
        {
            if (UseItemOn(view.Bot, view.Supplies.Soulstone, ally))
                ++result.ConsumablesUsed;
            return;
        }

        SpellInfo const* info = ActionCatalog::KnownRank(view.Bot, revive.FirstRank);
        SpellCastTargets targets;
        targets.SetUnitTarget(ally);
        Spell* spell = new Spell(view.Bot, info, TRIGGERED_NONE);
        if (spell->prepare(&targets) == SPELL_CAST_OK)
        {
            ++result.SpellCasts;
            ++result.Revives;
        }
    }

    void WriteRevives(SeatView const& view, float* out)
    {
        Player* bot = view.Bot;
        std::vector<ActionCatalog::Action> const& revives = view.L->AllyRevives;
        for (uint32 i = 0; i < revives.size(); ++i)
        {
            if (revives[i].Type == ActionCatalog::Kind::Soulstone)
            {
                if (view.Supplies.Soulstone && bot->GetItemCount(view.Supplies.Soulstone))
                {
                    out[i * 2] = 1.0f;
                    out[i * 2 + 1] = ItemCooldownFraction(bot, view.Supplies.Soulstone);
                }
            }
            else if (SpellInfo const* info = ActionCatalog::KnownRank(bot, revives[i].FirstRank))
            {
                out[i * 2] = 1.0f;
                out[i * 2 + 1] = CooldownFraction(bot, info);
            }
        }
    }

    Unit* FirstPet(Player* bot)
    {
        if (Pet* pet = bot->GetPet())
            return pet;

        for (Unit* controlled : bot->m_Controlled)
            if (controlled->IsAlive() && !controlled->IsTotem())
                return controlled;

        return nullptr;
    }

    SpellInfo const* CancellableForm(Player const* bot)
    {
        // Stances and presences cannot be cancelled from the client.
        for (AuraEffect const* effect : bot->GetAuraEffectsByType(SPELL_AURA_MOD_SHAPESHIFT))
        {
            SpellInfo const* info = effect->GetSpellInfo();
            if (!info->HasAttribute(SPELL_ATTR0_NO_AURA_CANCEL) && info->IsPositive() && !info->IsPassive())
                return info;
        }

        return nullptr;
    }

    bool IsCrowdControlled(Unit const* unit)
    {
        return unit->HasUnitState(CROWD_CONTROL_STATES) || unit->HasAuraType(SPELL_AURA_MOD_SILENCE)
            || unit->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE) || unit->HasAuraType(SPELL_AURA_TRANSFORM);
    }

    void WriteOpponentType(Unit const* unit, float* out)
    {
        uint32 const type = unit->IsPlayer() ? CREATURE_TYPE_HUMANOID : unit->GetCreatureType();
        for (uint32 i = 0; i < OPPONENT_TYPES.size(); ++i)
            out[i] = OPPONENT_TYPES[i] == type ? 1.0f : 0.0f;
    }

    float DamageModifier(Unit const* unit)
    {
        Creature const* creature = unit->ToCreature();
        return creature && creature->GetCreatureTemplate() ? creature->GetCreatureTemplate()->DamageModifier : 1.0f;
    }

    float ArmorReduction(Unit const* unit, uint8 attackerLevel)
    {
        // Unit::CalcArmorReducedDamage, before armor penetration.
        float const value = 0.1f * float(unit->GetArmor()) / (8.5f * float(attackerLevel) + 40.0f);
        return std::clamp(value / (1.0f + value), 0.0f, 0.75f);
    }

    bool IsImmuneToSchool(Unit const* unit, SpellSchools school)
    {
        return unit->IsImmunedToDamageOrSchool(SpellSchoolMask(1 << school));
    }

    bool IsImmuneToMechanic(Unit const* unit, Mechanics mechanic)
    {
        auto const& immune = unit->m_spellImmune[IMMUNITY_MECHANIC];
        return immune.count(mechanic) > 0 || (mechanic == MECHANIC_FEAR && immune.count(MECHANIC_HORROR) > 0);
    }

    int32 SlotOf(SeatView const& view, Unit const* unit)
    {
        if (!unit)
            return -1;

        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            if (view.Enemies[slot] == unit)
                return int32(slot);

        return -1;
    }

    int32 SlotAttacking(SeatView const& view, Unit const* victim, uint32 except)
    {
        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            if (Unit* enemy = view.Enemies[slot]; enemy && enemy->IsAlive() && enemy->GetVictim() == victim
                && slot != except)
                return int32(slot);

        return -1;
    }

    void SelectEnemy(SeatView& view, uint32 slot)
    {
        Unit* enemy = view.Enemies[slot];
        view.TargetSlot = slot;
        view.Bot->SetSelection(enemy->GetGUID());

        if (view.Bot->GetVictim())
            view.Bot->Attack(enemy, view.Bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING));
    }

    void MoveTo(Player* bot, uint32 pointId, float x, float y, float z)
    {
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(pointId, x, y, z);
    }

    bool PetAttack(Player* bot, Unit* target)
    {
        bool ordered = false;
        for (Unit* controlled : bot->m_Controlled)
        {
            Creature* pet = controlled->ToCreature();
            if (!pet || !pet->IsAlive() || !pet->IsAIEnabled || pet->GetVictim() == target
                || !pet->CanCreatureAttack(target))
                continue;

            // HandlePetActionHelper, COMMAND_ATTACK.
            pet->ClearUnitState(UNIT_STATE_FOLLOW);
            pet->AttackStop();
            if (CharmInfo* charmInfo = pet->GetCharmInfo())
            {
                charmInfo->SetIsCommandAttack(true);
                charmInfo->SetIsAtStay(false);
                charmInfo->SetIsFollowing(false);
                charmInfo->SetIsCommandFollow(false);
                charmInfo->SetIsReturning(false);
            }

            pet->AI()->AttackStart(target);
            ordered = true;
        }

        return ordered;
    }

    void StartCallBeastCooldown(Player* bot)
    {
        if (SpellInfo const* callPet = sSpellMgr->GetSpellInfo(SPELL_CALL_PET))
            bot->GetGlobalCooldownMgr().AddGlobalCooldown(callPet, CALL_BEAST_GCD_MS);
    }
}

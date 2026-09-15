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

#include "SeatEncoder.h"
#include "CharmInfo.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DBCEnums.h"
#include "DBCStores.h"
#include "Item.h"
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
#include <cmath>

namespace
{
    using Animus::Curriculum::ActionCatalog;
    using Animus::Curriculum::Layout;
    using Animus::Curriculum::LayoutConstants;
    using Animus::Curriculum::RangeBand;
    using Animus::Curriculum::Role;
    using Animus::Curriculum::SeatActionResult;
    using Animus::Curriculum::SeatEncoder;
    using Animus::Curriculum::SeatView;
    using Animus::Curriculum::Stage;
    using Animus::Curriculum::TalentBuilder;

    constexpr std::array<uint8, 10> PLAYABLE_RACES =
    {
        RACE_HUMAN, RACE_ORC, RACE_DWARF, RACE_NIGHTELF, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_GNOME, RACE_TROLL,
        RACE_BLOODELF, RACE_DRAENEI
    };

    constexpr std::array<ShapeshiftForm, 13> TRACKED_FORMS =
    {
        FORM_NONE, FORM_CAT, FORM_TREE, FORM_BEAR, FORM_DIREBEAR, FORM_MOONKIN, FORM_SHADOW, FORM_STEALTH,
        FORM_BATTLESTANCE, FORM_DEFENSIVESTANCE, FORM_BERSERKERSTANCE, FORM_METAMORPHOSIS, FORM_GHOSTWOLF
    };

    /// The classes, in the order of every class one-hot (owner, teammates, opponent).
    constexpr std::array<uint8, 10> CLASSES =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_DEATH_KNIGHT, CLASS_SHAMAN,
        CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID
    };

    enum EncoderSpells : uint32
    {
        SPELL_CALL_PET          = 883,      // its GCD is applied to calling a stabled beast
    };

    constexpr float GCD_MS = 1500.0f;
    constexpr float RUNE_COOLDOWN_MS = 10000.0f;
    constexpr float TALENT_POINTS_AT_MAX_LEVEL = 71.0f;

    constexpr uint32 DUEL_MOVE_POINT_ID = 1;
    constexpr uint32 FOLLOW_MOVE_POINT_ID = 3;
    constexpr uint32 FOLLOW_TANK_MOVE_POINT_ID = 4;
    constexpr float RANGED_DESIRED_RANGE = 25.0f;
    constexpr float BACK_OFF_DISTANCE = 10.0f;
    constexpr float FOLLOW_DISTANCE = 2.0f;
    constexpr float FOLLOW_MIN_DISTANCE = 4.0f;
    constexpr float FOLLOW_TANK_DISTANCE = 4.0f;
    constexpr float FOLLOW_TANK_MIN_DISTANCE = 8.0f;
    constexpr uint32 CALL_BEAST_GCD_MS = 1500;
    constexpr uint8 HUNTER_PET_LEVEL = 10;

    constexpr uint32 IMMOBILE_STATES = UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;
    constexpr uint32 STUN_STATES = UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING;
    constexpr uint32 CROWD_CONTROL_STATES = STUN_STATES | UNIT_STATE_ROOT;

    float CooldownFraction(Player const* bot, SpellInfo const* info)
    {
        uint32 const full = std::max(info->RecoveryTime, info->CategoryRecoveryTime);
        return full ? std::min(1.0f, float(bot->GetSpellCooldownDelay(info->Id)) / float(full)) : 0.0f;
    }

    float AuraFraction(Unit const* unit, uint32 spellId, ObjectGuid caster, float& stacks)
    {
        Aura const* aura = unit->GetAura(spellId, caster);
        if (!aura)
            return 0.0f;

        stacks = std::max(stacks, std::min(1.0f, float(std::max(aura->GetStackAmount(), aura->GetCharges())) / 5.0f));
        if (aura->GetMaxDuration() <= 0)
            return 1.0f;    // permanent (stances, forms, presences, auras)

        return std::clamp(float(aura->GetDuration()) / float(aura->GetMaxDuration()), 0.0f, 1.0f);
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

    /// A cast in its cast time (channels excluded): the client refuses to start another spell or use an item
    /// meanwhile. The core only checks this for client casts (m_cast_count), so the bot's actions check it
    /// here -- otherwise a new cast would silently cancel the one in progress. Stopping it is its own action.
    bool CastInProgress(Player const* bot)
    {
        return bot->IsNonMeleeSpellCast(false, true, true);
    }

    /// The core's own cast validation, without casting: cooldown, GCD, power, stance, range, facing,
    /// reagents, reactive requirements. Same pattern as PetAI.
    bool CanCast(Player* bot, SpellInfo const* info, Unit* target, Item* castItem = nullptr)
    {
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        spell->m_CastItem = castItem;
        spell->LoadScripts();

        SpellCastTargets targets = TargetsFor(info, bot, target);
        spell->InitExplicitTargets(targets);

        SpellCastResult const result = spell->CheckCast(true);
        delete spell;

        return result == SPELL_CAST_OK;
    }

    /// CanCast with an explicit friendly unit target (heals on an ally).
    bool CanCastOn(Player* bot, SpellInfo const* info, Unit* target)
    {
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        spell->LoadScripts();

        SpellCastTargets targets;
        targets.SetUnitTarget(target);
        spell->InitExplicitTargets(targets);

        SpellCastResult const result = spell->CheckCast(true);
        delete spell;
        return result == SPELL_CAST_OK;
    }

    /// Whether an ally heal can be cast on `ally` now.
    bool CanHeal(Player* bot, ActionCatalog::Action const& heal, Unit* ally)
    {
        SpellInfo const* info = ActionCatalog::KnownRank(bot, heal.FirstRank);
        if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id)
            || bot->GetGlobalCooldownMgr().HasGlobalCooldown(info) || bot->IsNonMeleeSpellCast(false, true, true))
            return false;

        if (!bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
            return false;

        return CanCastOn(bot, info, ally);
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

    Unit* FirstPet(Player* bot)
    {
        if (Pet* pet = bot->GetPet())
            return pet;

        for (Unit* controlled : bot->m_Controlled)
            if (controlled->IsAlive() && !controlled->IsTotem())
                return controlled;

        return nullptr;
    }

    /// A shapeshift the player could cancel from the client (druid forms, Shadowform, Ghost Wolf, Stealth);
    /// stances and presences cannot be.
    SpellInfo const* CancellableForm(Player const* bot)
    {
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

    SpellInfo const* UseSpell(uint32 itemEntry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemEntry);
        if (!proto || proto->Spells[0].SpellId <= 0 || proto->Spells[0].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            return nullptr;

        return sSpellMgr->GetSpellInfo(proto->Spells[0].SpellId);
    }

    /// The enemy slot of `unit`, or -1.
    int32 SlotOf(SeatView const& view, Unit const* unit)
    {
        if (!unit)
            return -1;

        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            if (view.Enemies[slot] == unit)
                return int32(slot);

        return -1;
    }

    /// An enemy slot, other than `except`, whose living enemy attacks `victim`; -1 if none.
    int32 SlotAttacking(SeatView const& view, Unit const* victim, uint32 except)
    {
        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            if (Unit* enemy = view.Enemies[slot]; enemy && enemy->IsAlive() && enemy->GetVictim() == victim
                && slot != except)
                return int32(slot);

        return -1;
    }

    /// Select enemy `slot` and keep swinging, at the new target.
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

    // Stage 1: the class's spells and trinkets.

    bool IsSpellActionAllowed(SeatView const& view, Unit* target, ActionCatalog::Action const& def)
    {
        Player* bot = view.Bot;

        // Cheap rejections before the full cast check.
        SpellInfo const* info = ActionCatalog::KnownRank(bot, def.FirstRank);
        if (!info || !bot->HasActiveSpell(info->Id) || bot->HasSpellCooldown(info->Id) || CastInProgress(bot))
            return false;

        if (def.NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
            return false;

        if (bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
            return false;

        // Server-driven movement does not set the movement flags CheckCast looks at: no cast-time or
        // channeled spell while running.
        if (view.L->Has(Stage::Duel) && !bot->movespline->Finalized() && (info->CalcCastTime(bot) || info->IsChanneled()))
            return false;

        return CanCast(bot, info, target);
    }

    bool IsActionAllowed(SeatView const& view, uint32 action)
    {
        ActionCatalog::Action const& def = view.L->Catalog().Actions()[action];
        Player* bot = view.Bot;

        switch (def.Type)
        {
            case ActionCatalog::Kind::Noop:
                return true;
            case ActionCatalog::Kind::CancelQueued:
                return bot->GetCurrentSpell(CURRENT_MELEE_SPELL) != nullptr;
            case ActionCatalog::Kind::Trinket:
            {
                Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, def.EquipmentSlot);
                SpellInfo const* info = SeatEncoder::TrinketSpell(item);
                return info && !CastInProgress(bot) && !bot->HasSpellCooldown(info->Id)
                    && CanCast(bot, info, view.Target, item);
            }
            case ActionCatalog::Kind::Spell:
                break;
        }

        return IsSpellActionAllowed(view, view.Target, def);
    }

    /// Casts a spell action at `target` (may be null: self-cast spells only). Returns true if it started.
    bool ApplySpellAction(SeatView const& view, Unit* target, ActionCatalog::Action const& def,
        SeatActionResult& result)
    {
        Player* bot = view.Bot;
        if (def.NextSwing && bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
            return false;

        SpellInfo const* info = ActionCatalog::KnownRank(bot, def.FirstRank);
        if (!info || !bot->HasActiveSpell(info->Id) || CastInProgress(bot))
            return false;

        // Same path as CMSG_CAST_SPELL. prepare() runs the full cast validation again, so a masked action
        // from a misbehaving client simply fails. The spell owns and frees itself.
        SpellCastTargets targets = TargetsFor(info, bot, target);
        bool const stealthed = bot->HasAuraType(SPELL_AURA_MOD_STEALTH);
        bool const targetCasting = target && target->IsNonMeleeSpellCast(false);
        Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
        if (spell->prepare(&targets) != SPELL_CAST_OK)
            return false;

        ++result.SpellCasts;

        // A stealth opener (Ambush, Garrote, Cheap Shot, Ravage, Pounce, ...) on the opponent.
        if (view.L->Has(Stage::Duel) && stealthed && info->HasAttribute(SPELL_ATTR0_ONLY_STEALTHED)
            && info->NeedsExplicitUnitTarget() && !info->IsPositive())
            result.StealthOpener = true;

        // An interrupt attempt on a casting enemy; the caller checks next decision whether the cast stopped.
        if (view.L->Has(Stage::Pack) && targetCasting && target != bot && ActionCatalog::IsInterruptingSpell(info))
            result.PendingInterrupt = target->GetGUID();

        return true;
    }

    // Stage 2: the duel.

    bool IsDuelActionAllowed(SeatView const& view, uint32 duelAction)
    {
        Player* bot = view.Bot;
        Unit* opponent = view.Target;
        if (!bot->IsAlive())
            return false;

        bool const casting = bot->IsNonMeleeSpellCast(false, false, true);

        // No target needed (between gauntlet pulls too).
        if (duelAction == LayoutConstants::DUEL_ACTION_STOP_CASTING)
            return casting;
        if (duelAction == LayoutConstants::DUEL_ACTION_CANCEL_FORM)
            return CancellableForm(bot) != nullptr;

        if (!opponent || !opponent->IsAlive())
            return false;
        bool const canMove = !casting && !bot->HasUnitState(IMMOBILE_STATES);

        switch (duelAction)
        {
            case LayoutConstants::DUEL_ACTION_MOVE_TO_TARGET:
            case LayoutConstants::DUEL_ACTION_MOVE_BEHIND:
            case LayoutConstants::DUEL_ACTION_MOVE_TO_RANGE:
            case LayoutConstants::DUEL_ACTION_BACK_OFF:
                return canMove;
            case LayoutConstants::DUEL_ACTION_STOP:
                return !bot->movespline->Finalized();
            case LayoutConstants::DUEL_ACTION_START_ATTACK:
                return bot->GetVictim() != opponent && bot->IsValidAttackTarget(opponent);
            case LayoutConstants::DUEL_ACTION_PET_ATTACK:
                return std::any_of(bot->m_Controlled.begin(), bot->m_Controlled.end(), [opponent](Unit* pet)
                {
                    return pet->IsAlive() && pet->IsCreature() && pet->GetVictim() != opponent;
                });
            default:
                break;
        }

        uint32 const slot = duelAction - LayoutConstants::DUEL_ACTION_CALL_BEAST_FIRST;
        if (slot >= view.StableCount || casting || bot->GetPetGUID() || bot->GetLevel() < HUNTER_PET_LEVEL)
            return false;

        SpellInfo const* callPet = sSpellMgr->GetSpellInfo(SPELL_CALL_PET);
        return !callPet || !bot->GetGlobalCooldownMgr().HasGlobalCooldown(callPet);
    }

    void ApplyDuelAction(SeatView const& view, uint32 duelAction, SeatActionResult& result)
    {
        if (!IsDuelActionAllowed(view, duelAction))
            return;

        Player* bot = view.Bot;
        Unit* opponent = view.Target;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        switch (duelAction)
        {
            case LayoutConstants::DUEL_ACTION_MOVE_TO_TARGET:
                opponent->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), 0.5f, opponent->GetAngle(bot));
                break;
            case LayoutConstants::DUEL_ACTION_MOVE_BEHIND:
                opponent->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), 0.5f,
                    Position::NormalizeOrientation(opponent->GetOrientation() + float(M_PI)));
                break;
            case LayoutConstants::DUEL_ACTION_MOVE_TO_RANGE:
                opponent->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), RANGED_DESIRED_RANGE - 1.0f,
                    opponent->GetAngle(bot));
                break;
            case LayoutConstants::DUEL_ACTION_BACK_OFF:
                opponent->GetNearPoint(bot, x, y, z, bot->GetCombatReach(),
                    bot->GetDistance(opponent) + BACK_OFF_DISTANCE, opponent->GetAngle(bot));
                break;
            case LayoutConstants::DUEL_ACTION_STOP:
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
                return;
            case LayoutConstants::DUEL_ACTION_START_ATTACK:
                bot->Attack(opponent, true);
                return;
            case LayoutConstants::DUEL_ACTION_PET_ATTACK:
                SeatEncoder::PetAttack(bot, opponent);
                return;
            case LayoutConstants::DUEL_ACTION_STOP_CASTING:
                // As CMSG_CANCEL_CAST / CMSG_CANCEL_CHANNELLING: the current cast or channel, cancelled by the caster.
                bot->InterruptNonMeleeSpells(false, 0, false, true);
                return;
            case LayoutConstants::DUEL_ACTION_CANCEL_FORM:
                // As CMSG_CANCEL_AURA.
                if (SpellInfo const* form = CancellableForm(bot))
                    bot->RemoveOwnedAura(form->Id, ObjectGuid::Empty, 0, AURA_REMOVE_BY_CANCEL);
                return;
            default:
                result.CallBeast = view.Stable[duelAction - LayoutConstants::DUEL_ACTION_CALL_BEAST_FIRST];
                return;
        }

        MoveTo(bot, DUEL_MOVE_POINT_ID, x, y, z);
    }

    void ObserveDuel(SeatView const& view, float* obs)
    {
        Player* bot = view.Bot;
        Unit* opponent = view.Target;
        float* duel = obs + view.L->DuelObsFirst;

        // The bot's own casting and form, with or without a target.
        if (Spell* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            cast && cast->getState() == SPELL_STATE_PREPARING && cast->GetCastTime() > 0)
        {
            float const total = float(cast->GetCastTime());
            float const left = std::clamp(float(cast->GetCastTimeRemaining()), 0.0f, total);
            duel[LayoutConstants::DUEL_OBS_CAST_PROGRESS] = 1.0f - left / total;
            duel[LayoutConstants::DUEL_OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
        }
        else if (Spell* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
            channel && channel->getState() == SPELL_STATE_CASTING)
        {
            float const left = float(std::max(0, channel->GetCastTimeRemaining()));
            float const total = std::max(left, float(std::max(1, channel->m_spellInfo->GetMaxDuration())));
            duel[LayoutConstants::DUEL_OBS_CAST_PROGRESS] = 1.0f - left / total;
            duel[LayoutConstants::DUEL_OBS_CAST_REMAINING] = std::min(1.0f, left / 3000.0f);
        }

        duel[LayoutConstants::DUEL_OBS_SHAPESHIFTED] = CancellableForm(bot) ? 1.0f : 0.0f;
        duel[LayoutConstants::DUEL_OBS_COMBAT_TIME] = view.CombatTime;

        // Hunters: what each stable slot offers, so the policy can find the pet it prefers.
        for (uint32 slot = 0; slot < view.StableCount && slot < LayoutConstants::STABLE_SLOTS; ++slot)
        {
            CreatureTemplate const* beast = sObjectMgr->GetCreatureTemplate(view.Stable[slot]);
            if (!beast)
                continue;

            float* features = duel + LayoutConstants::DUEL_OBS_STABLE_FIRST + slot * LayoutConstants::STABLE_FEATURES;
            features[0] = 1.0f;
            features[1] = float(beast->family) / 50.0f;

            if (CreatureFamilyEntry const* family = sCreatureFamilyStore.LookupEntry(beast->family))
                if (family->petTalentType >= 0 && family->petTalentType < 3)
                    features[2 + family->petTalentType] = 1.0f;     // ferocity, tenacity, cunning
        }

        if (!opponent)
            return;

        float const bearing = bot->GetRelativeAngle(opponent);
        duel[LayoutConstants::DUEL_OBS_DISTANCE] = std::min(1.0f, bot->GetDistance(opponent) / 60.0f);
        duel[LayoutConstants::DUEL_OBS_BEARING_SIN] = std::sin(bearing);
        duel[LayoutConstants::DUEL_OBS_BEARING_COS] = std::cos(bearing);
        duel[LayoutConstants::DUEL_OBS_BEHIND_TARGET] = opponent->isInBack(bot) ? 1.0f : 0.0f;
        duel[LayoutConstants::DUEL_OBS_TARGET_FACING_BOT] = opponent->HasInArc(float(M_PI), bot) ? 1.0f : 0.0f;
        duel[LayoutConstants::DUEL_OBS_TARGET_IN_COMBAT] = opponent->IsInCombat() ? 1.0f : 0.0f;
        duel[LayoutConstants::DUEL_OBS_TARGET_ATTACKS_BOT] = opponent->GetVictim() == bot ? 1.0f : 0.0f;
        duel[LayoutConstants::DUEL_OBS_TARGET_CASTING] = opponent->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
        duel[LayoutConstants::DUEL_OBS_BOT_MOVING] = bot->movespline->Finalized() ? 0.0f : 1.0f;
        duel[LayoutConstants::DUEL_OBS_BOT_IN_COMBAT] = bot->IsInCombat() ? 1.0f : 0.0f;
        duel[LayoutConstants::DUEL_OBS_BOT_STEALTHED] = bot->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;
        duel[LayoutConstants::DUEL_OBS_BOT_AUTO_ATTACKING] = bot->GetVictim() == opponent
            && bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING) ? 1.0f : 0.0f;
        duel[LayoutConstants::DUEL_OBS_DAMAGE_TAKEN] = view.LastStepDamageTaken;

        if (Unit* pet = FirstPet(bot))
        {
            duel[LayoutConstants::DUEL_OBS_PET_OUT] = 1.0f;
            duel[LayoutConstants::DUEL_OBS_PET_HEALTH] = pet->GetHealthPct() / 100.0f;
            duel[LayoutConstants::DUEL_OBS_PET_ATTACKING] = pet->GetVictim() == opponent ? 1.0f : 0.0f;
        }
    }

    // Stages 3 and 4: pack and gauntlet.

    void ObservePack(SeatView const& view, float* obs)
    {
        Player* bot = view.Bot;
        float* pack = obs + view.L->PackObsFirst;

        uint32 alive = 0;
        uint32 inCombat = 0;
        for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
        {
            Unit* enemy = view.Enemies[slot];
            if (!enemy)
                continue;

            float* features = pack + LayoutConstants::PACK_OBS_GLOBAL_COUNT + slot * LayoutConstants::SLOT_FEATURES;
            float const bearing = bot->GetRelativeAngle(enemy);
            Unit const* victim = enemy->GetVictim();

            features[LayoutConstants::SLOT_PRESENT] = 1.0f;
            features[LayoutConstants::SLOT_ALIVE] = enemy->IsAlive() ? 1.0f : 0.0f;
            features[LayoutConstants::SLOT_HEALTH] = enemy->GetHealthPct() / 100.0f;
            features[LayoutConstants::SLOT_DISTANCE] = std::min(1.0f, bot->GetDistance(enemy) / 60.0f);
            features[LayoutConstants::SLOT_BEARING_SIN] = std::sin(bearing);
            features[LayoutConstants::SLOT_BEARING_COS] = std::cos(bearing);
            features[LayoutConstants::SLOT_BEHIND] = enemy->isInBack(bot) ? 1.0f : 0.0f;
            features[LayoutConstants::SLOT_ATTACKS_BOT] = victim == bot ? 1.0f : 0.0f;
            features[LayoutConstants::SLOT_ATTACKS_PET] = victim && victim != bot
                && victim->GetOwnerGUID() == bot->GetGUID() ? 1.0f : 0.0f;
            features[LayoutConstants::SLOT_CASTING] = enemy->IsNonMeleeSpellCast(false) ? 1.0f : 0.0f;
            features[LayoutConstants::SLOT_IN_COMBAT] = enemy->IsInCombat() ? 1.0f : 0.0f;
            features[LayoutConstants::SLOT_CROWD_CONTROLLED] = IsCrowdControlled(enemy) ? 1.0f : 0.0f;
            features[LayoutConstants::SLOT_CURRENT_TARGET] = slot == view.TargetSlot ? 1.0f : 0.0f;
            features[LayoutConstants::SLOT_ELITE] = enemy->ToCreature() && enemy->ToCreature()->isElite() ? 1.0f : 0.0f;
            features[LayoutConstants::SLOT_LEVEL_DIFFERENCE] = (float(enemy->GetLevel()) - float(bot->GetLevel())) / 5.0f;

            alive += enemy->IsAlive() ? 1 : 0;
            inCombat += enemy->IsAlive() && enemy->IsInCombat() ? 1 : 0;
        }

        pack[LayoutConstants::PACK_OBS_ALIVE] = float(alive) / float(LayoutConstants::PACK_SLOTS);
        pack[LayoutConstants::PACK_OBS_IN_COMBAT] = float(inCombat) / float(LayoutConstants::PACK_SLOTS);

        float* tactical = pack + LayoutConstants::PACK_OBS_GLOBAL_COUNT
            + LayoutConstants::PACK_SLOTS * LayoutConstants::SLOT_FEATURES;
        std::vector<ActionCatalog::Action> const& actions = view.L->Catalog().Tactical();
        for (uint32 i = 0; i < actions.size(); ++i)
        {
            if (SpellInfo const* info = ActionCatalog::KnownRank(bot, actions[i].FirstRank))
            {
                tactical[i * 2] = 1.0f;
                tactical[i * 2 + 1] = CooldownFraction(bot, info);
            }
        }
    }

    bool IsPackActionAllowed(SeatView const& view, uint32 packAction)
    {
        // Only the target slots; tactical spells are masked by IsSpellActionAllowed.
        if (packAction >= LayoutConstants::PACK_SLOTS || packAction >= view.EnemyCount
            || packAction == view.TargetSlot || !view.Bot->IsAlive())
            return false;

        Unit const* enemy = view.Enemies[packAction];
        return enemy && enemy->IsAlive();
    }

    void ApplyPackAction(SeatView& view, uint32 packAction, SeatActionResult& result)
    {
        if (packAction >= LayoutConstants::PACK_SLOTS)
        {
            if (view.Target)
                ApplySpellAction(view, view.Target,
                    view.L->Catalog().Tactical()[packAction - LayoutConstants::PACK_SLOTS], result);
            return;
        }

        if (IsPackActionAllowed(view, packAction))
            SelectEnemy(view, packAction);
    }

    void ObserveGauntlet(SeatView const& view, float* obs)
    {
        Player* bot = view.Bot;
        float* gauntlet = obs + view.L->GauntletObsFirst;
        bool const pullActive = view.EnemyCount > 0;

        gauntlet[LayoutConstants::GAUNTLET_OBS_PULLS_CLEARED] = std::min(1.0f, float(view.PullsCleared) / 10.0f);
        gauntlet[LayoutConstants::GAUNTLET_OBS_PULL_ACTIVE] = pullActive ? 1.0f : 0.0f;
        gauntlet[LayoutConstants::GAUNTLET_OBS_QUIET_TIME] = pullActive ? 0.0f : view.QuietTime;
        gauntlet[LayoutConstants::GAUNTLET_OBS_PULL_TIME] = pullActive ? view.PullTime : 0.0f;
        gauntlet[LayoutConstants::GAUNTLET_OBS_ELITE_PULL] = pullActive && view.ElitePull ? 1.0f : 0.0f;
        gauntlet[LayoutConstants::GAUNTLET_OBS_EATING] = bot->HasAuraType(SPELL_AURA_MOD_REGEN) ? 1.0f : 0.0f;
        gauntlet[LayoutConstants::GAUNTLET_OBS_DRINKING] = bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN) ? 1.0f : 0.0f;
        gauntlet[LayoutConstants::GAUNTLET_OBS_FOOD_LEFT] = view.FoodItem
            ? float(bot->GetItemCount(view.FoodItem)) / float(LayoutConstants::CONSUMABLE_COUNT) : 0.0f;
        gauntlet[LayoutConstants::GAUNTLET_OBS_DRINK_LEFT] = view.DrinkItem
            ? float(bot->GetItemCount(view.DrinkItem)) / float(LayoutConstants::CONSUMABLE_COUNT) : 0.0f;

        float* sustain = gauntlet + LayoutConstants::GAUNTLET_OBS_GLOBAL_COUNT;
        std::vector<ActionCatalog::Action> const& actions = view.L->Catalog().Sustain();
        for (uint32 i = 0; i < actions.size(); ++i)
        {
            if (SpellInfo const* info = ActionCatalog::KnownRank(bot, actions[i].FirstRank))
            {
                sustain[i * 2] = 1.0f;
                sustain[i * 2 + 1] = CooldownFraction(bot, info);
            }
        }
    }

    bool IsGauntletActionAllowed(SeatView const& view, uint32 gauntletAction)
    {
        Player* bot = view.Bot;
        if (gauntletAction >= LayoutConstants::GAUNTLET_ACTION_SUSTAIN_FIRST)
            return IsSpellActionAllowed(view, view.Target,
                view.L->Catalog().Sustain()[gauntletAction - LayoutConstants::GAUNTLET_ACTION_SUSTAIN_FIRST]);

        bool const eat = gauntletAction == LayoutConstants::GAUNTLET_ACTION_EAT;
        uint32 const item = eat ? view.FoodItem : view.DrinkItem;
        SpellInfo const* info = item ? UseSpell(item) : nullptr;

        return info && bot->IsAlive() && !bot->IsInCombat() && bot->movespline->Finalized()
            && !bot->IsNonMeleeSpellCast(false) && bot->GetItemCount(item) && !bot->HasSpellCooldown(info->Id)
            && !bot->HasAuraType(eat ? SPELL_AURA_MOD_REGEN : SPELL_AURA_MOD_POWER_REGEN);
    }

    void ApplyGauntletAction(SeatView const& view, uint32 gauntletAction, SeatActionResult& result)
    {
        if (!IsGauntletActionAllowed(view, gauntletAction))
            return;

        Player* bot = view.Bot;
        if (gauntletAction >= LayoutConstants::GAUNTLET_ACTION_SUSTAIN_FIRST)
        {
            if (ApplySpellAction(view, view.Target,
                view.L->Catalog().Sustain()[gauntletAction - LayoutConstants::GAUNTLET_ACTION_SUSTAIN_FIRST], result))
                ++result.SustainCasts;
            return;
        }

        bool const eat = gauntletAction == LayoutConstants::GAUNTLET_ACTION_EAT;
        Item* item = bot->GetItemByEntry(eat ? view.FoodItem : view.DrinkItem);
        if (!item)
            return;

        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        bot->CastItemUseSpell(item, targets, 1, 0);

        if (bot->HasAuraType(eat ? SPELL_AURA_MOD_REGEN : SPELL_AURA_MOD_POWER_REGEN))
            ++(eat ? result.FoodUsed : result.DrinkUsed);
    }

    // Stage 5: the owner.

    void ObserveCompanion(SeatView const& view, float* obs)
    {
        Player* bot = view.Bot;
        Layout const& layout = *view.L;
        float* companion = obs + layout.CompanionObsFirst;

        Player* owner = view.Owner;
        if (owner && owner->IsInMap(bot))
        {
            float const bearing = bot->GetRelativeAngle(owner);
            companion[LayoutConstants::COMPANION_OBS_OWNER_PRESENT] = 1.0f;
            companion[LayoutConstants::COMPANION_OBS_OWNER_ALIVE] = owner->IsAlive() ? 1.0f : 0.0f;
            companion[LayoutConstants::COMPANION_OBS_OWNER_HEALTH] = owner->GetHealthPct() / 100.0f;
            if (uint32 const maxMana = owner->GetMaxPower(POWER_MANA))
                companion[LayoutConstants::COMPANION_OBS_OWNER_MANA] = float(owner->GetPower(POWER_MANA)) / float(maxMana);
            companion[LayoutConstants::COMPANION_OBS_OWNER_DISTANCE] = std::min(1.0f, bot->GetDistance(owner) / 40.0f);
            companion[LayoutConstants::COMPANION_OBS_OWNER_BEARING_SIN] = std::sin(bearing);
            companion[LayoutConstants::COMPANION_OBS_OWNER_BEARING_COS] = std::cos(bearing);
            companion[LayoutConstants::COMPANION_OBS_OWNER_IN_COMBAT] = owner->IsInCombat() ? 1.0f : 0.0f;
            companion[LayoutConstants::COMPANION_OBS_OWNER_MOVING] = owner->movespline->Finalized() ? 0.0f : 1.0f;
            companion[LayoutConstants::COMPANION_OBS_OWNER_LEVEL_DIFF] =
                (float(owner->GetLevel()) - float(bot->GetLevel())) / 5.0f;

            for (uint32 i = 0; i < CLASSES.size(); ++i)
                companion[LayoutConstants::COMPANION_OBS_OWNER_CLASS_FIRST + i] =
                    CLASSES[i] == owner->getClass() ? 1.0f : 0.0f;

            int32 const ownerTarget = SlotOf(view, owner->GetVictim());
            if (ownerTarget >= 0)
                companion[LayoutConstants::COMPANION_OBS_OWNER_TARGET_FIRST + ownerTarget] = 1.0f;
            else
                companion[LayoutConstants::COMPANION_OBS_OWNER_NO_TARGET] = 1.0f;

            uint32 attackers = 0;
            for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            {
                Unit* enemy = view.Enemies[slot];
                if (enemy && enemy->IsAlive() && enemy->GetVictim() == owner)
                {
                    companion[LayoutConstants::COMPANION_OBS_SLOT_ON_OWNER_FIRST + slot] = 1.0f;
                    ++attackers;
                }
            }

            companion[LayoutConstants::COMPANION_OBS_OWNER_ATTACKERS] =
                float(attackers) / float(LayoutConstants::PACK_SLOTS);
        }

        float* heals = companion + LayoutConstants::COMPANION_OBS_GLOBAL_COUNT;
        for (uint32 i = 0; i < layout.AllyHeals.size(); ++i)
        {
            if (SpellInfo const* info = ActionCatalog::KnownRank(bot, layout.AllyHeals[i].FirstRank))
            {
                heals[i * 2] = 1.0f;
                heals[i * 2 + 1] = CooldownFraction(bot, info);
            }
        }
    }

    bool IsCompanionActionAllowed(SeatView const& view, uint32 companionAction)
    {
        Player* bot = view.Bot;
        Player* owner = view.Owner;
        if (!owner || !owner->IsAlive() || !bot->IsAlive() || !owner->IsInMap(bot))
            return false;

        bool const casting = bot->IsNonMeleeSpellCast(false, false, true);

        switch (companionAction)
        {
            case LayoutConstants::COMPANION_ACTION_FOLLOW:
                return !casting && !bot->HasUnitState(IMMOBILE_STATES) && bot->GetDistance(owner) > FOLLOW_MIN_DISTANCE;
            case LayoutConstants::COMPANION_ACTION_ASSIST:
            {
                int32 const slot = SlotOf(view, owner->GetVictim());
                return slot >= 0 && uint32(slot) != view.TargetSlot && owner->GetVictim()->IsAlive();
            }
            case LayoutConstants::COMPANION_ACTION_GUARD:
                return SlotAttacking(view, owner, view.TargetSlot) >= 0;
            default:
                break;
        }

        return CanHeal(bot, view.L->AllyHeals[companionAction - LayoutConstants::COMPANION_ACTION_HEAL_FIRST], owner);
    }

    void ApplyCompanionAction(SeatView& view, uint32 companionAction, SeatActionResult& result)
    {
        if (!IsCompanionActionAllowed(view, companionAction))
            return;

        Player* bot = view.Bot;
        Player* owner = view.Owner;

        switch (companionAction)
        {
            case LayoutConstants::COMPANION_ACTION_FOLLOW:
            {
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                owner->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), FOLLOW_DISTANCE,
                    Position::NormalizeOrientation(owner->GetOrientation() + float(M_PI)));
                MoveTo(bot, FOLLOW_MOVE_POINT_ID, x, y, z);
                return;
            }
            case LayoutConstants::COMPANION_ACTION_ASSIST:
            case LayoutConstants::COMPANION_ACTION_GUARD:
            {
                int32 const slot = companionAction == LayoutConstants::COMPANION_ACTION_ASSIST
                    ? SlotOf(view, owner->GetVictim()) : SlotAttacking(view, owner, view.TargetSlot);
                if (slot >= 0 && view.Enemies[slot])
                    SelectEnemy(view, uint32(slot));
                return;
            }
            default:
                break;
        }

        Heal(bot, view.L->AllyHeals[companionAction - LayoutConstants::COMPANION_ACTION_HEAL_FIRST], owner, result);
    }

    // Stage 6: the party.

    void ObserveParty(SeatView const& view, float* obs)
    {
        Player* bot = view.Bot;
        float* party = obs + view.L->PartyObsFirst;

        uint32 alive = bot->IsAlive() ? 1 : 0;
        float lowest = 1.0f;
        if (Player* owner = view.Owner; owner && owner->IsAlive())
        {
            ++alive;
            lowest = std::min(lowest, owner->GetHealthPct() / 100.0f);
        }

        for (uint32 member = 0; member < LayoutConstants::PARTY_MEMBERS; ++member)
        {
            SeatView::Teammate const& other = view.Teammates[member];
            Player* teammate = other.Bot;
            if (!teammate)
                continue;

            float* features = party + LayoutConstants::PARTY_OBS_GLOBAL_COUNT + member * LayoutConstants::MEMBER_FEATURES;
            float const bearing = bot->GetRelativeAngle(teammate);

            features[LayoutConstants::MEMBER_PRESENT] = 1.0f;
            features[LayoutConstants::MEMBER_ALIVE] = teammate->IsAlive() ? 1.0f : 0.0f;
            features[LayoutConstants::MEMBER_HEALTH] = teammate->GetHealthPct() / 100.0f;
            if (uint32 const maxMana = teammate->GetMaxPower(POWER_MANA))
                features[LayoutConstants::MEMBER_MANA] = float(teammate->GetPower(POWER_MANA)) / float(maxMana);
            features[LayoutConstants::MEMBER_DISTANCE] = std::min(1.0f, bot->GetDistance(teammate) / 40.0f);
            features[LayoutConstants::MEMBER_BEARING_SIN] = std::sin(bearing);
            features[LayoutConstants::MEMBER_BEARING_COS] = std::cos(bearing);
            features[LayoutConstants::MEMBER_IN_COMBAT] = teammate->IsInCombat() ? 1.0f : 0.0f;
            features[LayoutConstants::MEMBER_ROLE_FIRST + uint32(other.PlayRole)] = 1.0f;

            for (uint32 i = 0; i < CLASSES.size(); ++i)
                features[LayoutConstants::MEMBER_CLASS_FIRST + i] = CLASSES[i] == other.Class ? 1.0f : 0.0f;

            int32 const target = SlotOf(view, teammate->GetVictim());
            if (target >= 0)
                features[LayoutConstants::MEMBER_TARGET_FIRST + target] = 1.0f;
            else
                features[LayoutConstants::MEMBER_NO_TARGET] = 1.0f;

            uint32 attackers = 0;
            for (uint32 slot = 0; slot < view.EnemyCount; ++slot)
            {
                Unit* enemy = view.Enemies[slot];
                if (enemy && enemy->IsAlive() && enemy->GetVictim() == teammate)
                {
                    features[LayoutConstants::MEMBER_SLOT_ON_FIRST + slot] = 1.0f;
                    ++attackers;
                }
            }
            features[LayoutConstants::MEMBER_ATTACKERS] = float(attackers) / float(LayoutConstants::PACK_SLOTS);

            if (teammate->IsAlive())
            {
                ++alive;
                lowest = std::min(lowest, teammate->GetHealthPct() / 100.0f);
                if (other.PlayRole == Role::Tank)
                    party[LayoutConstants::PARTY_OBS_HAS_TANK] = 1.0f;
                if (other.PlayRole == Role::Heal)
                    party[LayoutConstants::PARTY_OBS_HAS_HEALER] = 1.0f;
            }
        }

        party[LayoutConstants::PARTY_OBS_ALIVE] = float(alive) / float(LayoutConstants::PARTY_MEMBERS + 2);
        party[LayoutConstants::PARTY_OBS_LOWEST_HEALTH] = lowest;
    }

    bool IsPartyActionAllowed(SeatView const& view, uint32 partyAction)
    {
        Player* bot = view.Bot;
        if (!bot->IsAlive())
            return false;

        if (partyAction == LayoutConstants::PARTY_ACTION_FOLLOW_TANK)
        {
            Player* tank = view.Tank;
            return tank && tank != bot && !bot->IsNonMeleeSpellCast(false, false, true)
                && !bot->HasUnitState(IMMOBILE_STATES) && bot->GetDistance(tank) > FOLLOW_TANK_MIN_DISTANCE;
        }

        if (partyAction < LayoutConstants::PARTY_ACTION_GUARD_FIRST)
        {
            Player* teammate = view.Teammates[partyAction - LayoutConstants::PARTY_ACTION_ASSIST_FIRST].Bot;
            if (!teammate || !teammate->IsAlive())
                return false;

            int32 const slot = SlotOf(view, teammate->GetVictim());
            return slot >= 0 && uint32(slot) != view.TargetSlot && teammate->GetVictim()->IsAlive();
        }

        if (partyAction < LayoutConstants::PARTY_ACTION_HEAL_FIRST)
        {
            Player* teammate = view.Teammates[partyAction - LayoutConstants::PARTY_ACTION_GUARD_FIRST].Bot;
            return teammate && teammate->IsAlive() && SlotAttacking(view, teammate, view.TargetSlot) >= 0;
        }

        uint32 const heals = uint32(view.L->AllyHeals.size());
        uint32 const index = partyAction - LayoutConstants::PARTY_ACTION_HEAL_FIRST;
        Player* teammate = heals ? view.Teammates[index / heals].Bot : nullptr;
        if (!teammate || !teammate->IsAlive())
            return false;

        return CanHeal(bot, view.L->AllyHeals[index % heals], teammate);
    }

    void ApplyPartyAction(SeatView& view, uint32 partyAction, SeatActionResult& result)
    {
        if (!IsPartyActionAllowed(view, partyAction))
            return;

        Player* bot = view.Bot;

        if (partyAction == LayoutConstants::PARTY_ACTION_FOLLOW_TANK)
        {
            Player* tank = view.Tank;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            tank->GetNearPoint(bot, x, y, z, bot->GetCombatReach(), FOLLOW_TANK_DISTANCE,
                Position::NormalizeOrientation(tank->GetOrientation() + float(M_PI)));
            MoveTo(bot, FOLLOW_TANK_MOVE_POINT_ID, x, y, z);
            return;
        }

        if (partyAction < LayoutConstants::PARTY_ACTION_HEAL_FIRST)
        {
            bool const assist = partyAction < LayoutConstants::PARTY_ACTION_GUARD_FIRST;
            Player* teammate = view.Teammates[partyAction
                - (assist ? LayoutConstants::PARTY_ACTION_ASSIST_FIRST : LayoutConstants::PARTY_ACTION_GUARD_FIRST)].Bot;
            int32 const slot = assist ? SlotOf(view, teammate->GetVictim())
                : SlotAttacking(view, teammate, view.TargetSlot);
            if (slot >= 0 && view.Enemies[slot])
                SelectEnemy(view, uint32(slot));
            return;
        }

        uint32 const heals = uint32(view.L->AllyHeals.size());
        uint32 const index = partyAction - LayoutConstants::PARTY_ACTION_HEAL_FIRST;
        Heal(bot, view.L->AllyHeals[index % heals], view.Teammates[index / heals].Bot, result);
    }

    // Stages 7 and 8: PvP.

    void ObservePvp(SeatView const& view, float* obs)
    {
        Player* bot = view.Bot;
        float* pvp = obs + view.L->PvpObsFirst;

        pvp[LayoutConstants::PVP_OBS_BOT_STUNNED] = bot->HasUnitState(STUN_STATES) ? 1.0f : 0.0f;
        pvp[LayoutConstants::PVP_OBS_BOT_ROOTED] = bot->HasUnitState(UNIT_STATE_ROOT) ? 1.0f : 0.0f;
        pvp[LayoutConstants::PVP_OBS_BOT_SILENCED] = bot->HasAuraType(SPELL_AURA_MOD_SILENCE)
            || bot->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE) ? 1.0f : 0.0f;
        pvp[LayoutConstants::PVP_OBS_MIRROR] = view.Mirror ? 1.0f : 0.0f;

        Player* opponent = view.Opponent;
        if (!opponent)
            return;

        for (uint32 i = 0; i < CLASSES.size(); ++i)
            pvp[LayoutConstants::PVP_OBS_OPPONENT_CLASS_FIRST + i] = CLASSES[i] == view.OpponentClass ? 1.0f : 0.0f;
        pvp[LayoutConstants::PVP_OBS_OPPONENT_ROLE_FIRST + uint32(view.OpponentRole)] = 1.0f;

        pvp[LayoutConstants::PVP_OBS_OPPONENT_LEVEL_DIFF] = (float(opponent->GetLevel()) - float(bot->GetLevel())) / 5.0f;
        if (uint32 const maxMana = opponent->GetMaxPower(POWER_MANA))
            pvp[LayoutConstants::PVP_OBS_OPPONENT_MANA] = float(opponent->GetPower(POWER_MANA)) / float(maxMana);

        Powers const power = opponent->getPowerType();
        if (power != POWER_MANA)
            if (uint32 const maxPower = opponent->GetMaxPower(power))
                pvp[LayoutConstants::PVP_OBS_OPPONENT_RAGE_ENERGY] = float(opponent->GetPower(power)) / float(maxPower);

        pvp[LayoutConstants::PVP_OBS_OPPONENT_CONTROLLED] = IsCrowdControlled(opponent) ? 1.0f : 0.0f;
        pvp[LayoutConstants::PVP_OBS_OPPONENT_STEALTHED] = opponent->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;
        pvp[LayoutConstants::PVP_OBS_OPPONENT_PET_OUT] = opponent->GetPetGUID() || !opponent->m_Controlled.empty()
            ? 1.0f : 0.0f;

        if (Spell const* cast = opponent->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (cast->m_spellInfo->HasEffect(SPELL_EFFECT_HEAL) || cast->m_spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL))
                pvp[LayoutConstants::PVP_OBS_OPPONENT_HEALING] = 1.0f;
    }
}

SpellInfo const* Animus::Curriculum::SeatEncoder::TrinketSpell(Item const* item)
{
    if (!item)
        return nullptr;

    for (_Spell const& spellData : item->GetTemplate()->Spells)
        if (spellData.SpellId > 0 && spellData.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            return sSpellMgr->GetSpellInfo(spellData.SpellId);

    return nullptr;
}

void Animus::Curriculum::SeatEncoder::Observe(SeatView const& view, float* obs, uint8* mask)
{
    Layout const& layout = *view.L;
    std::fill(obs, obs + layout.ObsDim, 0.0f);
    std::fill(mask, mask + layout.NumActions, 0);
    mask[0] = 1;

    Player* bot = view.Bot;
    Unit* target = view.Target;
    bool const gauntlet = layout.Has(Stage::Gauntlet);

    obs[OBS_LEVEL] = float(view.Level) / float(DEFAULT_MAX_LEVEL);
    for (uint32 i = 0; i < PLAYABLE_RACES.size(); ++i)
        obs[OBS_RACE_FIRST + i] = PLAYABLE_RACES[i] == view.Race ? 1.0f : 0.0f;
    obs[OBS_SPEC_FIRST + std::min<uint32>(view.Spec, MAX_SPECS - 1)] = 1.0f;

    if (bot && bot->IsAlive() && (target || gauntlet))
    {
        ObjectGuid const botGuid = bot->GetGUID();
        uint8 const level = bot->GetLevel();

        obs[OBS_HEALTH] = bot->GetHealthPct() / 100.0f;
        if (uint32 const maxMana = bot->GetMaxPower(POWER_MANA))
            obs[OBS_MANA] = float(bot->GetPower(POWER_MANA)) / float(maxMana);
        obs[OBS_RAGE] = float(bot->GetPower(POWER_RAGE)) / 1000.0f;
        if (uint32 const maxEnergy = bot->GetMaxPower(POWER_ENERGY))
            obs[OBS_ENERGY] = float(bot->GetPower(POWER_ENERGY)) / float(maxEnergy);
        obs[OBS_RUNIC_POWER] = float(bot->GetPower(POWER_RUNIC_POWER)) / 1000.0f;

        if (bot->getClass() == CLASS_DEATH_KNIGHT)
            for (uint8 rune = 0; rune < MAX_RUNES; ++rune)
                obs[OBS_RUNE_FIRST + rune] = 1.0f
                    - std::min(1.0f, float(bot->GetRuneCooldown(rune)) / RUNE_COOLDOWN_MS);

        if (target)
            obs[OBS_COMBO_POINTS] = float(bot->GetComboPoints(target)) / 5.0f;

        ShapeshiftForm const form = bot->GetShapeshiftForm();
        for (uint32 i = 0; i < TRACKED_FORMS.size(); ++i)
            obs[OBS_FORM_FIRST + i] = TRACKED_FORMS[i] == form ? 1.0f : 0.0f;

        obs[OBS_CASTING] = bot->IsNonMeleeSpellCast(false, false, true) ? 1.0f : 0.0f;
        obs[OBS_QUEUED_NEXT_SWING] = bot->GetCurrentSpell(CURRENT_MELEE_SPELL) ? 1.0f : 0.0f;

        for (auto const& [index, attack] : { std::pair{ OBS_MAIN_HAND_SWING, BASE_ATTACK },
            std::pair{ OBS_OFF_HAND_SWING, OFF_ATTACK }, std::pair{ OBS_RANGED_SWING, RANGED_ATTACK } })
        {
            if (uint32 const attackTime = bot->GetAttackTime(attack))
                obs[index] = std::clamp(float(bot->getAttackTimer(attack)) / float(attackTime), 0.0f, 1.0f);
        }

        obs[OBS_MAIN_HAND_SPEED] = float(bot->GetAttackTime(BASE_ATTACK)) / 4000.0f;
        if (target)
        {
            obs[OBS_TARGET_HEALTH] = target->GetHealthPct() / 100.0f;
            obs[OBS_TARGET_DISTANCE] = std::min(1.0f, bot->GetDistance(target) / 40.0f);
            obs[OBS_IN_MELEE_FRONT] = bot->IsWithinMeleeRange(target) && bot->HasInArc(2 * float(M_PI) / 3, target)
                ? 1.0f : 0.0f;
        }

        obs[OBS_ATTACK_POWER] = bot->GetTotalAttackPowerValue(BASE_ATTACK) / (100.0f + 50.0f * level);
        obs[OBS_SPELL_POWER] = float(bot->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC)) / (50.0f + 30.0f * level);
        obs[OBS_MELEE_CRIT] = bot->GetFloatValue(PLAYER_CRIT_PERCENTAGE) / 100.0f;

        float spellCrit = 0.0f;
        for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
            spellCrit = std::max(spellCrit, bot->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + school));
        obs[OBS_SPELL_CRIT] = spellCrit / 100.0f;

        obs[OBS_MELEE_HASTE] = bot->GetRatingBonusValue(CR_HASTE_MELEE) / 100.0f;
        obs[OBS_SPELL_HASTE] = bot->GetRatingBonusValue(CR_HASTE_SPELL) / 100.0f;
        obs[OBS_MELEE_HIT] = bot->GetRatingBonusValue(CR_HIT_MELEE) / 100.0f;
        obs[OBS_SPELL_HIT] = bot->GetRatingBonusValue(CR_HIT_SPELL) / 100.0f;
        obs[OBS_EXPERTISE] = float(bot->GetUInt32Value(PLAYER_EXPERTISE)) / 30.0f;
        obs[OBS_ARMOR_PENETRATION] = bot->GetRatingBonusValue(CR_ARMOR_PENETRATION) / 100.0f;
        obs[OBS_LAST_STEP_DAMAGE] = view.LastStepDamage;
        obs[OBS_LAST_STEP_POWER_DELTA] = view.LastStepPowerDelta;

        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        for (uint32 action = 0; action < actions.size(); ++action)
        {
            SpellInfo const* info = nullptr;
            if (actions[action].Type == ActionCatalog::Kind::Spell)
                info = ActionCatalog::KnownRank(bot, actions[action].FirstRank);
            else if (actions[action].Type == ActionCatalog::Kind::Trinket)
                info = TrinketSpell(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, actions[action].EquipmentSlot));

            if (info)
            {
                float* features = obs + layout.ActionObsFirst + action * ACTION_FEATURES;
                float stacks = 0.0f;
                features[0] = 1.0f;
                features[1] = CooldownFraction(bot, info);
                features[2] = target ? AuraFraction(target, info->Id, botGuid, stacks) : 0.0f;
                features[3] = AuraFraction(bot, info->Id, botGuid, stacks);
                features[4] = stacks;

                if (!obs[OBS_GCD] && info->StartRecoveryTime)
                    obs[OBS_GCD] = std::min(1.0f, float(bot->GetGlobalCooldownMgr().GetGlobalCooldown(info)) / GCD_MS);
            }

            if (action > 0)
                mask[action] = IsActionAllowed(view, action) ? 1 : 0;
        }

        if (layout.Has(Stage::Duel))
        {
            ObserveDuel(view, obs);
            for (uint32 duelAction = 0; duelAction < layout.DuelActionCount; ++duelAction)
                mask[layout.DuelActionFirst + duelAction] = IsDuelActionAllowed(view, duelAction) ? 1 : 0;
        }

        if (layout.Has(Stage::Pack))
        {
            ObservePack(view, obs);
            std::vector<ActionCatalog::Action> const& tactical = layout.Catalog().Tactical();
            for (uint32 packAction = 0; packAction < layout.PackActionCount; ++packAction)
                mask[layout.PackActionFirst + packAction] = packAction < PACK_SLOTS
                    ? IsPackActionAllowed(view, packAction)
                    : target && IsSpellActionAllowed(view, target, tactical[packAction - PACK_SLOTS]);
        }

        if (gauntlet)
        {
            ObserveGauntlet(view, obs);
            for (uint32 gauntletAction = 0; gauntletAction < layout.GauntletActionCount; ++gauntletAction)
                mask[layout.GauntletActionFirst + gauntletAction] = IsGauntletActionAllowed(view, gauntletAction) ? 1 : 0;
        }

        if (layout.Has(Stage::Companion))
        {
            ObserveCompanion(view, obs);
            for (uint32 companionAction = 0; companionAction < layout.CompanionActionCount; ++companionAction)
                mask[layout.CompanionActionFirst + companionAction] =
                    IsCompanionActionAllowed(view, companionAction) ? 1 : 0;
        }

        if (layout.Has(Stage::Party))
        {
            ObserveParty(view, obs);
            for (uint32 partyAction = 0; partyAction < layout.PartyActionCount; ++partyAction)
                mask[layout.PartyActionFirst + partyAction] = IsPartyActionAllowed(view, partyAction) ? 1 : 0;
        }

        if (layout.Has(Stage::Pvp))
            ObservePvp(view, obs);
    }

    if (view.Build)
    {
        std::vector<TalentBuilder::Talent> const& talents = layout.Assets->Talents->Talents();
        for (uint32 i = 0; i < talents.size() && i < view.Build->Ranks.size(); ++i)
            obs[layout.TalentObsFirst + i] = float(view.Build->Ranks[i]) / float(std::max<uint8>(1, talents[i].MaxRank));

        for (uint32 tree = 0; tree < TalentBuilder::TREE_COUNT; ++tree)
            obs[layout.TreeObsFirst + tree] = float(view.Build->TreePoints[tree]) / TALENT_POINTS_AT_MAX_LEVEL;
    }
}

void Animus::Curriculum::SeatEncoder::Apply(SeatView& view, int32 action, SeatActionResult& result)
{
    Player* bot = view.Bot;
    if (!bot || !view.L)
        return;

    Layout const& layout = *view.L;
    Unit* target = view.Target;

    // Only the gauntlet has moments without a target (between pulls).
    if (!target && !layout.Has(Stage::Gauntlet))
        return;

    if (layout.Has(Stage::Duel))
    {
        // Face the target whenever not running somewhere: casts and swings need it, and turning is not a decision
        // worth learning.
        if (target && bot->IsAlive() && bot->movespline->Finalized() && !bot->HasInArc(float(M_PI) / 2, target))
            bot->SetFacingToObject(target);

        if (layout.Has(Stage::Party) && action >= int32(layout.PartyActionFirst))
        {
            ApplyPartyAction(view, uint32(action) - layout.PartyActionFirst, result);
            return;
        }

        if (layout.Has(Stage::Companion) && action >= int32(layout.CompanionActionFirst)
            && action < int32(layout.CompanionActionFirst + layout.CompanionActionCount))
        {
            ApplyCompanionAction(view, uint32(action) - layout.CompanionActionFirst, result);
            return;
        }

        if (layout.Has(Stage::Gauntlet) && action >= int32(layout.GauntletActionFirst)
            && action < int32(layout.GauntletActionFirst + layout.GauntletActionCount))
        {
            ApplyGauntletAction(view, uint32(action) - layout.GauntletActionFirst, result);
            return;
        }

        if (layout.Has(Stage::Pack) && action >= int32(layout.PackActionFirst)
            && action < int32(layout.PackActionFirst + layout.PackActionCount))
        {
            ApplyPackAction(view, uint32(action) - layout.PackActionFirst, result);
            return;
        }

        if (action >= int32(layout.DuelActionFirst) && action < int32(layout.DuelActionFirst + layout.DuelActionCount))
        {
            // Stopping a cast and leaving a form need no target; ApplyDuelAction checks the rest.
            ApplyDuelAction(view, uint32(action) - layout.DuelActionFirst, result);
            return;
        }
    }

    std::vector<ActionCatalog::Action> const& catalog = layout.Catalog().Actions();
    if (action <= 0 || action >= int32(catalog.size()))
        return;

    ActionCatalog::Action const& def = catalog[action];
    switch (def.Type)
    {
        case ActionCatalog::Kind::Noop:
            return;
        case ActionCatalog::Kind::CancelQueued:
            if (bot->GetCurrentSpell(CURRENT_MELEE_SPELL))
                bot->InterruptSpell(CURRENT_MELEE_SPELL);
            return;
        case ActionCatalog::Kind::Trinket:
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, def.EquipmentSlot);
            SpellInfo const* info = TrinketSpell(item);
            if (!info || bot->HasSpellCooldown(info->Id))
                return;

            bot->CastItemUseSpell(item, TargetsFor(info, bot, target), 1, 0);
            if (bot->HasSpellCooldown(info->Id))
                ++result.TrinketUses;
            return;
        }
        case ActionCatalog::Kind::Spell:
            break;
    }

    ApplySpellAction(view, target, def, result);
}

void Animus::Curriculum::SeatEncoder::StartCallBeastCooldown(Player* bot)
{
    if (SpellInfo const* callPet = sSpellMgr->GetSpellInfo(SPELL_CALL_PET))
        bot->GetGlobalCooldownMgr().AddGlobalCooldown(callPet, CALL_BEAST_GCD_MS);
}

bool Animus::Curriculum::SeatEncoder::PetAttack(Player* bot, Unit* target)
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

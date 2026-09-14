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

#include "WarriorDummyPolicyIO.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <algorithm>

namespace
{
    enum WarriorDummySpells : uint32
    {
        SPELL_HEROIC_STRIKE_RANK_1  = 78,
    };

    bool IsHeroicStrikeQueued(Player const* bot)
    {
        Spell const* spell = bot->GetCurrentSpell(CURRENT_MELEE_SPELL);
        return spell && spell->m_spellInfo->Id == SPELL_HEROIC_STRIKE_RANK_1;
    }

    uint32 HeroicStrikeCost(Player* bot)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_HEROIC_STRIKE_RANK_1);
        return info ? uint32(std::max(0, info->CalcPowerCost(bot, info->GetSchoolMask()))) : 0;
    }

    bool CanQueueHeroicStrike(Player* bot)
    {
        return bot->HasActiveSpell(SPELL_HEROIC_STRIKE_RANK_1) && !bot->GetCurrentSpell(CURRENT_MELEE_SPELL)
            && bot->GetPower(POWER_RAGE) >= HeroicStrikeCost(bot);
    }
}

float Animus::WarriorDummy::DamageScale(Player const* bot)
{
    return std::max(1.0f, bot->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE));
}

void Animus::WarriorDummy::Observe(Player* bot, Unit const* target, StepFeedback const& feedback, float* obs,
    uint8* mask)
{
    std::fill(obs, obs + OBS_COUNT, 0.0f);
    std::fill(mask, mask + ACTION_COUNT, 0);
    mask[ACTION_NOOP] = 1;

    if (!bot || !target)
        return;

    float const maxRage = float(std::max<uint32>(1, bot->GetMaxPower(POWER_RAGE)));
    float const attackTime = float(std::max<uint32>(1, bot->GetAttackTime(BASE_ATTACK)));
    bool const queued = IsHeroicStrikeQueued(bot);

    obs[OBS_RAGE] = float(bot->GetPower(POWER_RAGE)) / maxRage;
    obs[OBS_SWING_REMAINING] = float(std::max(0, bot->getAttackTimer(BASE_ATTACK))) / attackTime;
    obs[OBS_WEAPON_SPEED] = attackTime / 4000.0f;
    obs[OBS_HEROIC_STRIKE_QUEUED] = queued ? 1.0f : 0.0f;
    obs[OBS_AUTO_ATTACKING] = bot->GetVictim() == target ? 1.0f : 0.0f;
    obs[OBS_IN_MELEE_FRONT] = bot->IsWithinMeleeRange(target) && bot->HasInArc(2 * float(M_PI) / 3, target)
        ? 1.0f : 0.0f;
    obs[OBS_HEROIC_STRIKE_COST] = float(HeroicStrikeCost(bot)) / maxRage;
    obs[OBS_LAST_STEP_DAMAGE] = feedback.LastStepDamage;
    obs[OBS_LAST_STEP_RAGE_DELTA] = feedback.LastStepRageDelta;

    mask[ACTION_QUEUE_HEROIC_STRIKE] = CanQueueHeroicStrike(bot) ? 1 : 0;
    mask[ACTION_CANCEL_QUEUED] = queued ? 1 : 0;
}

void Animus::WarriorDummy::Apply(Player* bot, Unit* target, int32 action)
{
    switch (action)
    {
        case ACTION_QUEUE_HEROIC_STRIKE:
        {
            if (!CanQueueHeroicStrike(bot))
                break;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_HEROIC_STRIKE_RANK_1);
            if (!info)
                break;

            // Same path as CMSG_CAST_SPELL: an untriggered cast that parks in CURRENT_MELEE_SPELL
            // and replaces the next main-hand swing. The spell owns and frees itself.
            SpellCastTargets targets;
            targets.SetUnitTarget(target);

            Spell* spell = new Spell(bot, info, TRIGGERED_NONE);
            spell->prepare(&targets);
            break;
        }
        case ACTION_CANCEL_QUEUED:
            if (IsHeroicStrikeQueued(bot))
                bot->InterruptSpell(CURRENT_MELEE_SPELL);
            break;
        default:
            break;
    }
}

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

#include "AllCreatureScript.h"
#include "AllSpellScript.h"
#include "EnvPool.h"
#include "PoolRegistry.h"
#include "SummonLevel.h"
#include "Unit.h"
#include "UnitScript.h"

/*
 * The hooks every env pool needs, whichever module runs it: combat totals for rewards and episode info, and the level
 * of creatures the encounters summon.
 */
namespace
{
    /// The spell whose damage this thread is about to deal. The core's DealDamage hook does not say which spell dealt
    /// a hit, but spell damage runs ModifySpellDamageTaken (direct) or ModifyPeriodicDamageAurasTick (periodic) for
    /// the same attacker and victim just before it, with nothing in between that deals damage from that attacker to
    /// that victim. Only compared by address, never dereferenced as a unit.
    struct PendingSpellDamage
    {
        Unit const* Attacker = nullptr;
        Unit const* Victim = nullptr;
        SpellInfo const* Spell = nullptr;
    };

    thread_local PendingSpellDamage Pending;

    class AnimusLibUnitScript : public UnitScript
    {
    public:
        AnimusLibUnitScript() : UnitScript("AnimusLibUnitScript") { }

        void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& /*damage*/,
            SpellInfo const* spellInfo) override
        {
            Pending = { attacker, target, spellInfo };
        }

        void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& /*damage*/,
            SpellInfo const* spellInfo) override
        {
            Pending = { attacker, target, spellInfo };
        }

        /// Called for every damage event, on map threads, before the victim's AI can change the
        /// amount (a creature script may rewrite it in DamageTaken), so it counts what was dealt.
        uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType type) override
        {
            SpellInfo const* spell = nullptr;
            if (type != DIRECT_DAMAGE && Pending.Attacker == attacker && Pending.Victim == victim)
            {
                spell = Pending.Spell;
                Pending = {};
            }

            for (Animus::EnvPool* pool : Animus::PoolRegistry::Pools())
                pool->RecordDamage(attacker, victim, damage, type, spell);

            return damage;
        }

        /// Called for every heal, on map threads, with the health actually gained (overhealing excluded).
        void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override
        {
            for (Animus::EnvPool* pool : Animus::PoolRegistry::Pools())
                pool->RecordHeal(healer, receiver, gain);
        }
    };

    class AnimusLibSpellScript : public AllSpellScript
    {
    public:
        AnimusLibSpellScript() : AllSpellScript("AnimusLibSpellScript",
            { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL }) { }

        /// Called on map threads once a spell's cast time is over and it goes off.
        void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*skipCheck*/) override
        {
            for (Animus::EnvPool* pool : Animus::PoolRegistry::Pools())
                pool->RecordCastCompleted(caster, spell);
        }

        /// Called on map threads when a cast or channel is cancelled, with the spell still in its old state.
        void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool bySelf) override
        {
            for (Animus::EnvPool* pool : Animus::PoolRegistry::Pools())
                pool->RecordCastCancelled(caster, spell, bySelf);
        }
    };

    class AnimusLibCreatureScript : public AllCreatureScript
    {
    public:
        AnimusLibCreatureScript() : AllCreatureScript("AnimusLibCreatureScript") { }

        void OnBeforeCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* /*creature*/,
            uint8& level) override
        {
            if (Animus::PendingSummonLevel)
                level = Animus::PendingSummonLevel;
        }
    };
}

void AddSC_animus_lib()
{
    new AnimusLibUnitScript();
    new AnimusLibSpellScript();
    new AnimusLibCreatureScript();
}

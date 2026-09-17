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

#ifndef ANIMUS_LIB_CURRICULUM_ACTION_CATALOG_H
#define ANIMUS_LIB_CURRICULUM_ACTION_CATALOG_H

#include "Define.h"
#include <string>
#include <vector>

class Player;
class SpellInfo;

namespace Animus::Curriculum
{
    class ClassKit;
    class TalentBuilder;

    /// The fixed action space of one class: every spell of its own kit any of its races can know by level 80
    /// (trainer spells, starting spells, racials, active talents, and the spells those teach) -- combat spells, then
    /// the use effects of its trinkets, weapon and off-hand item, then its
    /// tactical spells (crowd control, interrupts) and its sustain spells (heals, shields) cast on itself.
    ///
    /// A spell action stands for a whole rank chain and casts the highest rank the bot knows. Actions
    /// the current bot cannot use (other race, level too low, talent not taken, on cooldown, wrong
    /// stance) are masked every decision.
    class ActionCatalog
    {
    public:
        enum class Kind : uint8
        {
            Noop,
            CancelQueued,       // cancel a queued on-next-swing ability
            Spell,
            Trinket,            // the use effect of the item in EquipmentSlot (trinkets, main hand, off hand)
            Soulstone,          // warlocks: use the soulstone in the bags on a friendly player
        };

        /// Which of the kit's lists a spell action came from.
        enum class Group : uint8
        {
            None,               // not a spell, or a revive
            Combat,
            Tactical,
            Sustain,
        };

        struct Action
        {
            Kind Type = Kind::Noop;
            std::string Name;
            uint32 FirstRank = 0;       // Spell: first spell of the rank chain
            uint8 EquipmentSlot = 0;    // Trinket: EQUIPMENT_SLOT_TRINKET1/2
            bool NextSwing = false;
            /// Its own position in the list it belongs to, so a per-seat table can be keyed by action. Filled
            /// after the lists are built; the brace initialisers above it are positional.
            uint32 Index = 0;
            Group From = Group::None;
        };

        ActionCatalog(uint8 playerClass, ClassKit const& kit, TalentBuilder const& talents);

        /// The core block's actions: no-op, cancel-queued, combat spells, equipped item uses, tactical spells, sustain
        /// spells.
        /// A player fights with the whole kit, from the first stage on: a healer heals itself and a mage polymorphs.
        [[nodiscard]] std::vector<Action> const& Actions() const { return _actions; }

        /// Interrupts, stuns, silences, fears, roots, polymorphs, knockbacks, taunts and offensive dispels
        /// that are not already combat actions (also in Actions()).
        [[nodiscard]] std::vector<Action> const& Tactical() const { return _tactical; }

        /// Heals, heal-over-time, absorbs and friendly dispels that are not already combat or tactical
        /// actions (also in Actions(), cast on the bot itself; companion and party blocks cast them on allies).
        [[nodiscard]] std::vector<Action> const& Sustain() const { return _sustain; }

        /// Resurrection spells that take a dead friendly player as their target (Resurrection, Redemption, Ancestral
        /// Spirit, Revive, Rebirth), then, for warlocks, the soulstone. Used from the companion stage on.
        [[nodiscard]] std::vector<Action> const& Revives() const { return _revives; }

        /// Highest rank of a spell action the bot knows, or nullptr.
        [[nodiscard]] static SpellInfo const* KnownRank(Player const* bot, uint32 firstRank);

        /// Whether a spell is worth an action slot for damage on a target.
        [[nodiscard]] static bool IsCombatSpell(SpellInfo const* info);
        [[nodiscard]] static bool IsTacticalSpell(SpellInfo const* info);
        [[nodiscard]] static bool IsSustainSpell(SpellInfo const* info);

        /// Whether casting the spell on a casting target stops the cast (interrupt, stun, silence, ...).
        [[nodiscard]] static bool IsInterruptingSpell(SpellInfo const* info);

    private:
        std::vector<Action> _actions;
        std::vector<Action> _tactical;
        std::vector<Action> _sustain;
        std::vector<Action> _revives;
    };
}

#endif

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

#include "Baselines.h"
#include "CoreBlock.h"
#include "DuelBlock.h"
#include "GauntletBlock.h"
#include "PetBlock.h"
#include "SupportBlock.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TravelBlock.h"
#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace
{
    using namespace Animus::Curriculum;

    constexpr float EAT_BELOW = 0.8f;
    constexpr float DRINK_BELOW = 0.8f;
    constexpr float HEAL_BELOW = 0.6f;          // the most hurt living friend below this is healed
    constexpr float DEFENSIVE_BELOW = 0.3f;     // the bot below this uses a defensive
    constexpr float CLOSE_IN_BEYOND_YARDS = 4.0f;
    constexpr float HOLD_RANGE_BEYOND_YARDS = 28.0f;    // ranged specs close to DuelBlock::MOVE_TO_RANGE_DISTANCE
    constexpr float MOUNT_BEYOND_YARDS = 80.0f;
    constexpr float CRUISE_HEIGHT_YARDS = 20.0f;

    /// A seat's row, read by block: features and actions by their block-relative index.
    class Row
    {
    public:
        Row(Layout const& layout, float const* obs, uint8 const* mask) : _layout(layout), _obs(obs), _mask(mask) { }

        [[nodiscard]] bool Has(BlockId block) const { return _layout.Has(block); }

        [[nodiscard]] float Obs(BlockId block, uint32 feature) const
        {
            return _obs[_layout.Slice(block).ObsFirst + feature];
        }

        /// The row's action for `action` of `block`, if the layout has it and it is allowed.
        [[nodiscard]] std::optional<int32> Allowed(BlockId block, uint32 action) const
        {
            BlockSlice const& slice = _layout.Slice(block);
            if (!Has(block) || action >= slice.ActionCount || !_mask[slice.ActionFirst + action])
                return std::nullopt;
            return int32(slice.ActionFirst + action);
        }

    private:
        Layout const& _layout;
        float const* _obs;
        uint8 const* _mask;
    };

    /// The summons a pet class's baseline casts when its pet is not out, best first: a warlock's demons from the one
    /// that soaks a fight best, a death knight's ghoul, a frost mage's elemental.
    constexpr std::array<uint32, 7> PET_SUMMONS =
    {
        30146,  // Summon Felguard
        697,    // Summon Voidwalker
        691,    // Summon Felhunter
        712,    // Summon Succubus
        688,    // Summon Imp
        46584,  // Raise Dead
        31687,  // Summon Water Elemental
    };

    /// A pet class's pet: call a hunter's first stable beast or cast the best summon when no living pet is out (out
    /// of combat only: a demon takes seconds to summon), then send the pet at the target.
    std::optional<int32> PetAction(Row const& row, Layout const& layout)
    {
        if (!row.Has(BlockId::Pet) || !PetBlock::HasPet(layout.Profile->Class))
            return std::nullopt;

        bool const petOut = row.Obs(BlockId::Pet, PetBlock::OBS_PRESENT) > 0.0f
            && row.Obs(BlockId::Pet, PetBlock::OBS_ALIVE) > 0.0f;
        bool const inCombat = row.Obs(BlockId::Duel, DuelBlock::OBS_BOT_IN_COMBAT) > 0.0f;

        if (!petOut && !inCombat)
        {
            for (uint32 slot = 0; slot < STABLE_SLOTS; ++slot)
                if (std::optional<int32> call = row.Allowed(BlockId::Duel, DuelBlock::ACTION_CALL_BEAST_FIRST + slot))
                    return call;

            std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
            for (uint32 summon : PET_SUMMONS)
                for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
                    if (actions[action].Type == ActionCatalog::Kind::Spell && actions[action].FirstRank == summon)
                        if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                            return cast;
        }

        if (petOut)
            if (std::optional<int32> send = row.Allowed(BlockId::Duel, DuelBlock::ACTION_PET_ATTACK))
                return send;

        return std::nullopt;
    }

    /// While the pet attacks the target: its first allowed damaging ability, as autocast would. Pets no longer
    /// autocast, and an Imp or a Water Elemental cannot melee (PetAI::_canMeleeAttack), so without this they do
    /// nothing.
    std::optional<int32> PetDamage(Row const& row, Layout const& layout)
    {
        if (!row.Has(BlockId::Pet) || !PetBlock::HasPet(layout.Profile->Class)
            || row.Obs(BlockId::Pet, PetBlock::OBS_ATTACKING_TARGET) == 0.0f)
            return std::nullopt;

        for (uint32 slot = 0; slot < PetBlock::ABILITY_SLOTS; ++slot)
        {
            uint32 const first = PetBlock::OBS_SLOT_FIRST + slot * PetBlock::SLOT_FEATURES;
            if (row.Obs(BlockId::Pet, first + PetBlock::SLOT_DAMAGE) > 0.0f
                && row.Obs(BlockId::Pet, first + PetBlock::SLOT_POSITIVE) == 0.0f)
                if (std::optional<int32> cast = row.Allowed(BlockId::Pet, PetBlock::ACTION_ABILITY_FIRST + slot))
                    return cast;
        }

        return std::nullopt;
    }

    /// The seat's spec, read from the core block's spec one-hot; null if none is set.
    SpecProfile const* SpecOf(Row const& row, Layout const& layout)
    {
        std::vector<SpecProfile> const& specs = layout.Profile->Specs;
        for (uint32 spec = 0; spec < specs.size() && spec < CoreBlock::MAX_SPECS; ++spec)
            if (row.Obs(BlockId::Core, CoreBlock::OBS_SPEC_FIRST + spec) > 0.0f)
                return &specs[spec];

        return nullptr;
    }

    /// Whether the seat's spec fights from range (hunters, casters, healers).
    bool FightsFromRange(Row const& row, Layout const& layout)
    {
        SpecProfile const* spec = SpecOf(row, layout);
        return spec && spec->Range != RangeBand::Melee;
    }

    /// What a catalog spell is to the `fight` rotation.
    enum class SpellUse : uint8
    {
        Other,
        Damage,             // hits the target now: school or weapon damage, a leech, a melee or ranged weapon attack
        DamageOverTime,     // only ticks: worth casting while it isn't on the target
        Buff,               // an aura on the bot or its party with no cooldown of its own
        Form,               // a shapeshift (stances, forms, Shadowform): only the spec's own, see SpecForms
    };

    SpellUse UseOf(SpellInfo const* info)
    {
        if (!info)
            return SpellUse::Other;

        if (info->HasAura(SPELL_AURA_MOD_SHAPESHIFT))
            return SpellUse::Form;

        if (!info->IsPositive())
        {
            // Crowd control that damage breaks (Polymorph, Scatter Shot, Fear) would undo the fight's own hits.
            if (info->HasAura(SPELL_AURA_MOD_CONFUSE) || info->HasAura(SPELL_AURA_MOD_FEAR)
                || info->HasAura(SPELL_AURA_TRANSFORM))
                return SpellUse::Other;

            bool direct = info->DmgClass == SPELL_DAMAGE_CLASS_MELEE || info->DmgClass == SPELL_DAMAGE_CLASS_RANGED;
            bool periodic = false;
            for (SpellEffectInfo const& effect : info->GetEffects())
            {
                switch (effect.Effect)
                {
                    case SPELL_EFFECT_SCHOOL_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE:
                    case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                    case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                    case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                    case SPELL_EFFECT_HEALTH_LEECH:
                        direct = true;
                        break;
                    default:
                        break;
                }

                switch (effect.ApplyAuraName)
                {
                    case SPELL_AURA_PERIODIC_DAMAGE:
                    case SPELL_AURA_PERIODIC_LEECH:
                    case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                    case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
                        periodic = true;
                        break;
                    default:
                        break;
                }
            }

            return direct ? SpellUse::Damage : periodic ? SpellUse::DamageOverTime : SpellUse::Other;
        }

        // A buff to keep up, cast once: not a cooldown (a defensive saved for need), not speed (an aspect that dazes
        // when hit), not stealth or invisibility (which a fight breaks), not feigning death.
        bool const aura = info->HasEffect(SPELL_EFFECT_APPLY_AURA)
            || info->HasEffect(SPELL_EFFECT_APPLY_AREA_AURA_PARTY)
            || info->HasEffect(SPELL_EFFECT_APPLY_AREA_AURA_RAID);
        if (!aura || info->RecoveryTime || info->CategoryRecoveryTime || info->HasAura(SPELL_AURA_MOD_INCREASE_SPEED)
            || info->HasAura(SPELL_AURA_MOD_STEALTH) || info->HasAura(SPELL_AURA_MOD_INVISIBILITY)
            || info->HasAura(SPELL_AURA_FEIGN_DEATH))
            return SpellUse::Other;

        return SpellUse::Buff;
    }

    /// The form a spec fights in, best first: the rotation shifts into the first one it knows while in no form. A spec
    /// not listed fights in no form (warriors are put in their stance by SeatCharacter::PrepareFighter).
    struct SpecForm
    {
        uint8 Class;
        std::string_view Spec;
        std::array<uint32, 2> Forms;
    };

    constexpr std::array<SpecForm, 4> SPEC_FORMS =
    {{
        { CLASS_DRUID, "balance", { 24858, 0 } },           // Moonkin Form
        { CLASS_DRUID, "feral_cat", { 768, 0 } },           // Cat Form
        { CLASS_DRUID, "feral_bear", { 9634, 5487 } },      // Dire Bear Form, Bear Form
        { CLASS_PRIEST, "shadow", { 15473, 0 } },           // Shadowform
    }};

    /// The core block's feature `feature` of catalog action `action` (known, cooldown, aura on target, aura on self).
    float ActionFeature(Row const& row, uint32 action, uint32 feature)
    {
        return row.Obs(BlockId::Core, CoreBlock::OBS_GLOBAL_COUNT + action * CoreBlock::ACTION_FEATURES + feature);
    }

    constexpr uint32 ACTION_AURA_ON_TARGET = 2;
    constexpr uint32 ACTION_AURA_ON_SELF = 3;

    /// `fight`'s spells, first match wins: the spec's form while in no form; the first allowed damaging spell (one that
    /// only ticks while it isn't on the target); out of combat, a buff not already on the bot, one per exclusive kind
    /// (a seal, an aura, an armor); otherwise nothing. Casting for its own sake resets the swing timer, and the first
    /// spell in catalog order is often a buff that can be cast again forever.
    std::optional<int32> Rotation(Row const& row, Layout const& layout)
    {
        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        auto const castable = [&actions](uint32 action)
        {
            return actions[action].Type == ActionCatalog::Kind::Spell;
        };

        // In no form (the tracked forms' one-hot starts with FORM_NONE): the spec's own.
        SpecProfile const* spec = SpecOf(row, layout);
        if (spec && row.Obs(BlockId::Core, CoreBlock::OBS_FORM_FIRST) > 0.0f)
        {
            for (SpecForm const& entry : SPEC_FORMS)
            {
                if (entry.Class != layout.Profile->Class || entry.Spec != spec->Name)
                    continue;

                for (uint32 form : entry.Forms)
                    for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
                        if (form && castable(action) && actions[action].FirstRank == form)
                            if (std::optional<int32> shift = row.Allowed(BlockId::Core, action))
                                return shift;
            }
        }

        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
        {
            if (!castable(action))
                continue;

            SpellUse const use = UseOf(sSpellMgr->GetSpellInfo(actions[action].FirstRank));
            if (use == SpellUse::Damage
                || (use == SpellUse::DamageOverTime && ActionFeature(row, action, ACTION_AURA_ON_TARGET) == 0.0f))
                if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                    return cast;
        }

        if (row.Obs(BlockId::Duel, DuelBlock::OBS_BOT_IN_COMBAT) > 0.0f)
            return std::nullopt;

        // Exclusive kinds already on the bot: a second seal would only replace the first, and back again.
        std::vector<SpellSpecificType> active;
        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
            if (castable(action) && ActionFeature(row, action, ACTION_AURA_ON_SELF) > 0.0f)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(actions[action].FirstRank))
                    if (info->GetSpellSpecific() != SPELL_SPECIFIC_NORMAL)
                        active.push_back(info->GetSpellSpecific());

        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
        {
            if (!castable(action) || ActionFeature(row, action, ACTION_AURA_ON_SELF) > 0.0f)
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(actions[action].FirstRank);
            if (UseOf(info) != SpellUse::Buff || (info->GetSpellSpecific() != SPELL_SPECIFIC_NORMAL
                && std::find(active.begin(), active.end(), info->GetSpellSpecific()) != active.end()))
                continue;

            if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                return cast;
        }

        return std::nullopt;
    }

    /// The first allowed core spell action that `wanted` picks.
    std::optional<int32> FirstSpell(Row const& row, Layout const& layout,
        bool (*wanted)(ActionCatalog::Action const& action))
    {
        std::vector<ActionCatalog::Action> const& actions = layout.Catalog().Actions();
        for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < actions.size(); ++action)
            if (actions[action].Type == ActionCatalog::Kind::Spell && wanted(actions[action]))
                if (std::optional<int32> cast = row.Allowed(BlockId::Core, action))
                    return cast;

        return std::nullopt;
    }

    /// `fight`'s support, as a simple healer plays: a defensive when the bot is low; the most hurt living friend below
    /// HEAL_BELOW selected and healed (the bot itself without the support block); a healer keeps its own heal over
    /// time or shield on the owner, or a tank teammate, once they are in the fight. The masks keep it from healing a
    /// friend at full health or re-casting what is still up.
    std::optional<int32> Support(Row const& row, Layout const& layout)
    {
        float const health = row.Obs(BlockId::Core, CoreBlock::OBS_HEALTH);
        if (health > 0.0f && health < DEFENSIVE_BELOW)
            if (std::optional<int32> defend = FirstSpell(row, layout,
                [](ActionCatalog::Action const& action) { return action.Defensive; }))
                return defend;

        auto const heal = [](ActionCatalog::Action const& action) { return action.Healing; };
        if (!row.Has(BlockId::Support))
            return health > 0.0f && health < HEAL_BELOW ? FirstSpell(row, layout, heal) : std::nullopt;

        auto const friendObs = [&row](uint32 slot, uint32 feature)
        {
            return row.Obs(BlockId::Support, SupportBlock::OBS_GLOBAL_COUNT + slot * SupportBlock::FRIEND_FEATURES
                + feature);
        };
        auto const aim = [&row](uint32 slot) -> std::optional<int32>
        {
            if (row.Obs(BlockId::Support, SupportBlock::OBS_SELECTED_FIRST + slot) > 0.0f)
                return std::nullopt;
            return row.Allowed(BlockId::Support, SupportBlock::ACTION_SELECT_FRIEND_FIRST + slot);
        };

        uint32 lowest = FRIEND_SLOTS;
        float lowestHealth = HEAL_BELOW;
        for (uint32 slot = 0; slot < FRIEND_SLOTS; ++slot)
        {
            if (friendObs(slot, SupportBlock::FRIEND_ALIVE) == 0.0f)
                continue;

            float const friendHealth = friendObs(slot, SupportBlock::FRIEND_HEALTH);
            if (friendHealth < lowestHealth)
            {
                lowest = slot;
                lowestHealth = friendHealth;
            }
        }

        if (lowest < FRIEND_SLOTS)
        {
            if (std::optional<int32> select = aim(lowest))
                return select;
            if (std::optional<int32> cast = FirstSpell(row, layout, heal))
                return cast;

            // Heals cannot be cast in most forms.
            if (layout.PlayRole() == Role::Heal)
                if (std::optional<int32> cancel = row.Allowed(BlockId::Duel, DuelBlock::ACTION_CANCEL_FORM))
                    return cancel;
        }

        if (layout.PlayRole() != Role::Heal)
            return std::nullopt;

        for (uint32 slot = FRIEND_OWNER; slot < FRIEND_SLOTS; ++slot)
        {
            bool const tank = friendObs(slot, SupportBlock::FRIEND_ROLE_FIRST + uint32(Role::Tank)) > 0.0f;
            if (friendObs(slot, SupportBlock::FRIEND_ALIVE) == 0.0f
                || friendObs(slot, SupportBlock::FRIEND_ATTACKERS) == 0.0f || (slot != FRIEND_OWNER && !tank)
                || friendObs(slot, SupportBlock::FRIEND_OWN_HEAL_OVER_TIME) > 0.0f
                || friendObs(slot, SupportBlock::FRIEND_OWN_ABSORB) > 0.0f)
                continue;

            if (std::optional<int32> select = aim(slot))
                return select;
            return FirstSpell(row, layout, [](ActionCatalog::Action const& action)
            {
                return action.Healing && action.KeepsAura;
            });
        }

        return std::nullopt;
    }

    std::optional<int32> Fight(Row const& row, Layout const& layout)
    {
        // A living target's health; not the distance, which is 0 in melee range (it is measured between reaches).
        bool const hasTarget = row.Obs(BlockId::Core, CoreBlock::OBS_TARGET_HEALTH) > 0.0f;

        // Travel: a flying mount for a long trip where it flies, else a ground mount; fly at a safe height, land at
        // the objective and dismount there.
        if (row.Has(BlockId::Travel) && row.Obs(BlockId::Travel, TravelBlock::OBS_OBJECTIVE) > 0.0f)
        {
            float const yards = row.Obs(BlockId::Travel, TravelBlock::OBS_OBJECTIVE_DISTANCE) * 500.0f;
            float const height = row.Obs(BlockId::Travel, TravelBlock::OBS_HEIGHT) * 50.0f;
            bool const mounted = row.Obs(BlockId::Travel, TravelBlock::OBS_MOUNTED) > 0.0f;
            bool const flying = row.Obs(BlockId::Travel, TravelBlock::OBS_FLYING_MOUNT) > 0.0f;
            bool const moving = row.Obs(BlockId::Travel, TravelBlock::OBS_MOVING) > 0.0f;

            if (row.Obs(BlockId::Travel, TravelBlock::OBS_AT_OBJECTIVE) > 0.0f)
                return row.Allowed(BlockId::Travel, TravelBlock::ACTION_DISMOUNT);

            if (!mounted && yards > MOUNT_BEYOND_YARDS)
            {
                if (std::optional<int32> fly = row.Allowed(BlockId::Travel, TravelBlock::ACTION_MOUNT_FLYING))
                    return fly;
                if (std::optional<int32> ride = row.Allowed(BlockId::Travel, TravelBlock::ACTION_MOUNT_GROUND))
                    return ride;
            }

            if (flying && yards > MOUNT_BEYOND_YARDS * 0.5f && height < CRUISE_HEIGHT_YARDS && !moving)
                if (std::optional<int32> climb = row.Allowed(BlockId::Travel, TravelBlock::ACTION_ASCEND))
                    return climb;

            if (flying && yards < TravelBlock::ARRIVE_DISTANCE && height > 1.0f && !moving)
                if (std::optional<int32> land = row.Allowed(BlockId::Travel, TravelBlock::ACTION_DESCEND))
                    return land;

            if (!moving)
                if (std::optional<int32> go = row.Allowed(BlockId::Travel, TravelBlock::ACTION_MOVE_TO_OBJECTIVE))
                    return go;

            // On the way: wait (the no-op), rather than cast something that would take the mount away.
            return 0;
        }

        if (row.Has(BlockId::Gauntlet) && !hasTarget)
        {
            if (row.Obs(BlockId::Core, CoreBlock::OBS_HEALTH) < EAT_BELOW)
                if (std::optional<int32> eat = row.Allowed(BlockId::Gauntlet, GauntletBlock::ACTION_EAT))
                    return eat;

            float const mana = row.Obs(BlockId::Core, CoreBlock::OBS_MANA);
            if (mana > 0.0f && mana < DRINK_BELOW)
                if (std::optional<int32> drink = row.Allowed(BlockId::Gauntlet, GauntletBlock::ACTION_DRINK))
                    return drink;
        }

        if (std::optional<int32> support = Support(row, layout))
            return support;

        // A pet class fights with its pet, as a player does: out before the fight, and sent at the target.
        if (std::optional<int32> pet = PetAction(row, layout))
            return pet;

        float const yards = row.Obs(BlockId::Duel, DuelBlock::OBS_DISTANCE) * 60.0f;
        bool const moving = row.Obs(BlockId::Duel, DuelBlock::OBS_BOT_MOVING) > 0.0f;
        bool const inMelee = yards <= CLOSE_IN_BEYOND_YARDS;

        if (FightsFromRange(row, layout))
        {
            if (hasTarget && !moving)
            {
                // To casting range from afar, and closer only when something is in the way.
                if (yards > HOLD_RANGE_BEYOND_YARDS)
                    if (std::optional<int32> move = row.Allowed(BlockId::Duel, DuelBlock::ACTION_MOVE_TO_RANGE))
                        return move;

                if (row.Obs(BlockId::Duel, DuelBlock::OBS_TARGET_IN_LINE_OF_SIGHT) == 0.0f)
                    if (std::optional<int32> move = row.Allowed(BlockId::Duel, DuelBlock::ACTION_MOVE_TO_TARGET))
                        return move;

                // A hunter cannot shoot inside melee reach: with its pet on the target, it steps back out and lets
                // the pet hold it. A caster casts where it stands.
                if (layout.Profile->Class == CLASS_HUNTER && inMelee
                    && row.Obs(BlockId::Duel, DuelBlock::OBS_TARGET_ATTACKS_BOT) > 0.0f
                    && row.Obs(BlockId::Duel, DuelBlock::OBS_PET_ATTACKING) > 0.0f)
                    if (std::optional<int32> back = row.Allowed(BlockId::Duel, DuelBlock::ACTION_BACK_OFF))
                        return back;
            }

            // Melee only as the fallback once the target is on it: a hunter with no pet yet, or one still held.
            if (inMelee)
                if (std::optional<int32> attack = row.Allowed(BlockId::Duel, DuelBlock::ACTION_START_ATTACK))
                    return attack;

            return std::nullopt;
        }

        if (std::optional<int32> attack = row.Allowed(BlockId::Duel, DuelBlock::ACTION_START_ATTACK))
            return attack;

        if (hasTarget && !inMelee && !moving)
            if (std::optional<int32> move = row.Allowed(BlockId::Duel, DuelBlock::ACTION_MOVE_TO_TARGET))
                return move;

        return std::nullopt;
    }
}

bool Animus::Curriculum::Baselines::Supports(std::string const& policy, Layout const& layout)
{
    return policy == "greedy" || (policy == "fight" && layout.Has(BlockId::Duel));
}

int32 Animus::Curriculum::Baselines::Choose(std::string const& policy, Layout const& layout, float const* obs,
    uint8 const* mask)
{
    Row const row(layout, obs, mask);

    if (policy == "fight" && layout.Has(BlockId::Duel))
    {
        if (std::optional<int32> action = Fight(row, layout))
            return *action;
        if (std::optional<int32> ability = PetDamage(row, layout))
            return *ability;
        if (std::optional<int32> spell = Rotation(row, layout))
            return *spell;
        return 0;
    }

    // greedy: the first usable spell or trinket in catalog order.
    uint32 const catalog = uint32(layout.Catalog().Actions().size());
    for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < catalog; ++action)
        if (std::optional<int32> allowed = row.Allowed(BlockId::Core, action))
            return *allowed;

    return 0;
}

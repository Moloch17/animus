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

#include "ClassRoleLayout.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <cmath>

namespace
{
    using Animus::ClassRole::ActionCatalog;

    std::string ActionJson(ActionCatalog::Action const& action)
    {
        switch (action.Type)
        {
            case ActionCatalog::Kind::Noop:
                return R"({"kind":"noop"})";
            case ActionCatalog::Kind::CancelQueued:
                return R"({"kind":"cancel_queued"})";
            case ActionCatalog::Kind::Trinket:
                return Acore::StringFormat(R"({{"kind":"trinket","slot":{}}})", action.EquipmentSlot);
            case ActionCatalog::Kind::Spell:
                break;
        }

        return Acore::StringFormat(R"({{"kind":"spell","first_rank":{},"next_swing":{}}})", action.FirstRank,
            action.NextSwing ? "true" : "false");
    }

    std::string SpellList(std::vector<ActionCatalog::Action> const& actions)
    {
        std::string out = "[";
        for (std::size_t i = 0; i < actions.size(); ++i)
            out += (i ? "," : "") + std::to_string(actions[i].FirstRank);
        return out + "]";
    }
}

float Animus::ClassRole::DamageScale(uint8 level)
{
    return 15.0f * std::exp(0.068f * float(level));
}

char const* Animus::ClassRole::StageScenarioName(Stage stage)
{
    switch (stage)
    {
        case Stage::Duel:      return "class_role_duel";
        case Stage::Pack:      return "class_role_pack";
        case Stage::Gauntlet:  return "class_role_gauntlet";
        case Stage::Companion: return "class_role_companion";
        case Stage::Party:     return "class_role_party";
        case Stage::Pvp:       return "class_role_pvp";
        case Stage::Arena:     return "class_role_arena";
        case Stage::Dummy:     break;
    }

    return "class_role";
}

char const* Animus::ClassRole::StageSuffix(Stage stage)
{
    return StageScenarioName(stage) + std::char_traits<char>::length("class_role");
}

Animus::ClassRole::Layout Animus::ClassRole::Layout::Build(ClassRoleProfile const& profile, Stage stage)
{
    // Every stage keeps the previous stage's layout unchanged and appends its own (see Stage).
    Layout layout;
    layout.StageId = stage;
    layout.Profile = &profile;
    layout.Assets = &ClassRoleAssets::For(profile);

    ActionCatalog const& catalog = layout.Catalog();
    uint32 const actions = uint32(catalog.Actions().size());
    uint32 const talents = uint32(layout.Assets->Talents->Talents().size());

    layout.ActionObsFirst = OBS_GLOBAL_COUNT;
    layout.TalentObsFirst = layout.ActionObsFirst + actions * ACTION_FEATURES;
    layout.TreeObsFirst = layout.TalentObsFirst + talents;
    layout.ObsDim = layout.TreeObsFirst + TalentBuilder::TREE_COUNT;
    layout.NumActions = actions;

    if (layout.Has(Stage::Duel))
    {
        uint32 const stable = profile.Class == CLASS_HUNTER ? STABLE_SLOTS : 0;
        layout.DuelObsFirst = layout.ObsDim;
        layout.ObsDim += DUEL_OBS_COUNT_WITHOUT_STABLE + stable * STABLE_FEATURES;
        layout.DuelActionFirst = layout.NumActions;
        layout.DuelActionCount = DUEL_ACTION_COUNT_WITHOUT_STABLE + stable;
        layout.NumActions += layout.DuelActionCount;
    }

    if (layout.Has(Stage::Pack))
    {
        uint32 const tactical = uint32(catalog.Tactical().size());
        layout.PackObsFirst = layout.ObsDim;
        layout.ObsDim += PACK_OBS_GLOBAL_COUNT + PACK_SLOTS * SLOT_FEATURES + tactical * 2;
        layout.PackActionFirst = layout.NumActions;
        layout.PackActionCount = PACK_SLOTS + tactical;
        layout.NumActions += layout.PackActionCount;
    }

    if (layout.Has(Stage::Gauntlet))
    {
        uint32 const sustain = uint32(catalog.Sustain().size());
        layout.GauntletObsFirst = layout.ObsDim;
        layout.ObsDim += GAUNTLET_OBS_GLOBAL_COUNT + sustain * 2;
        layout.GauntletActionFirst = layout.NumActions;
        layout.GauntletActionCount = GAUNTLET_ACTION_SUSTAIN_FIRST + sustain;
        layout.NumActions += layout.GauntletActionCount;
    }

    if (layout.Has(Stage::Companion))
    {
        // Heals that take a friendly unit target can be cast on an ally.
        for (ActionCatalog::Action const& heal : catalog.Sustain())
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(heal.FirstRank);
                info && info->IsPositive() && info->NeedsExplicitUnitTarget())
                layout.AllyHeals.push_back(heal);

        uint32 const heals = uint32(layout.AllyHeals.size());
        layout.CompanionObsFirst = layout.ObsDim;
        layout.ObsDim += COMPANION_OBS_GLOBAL_COUNT + heals * 2;
        layout.CompanionActionFirst = layout.NumActions;
        layout.CompanionActionCount = COMPANION_ACTION_HEAL_FIRST + heals;
        layout.NumActions += layout.CompanionActionCount;
    }

    if (layout.Has(Stage::Party))
    {
        layout.PartyObsFirst = layout.ObsDim;
        layout.ObsDim += PARTY_OBS_GLOBAL_COUNT + PARTY_MEMBERS * MEMBER_FEATURES;
        layout.PartyActionFirst = layout.NumActions;
        layout.PartyActionCount = PARTY_ACTION_HEAL_FIRST + PARTY_MEMBERS * uint32(layout.AllyHeals.size());
        layout.NumActions += layout.PartyActionCount;
    }

    if (layout.Has(Stage::Pvp))
    {
        layout.PvpObsFirst = layout.ObsDim;
        layout.ObsDim += PVP_OBS_COUNT;
    }

    return layout;
}

std::string Animus::ClassRole::Layout::ModelName() const
{
    return Profile->ScenarioName + StageSuffix(StageId);
}

std::string Animus::ClassRole::Layout::Manifest() const
{
    std::string actions = "[";
    std::vector<ActionCatalog::Action> const& catalog = Catalog().Actions();
    for (std::size_t i = 0; i < catalog.size(); ++i)
        actions += (i ? "," : "") + ActionJson(catalog[i]);
    actions += "]";

    std::string talents = "[";
    std::vector<TalentBuilder::Talent> const& talentList = Assets->Talents->Talents();
    for (std::size_t i = 0; i < talentList.size(); ++i)
        talents += Acore::StringFormat("{}[{},{}]", i ? "," : "", talentList[i].TalentId, talentList[i].MaxRank);
    talents += "]";

    std::string specs = "[";
    for (std::size_t i = 0; i < Profile->Specs.size(); ++i)
        specs += Acore::StringFormat("{}{}", i ? "," : "", Profile->Specs[i].TabPage);
    specs += "]";

    return Acore::StringFormat(
        R"({{"format":1,"model":"{}","scenario":"{}","class_role":"{}","class":{},"role":"{}","obs_dim":{},)"
        R"("num_actions":{},"blocks":{{"action_obs":{},"talent_obs":{},"tree_obs":{},"duel_obs":{},"duel_actions":[{},{}],)"
        R"("pack_obs":{},"pack_actions":[{},{}],"gauntlet_obs":{},"gauntlet_actions":[{},{}],"companion_obs":{},)"
        R"("companion_actions":[{},{}],"party_obs":{},"party_actions":[{},{}],"pvp_obs":{}}},"specs":{},)"
        R"("actions":{},"tactical":{},"sustain":{},"ally_heals":{},"talents":{}}})",
        ModelName(), StageScenarioName(StageId), Profile->ScenarioName, Profile->Class, RoleName(PlayRole()), ObsDim,
        NumActions, ActionObsFirst, TalentObsFirst, TreeObsFirst, DuelObsFirst, DuelActionFirst, DuelActionCount,
        PackObsFirst, PackActionFirst, PackActionCount, GauntletObsFirst, GauntletActionFirst, GauntletActionCount,
        CompanionObsFirst, CompanionActionFirst, CompanionActionCount, PartyObsFirst, PartyActionFirst,
        PartyActionCount, PvpObsFirst, specs, actions, SpellList(Catalog().Tactical()), SpellList(Catalog().Sustain()),
        SpellList(AllyHeals), talents);
}

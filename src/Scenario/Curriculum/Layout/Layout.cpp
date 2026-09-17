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

#include "Layout.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StageDefinition.h"
#include "StringFormat.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

namespace
{
    /// Manifest format: 3 lists blocks generically (format 2 had one fixed field per stage block).
    constexpr uint32 MANIFEST_FORMAT = 3;
}

std::string_view Animus::Curriculum::BlockName(BlockId id)
{
    switch (id)
    {
        case BlockId::Core:      return "core";
        case BlockId::Duel:      return "duel";
        case BlockId::Pack:      return "pack";
        case BlockId::Gauntlet:  return "gauntlet";
        case BlockId::Companion: return "companion";
        case BlockId::Party:     return "party";
        case BlockId::Pvp:       return "pvp";
        case BlockId::Context:   return "context";
        case BlockId::Hostiles:  return "hostiles";
        case BlockId::Pet:       return "pet";
        case BlockId::Travel:    return "travel";
        case BlockId::Flag:      return "flag";
        case BlockId::Count:     break;
    }

    return "unknown";
}

std::optional<Animus::Curriculum::BlockId> Animus::Curriculum::FindBlock(std::string_view name)
{
    for (std::size_t i = 0; i < BLOCK_COUNT; ++i)
        if (BlockName(BlockId(i)) == name)
            return BlockId(i);

    return std::nullopt;
}

Animus::Curriculum::Layout Animus::Curriculum::Layout::Build(ClassRoleProfile const& profile,
    StageDefinition const& stage)
{
    Layout layout;
    layout.Stage = &stage;
    layout.Profile = &profile;
    layout.Assets = &ClassRoleAssets::For(profile);
    layout.Blocks = stage.Blocks;

    // Positive spells that take a friendly unit target, resurrections and the soulstone can be cast on an ally
    // (companion and party blocks): the heals first, then everything else a player casts on a friend.
    if (stage.Has(BlockId::Companion) || stage.Has(BlockId::Party))
    {
        auto const onAlly = [](ActionCatalog::Action const& action)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(action.FirstRank);
            return info && info->IsPositive() && info->NeedsExplicitUnitTarget();
        };

        for (ActionCatalog::Action const& heal : layout.Catalog().Sustain())
            if (onAlly(heal))
                layout.AllySpells.push_back(heal);
        layout.AllyHealCount = uint32(layout.AllySpells.size());

        for (ActionCatalog::Action const& action : layout.Catalog().Actions())
            if (action.Type == ActionCatalog::Kind::Spell && onAlly(action))
                layout.AllySpells.push_back(action);

        layout.AllyRevives = layout.Catalog().Revives();
    }

    // Each block starts where the previous one ended.
    for (BlockId id : layout.Blocks)
    {
        BlockSize const size = GetBlock(id).Size(layout);
        layout.Slices[std::size_t(id)] = { layout.ObsDim, size.Obs, layout.NumActions, size.Actions };
        layout.ObsDim += size.Obs;
        layout.NumActions += size.Actions;
        layout._blockMask |= 1u << uint32(id);
    }

    layout.MoveDirections.assign(layout.NumActions, 0);
    layout.ModeGroups.assign(layout.NumActions, 0);
    for (BlockId id : layout.Blocks)
    {
        BlockSlice const& slice = layout.Slice(id);
        for (uint32 local = 0; local < slice.ActionCount; ++local)
        {
            layout.MoveDirections[slice.ActionFirst + local] = GetBlock(id).MoveDirection(local);
            layout.ModeGroups[slice.ActionFirst + local] = uint8(GetBlock(id).ModeGroupOf(layout, local));
        }
    }

    return layout;
}

std::optional<Animus::Curriculum::BlockId> Animus::Curriculum::Layout::BlockOfAction(uint32 action) const
{
    for (BlockId id : Blocks)
        if (Slice(id).ContainsAction(action))
            return id;

    return std::nullopt;
}

boost::json::array Animus::Curriculum::SpellList(std::vector<ActionCatalog::Action> const& actions)
{
    boost::json::array list;
    list.reserve(actions.size());
    for (ActionCatalog::Action const& action : actions)
        list.push_back(action.FirstRank);

    return list;
}

boost::json::array Animus::Curriculum::Span(uint32 first, uint32 count)
{
    return { first, count };
}

std::string Animus::Curriculum::Layout::ModelName() const
{
    return Profile->Name + Stage->Suffix;
}

std::vector<std::string> Animus::Curriculum::Layout::ActionNames() const
{
    std::vector<std::string> names(NumActions);
    for (BlockId id : Blocks)
    {
        BlockSlice const& slice = Slice(id);
        for (uint32 local = 0; local < slice.ActionCount && slice.ActionFirst + local < NumActions; ++local)
        {
            std::string name = GetBlock(id).ActionName(*this, local);
            names[slice.ActionFirst + local] = name.empty()
                ? Acore::StringFormat("{}_{}", BlockName(id), local) : std::move(name);
        }
    }

    return names;
}

std::string Animus::Curriculum::Layout::Manifest() const
{
    boost::json::object manifest;
    manifest["format"] = MANIFEST_FORMAT;
    manifest["model"] = ModelName();
    manifest["stage"] = Stage->Name;
    manifest["class_role"] = Profile->Name;
    manifest["class"] = Profile->Class;
    manifest["role"] = RoleName(PlayRole());
    manifest["obs_dim"] = ObsDim;
    manifest["num_actions"] = NumActions;

    boost::json::array& actionNames = manifest["action_names"].emplace_array();
    for (std::string const& name : ActionNames())
        actionNames.push_back(boost::json::string(name));

    boost::json::array& specs = manifest["specs"].emplace_array();
    for (SpecProfile const& spec : Profile->Specs)
        specs.push_back(spec.TabPage);

    boost::json::array& blocks = manifest["blocks"].emplace_array();
    for (BlockId id : Blocks)
    {
        BlockSlice const& slice = Slice(id);
        boost::json::object& block = blocks.emplace_back(boost::json::object()).get_object();
        block["name"] = BlockName(id);
        block["obs"] = Span(slice.ObsFirst, slice.ObsCount);
        block["actions"] = Span(slice.ActionFirst, slice.ActionCount);
        GetBlock(id).DescribeManifest(*this, block);
    }

    return boost::json::serialize(manifest);
}

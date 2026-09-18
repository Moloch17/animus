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

#include "SeatEncoder.h"
#include "CoreBlock.h"
#include "DuelBlock.h"
#include "Player.h"
#include <algorithm>

void Animus::Curriculum::SeatEncoder::Observe(SeatView const& view, float* obs, uint8* mask)
{
    Layout const& layout = *view.L;
    std::fill(obs, obs + layout.ObsDim, 0.0f);
    if (mask)
    {
        std::fill(mask, mask + layout.NumActions, 0);
        mask[0] = 1;
    }

    // A block's slice of the mask, or null when no mask is wanted.
    auto const blockMask = [mask](BlockSlice const& slice) { return mask ? mask + slice.ActionFirst : nullptr; };

    CoreBlock::ObserveCharacter(view, obs);

    Player* bot = view.Bot;
    if (bot && !bot->IsAlive())
    {
        // Dead: whether it can resurrect itself, and the action that does.
        if (layout.Has(BlockId::Duel))
        {
            BlockSlice const& duel = layout.Slice(BlockId::Duel);
            DuelBlock::ObserveDead(view, obs + duel.ObsFirst, blockMask(duel));
        }
        return;
    }

    // A hidden target still counts as one: the blocks see no target, and the duel block searches for it.
    if (!bot || (!view.Target && !view.HiddenTarget && !ActsWithoutTarget(layout)))
        return;

    for (BlockId id : layout.Blocks)
    {
        BlockSlice const& slice = layout.Slice(id);
        GetBlock(id).Observe(view, obs + slice.ObsFirst, blockMask(slice));
    }

    // The no-op stays allowed whatever the core block decided.
    if (mask)
        mask[0] = 1;
}

void Animus::Curriculum::SeatEncoder::Apply(SeatView& view, int32 action, SeatActionResult& result)
{
    if (!view.Bot || !view.L)
        return;

    Layout const& layout = *view.L;

    // Dead: only its own resurrection.
    if (!view.Bot->IsAlive())
    {
        BlockSlice const* duel = layout.Has(BlockId::Duel) ? &layout.Slice(BlockId::Duel) : nullptr;
        if (duel && action == int32(duel->ActionFirst + DuelBlock::ACTION_SELF_RESURRECT))
            GetBlock(BlockId::Duel).Apply(view, DuelBlock::ACTION_SELF_RESURRECT, result);
        return;
    }

    if (!view.Target && !view.HiddenTarget && !ActsWithoutTarget(layout))
        return;

    std::optional<BlockId> const block = action > 0 ? layout.BlockOfAction(uint32(action)) : std::nullopt;
    uint32 const local = block ? uint32(action) - layout.Slice(*block).ActionFirst : 0;

    // A durative action runs until the policy does something else: anything but the no-op takes over from it, except
    // that a positioning option (IsPositioning) survives everything but the seat moving itself -- casting and
    // swinging are what it is there to keep the seat in place for. Its own action is masked while it runs, so this
    // cannot cancel a press of the option that is already going.
    if (action > 0 && view.Option && (!IsPositioning(view.Option->Kind)
        || (block && GetBlock(*block).IsMovement(local))))
        *view.Option = SeatOption();

    // Every decision, whatever the action (the no-op included): this is where a running option acts.
    for (BlockId id : layout.Blocks)
        GetBlock(id).BeforeApply(view, result);

    if (action <= 0)
        return;

    if (block)
        GetBlock(*block).Apply(view, local, result);
}

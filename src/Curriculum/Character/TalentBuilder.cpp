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

#include "TalentBuilder.h"
#include "DBCStores.h"
#include "Player.h"
#include "Random.h"
#include <algorithm>
#include <map>

Animus::Curriculum::TalentBuilder::TalentBuilder(uint8 playerClass)
{
    uint32 const classMask = 1 << (playerClass - 1);

    std::map<uint32, uint8> tabPages;   // TalentTabID -> tab page
    for (uint32 i = 0; i < sTalentTabStore.GetNumRows(); ++i)
        if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(i))
            if (tab->ClassMask & classMask)
                tabPages[tab->TalentTabID] = uint8(tab->tabpage);

    std::vector<TalentEntry const*> entries;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        if (TalentEntry const* entry = sTalentStore.LookupEntry(i))
            if (tabPages.contains(entry->TalentTab))
                entries.push_back(entry);

    std::sort(entries.begin(), entries.end(), [&](TalentEntry const* a, TalentEntry const* b)
    {
        uint8 const tabA = tabPages[a->TalentTab];
        uint8 const tabB = tabPages[b->TalentTab];
        if (tabA != tabB)
            return tabA < tabB;
        return a->Row != b->Row ? a->Row < b->Row : a->Col < b->Col;
    });

    for (TalentEntry const* entry : entries)
    {
        Talent talent;
        talent.TalentId = entry->TalentID;
        talent.Tab = tabPages[entry->TalentTab];
        talent.Row = entry->Row;
        for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
        {
            talent.RankSpells[rank] = entry->RankID[rank];
            if (entry->RankID[rank])
                talent.MaxRank = rank + 1;
        }

        _talents.push_back(talent);
    }

    // Prerequisites by index. Player::LearnTalent accepts the prerequisite at rank index DependsOnRank
    // or higher, i.e. DependsOnRank + 1 ranks.
    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (!entries[i]->DependsOn)
            continue;

        for (size_t j = 0; j < entries.size(); ++j)
        {
            if (entries[j]->TalentID == entries[i]->DependsOn)
            {
                _talents[i].DependsOn = int32(j);
                _talents[i].DependsOnRanks = uint8(entries[i]->DependsOnRank + 1);
                break;
            }
        }
    }
}

Animus::Curriculum::TalentBuilder::Build Animus::Curriculum::TalentBuilder::Random(uint8 specTab, uint32 points) const
{
    Build build;
    build.Ranks.assign(_talents.size(), 0);

    uint32 const specPoints = std::min(points, SPEC_TREE_POINTS);
    uint32 const otherTrees = ((1u << TREE_COUNT) - 1) & ~(1u << specTab);

    Spend(build, 1u << specTab, specPoints);
    Spend(build, otherTrees, points - uint32(build.Order.size()));

    // Only if the other trees could not take the rest: back to the spec tree.
    Spend(build, 1u << specTab, points - uint32(build.Order.size()));

    return build;
}

void Animus::Curriculum::TalentBuilder::Spend(Build& build, uint32 treeMask, uint32 points) const
{
    std::vector<uint32> candidates;
    for (uint32 point = 0; point < points; ++point)
    {
        candidates.clear();
        for (uint32 i = 0; i < _talents.size(); ++i)
        {
            Talent const& talent = _talents[i];
            if (!(treeMask & (1u << talent.Tab)) || build.Ranks[i] >= talent.MaxRank)
                continue;

            if (build.TreePoints[talent.Tab] < talent.Row * MAX_TALENT_RANK)
                continue;

            if (talent.DependsOn >= 0 && build.Ranks[talent.DependsOn] < talent.DependsOnRanks)
                continue;

            candidates.push_back(i);
        }

        if (candidates.empty())
            return;

        uint32 const chosen = candidates[urand(0, uint32(candidates.size()) - 1)];
        build.Order.push_back({ chosen, build.Ranks[chosen] });
        ++build.Ranks[chosen];
        ++build.TreePoints[_talents[chosen].Tab];
    }
}

uint32 Animus::Curriculum::TalentBuilder::Apply(Player* bot, Build const& build) const
{
    for (Step const& step : build.Order)
        bot->LearnTalent(_talents[step.Index].TalentId, step.Rank);

    return bot->GetFreeTalentPoints();
}

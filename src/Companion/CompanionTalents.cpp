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

#include "CompanionTalents.h"
#include "DBCStores.h"
#include "Pet.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include <algorithm>
#include <array>

namespace
{
    constexpr uint32 POINTS_PER_ROW = 5;        // a character's tree: points in the rows above a row it may spend in
    constexpr uint32 PET_POINTS_PER_ROW = 3;

    /// Ranks (1-based) a character has of a talent, in its active spec.
    uint8 RankOf(Player const* bot, TalentEntry const* talent)
    {
        for (uint8 rank = MAX_TALENT_RANK; rank > 0; --rank)
            if (talent->RankID[rank - 1] && bot->HasTalent(talent->RankID[rank - 1], bot->GetActiveSpec()))
                return rank;
        return 0;
    }

    uint8 PetRankOf(Pet const* pet, TalentEntry const* talent)
    {
        for (uint8 rank = MAX_PET_TALENT_RANK; rank > 0; --rank)
            if (talent->RankID[rank - 1] && pet->HasSpell(talent->RankID[rank - 1]))
                return rank;
        return 0;
    }

    /// A talent of the character's class, or null with `message` set.
    TalentEntry const* TalentOf(Player const* bot, uint32 talentId, std::string& message)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(talentId);
        TalentTabEntry const* tab = talent ? sTalentTabStore.LookupEntry(talent->TalentTab) : nullptr;
        if (!tab || !(tab->ClassMask & bot->getClassMask()))
        {
            message = Acore::StringFormat("Talent {} is not one of a {}'s.", talentId, bot->getClass());
            return nullptr;
        }
        return talent;
    }

    /// The pet's tree (ferocity, tenacity or cunning), or null with `message` set for a pet without one.
    TalentTabEntry const* PetTabOf(Pet const* pet, std::string& message)
    {
        CreatureFamilyEntry const* family = pet->getPetType() == HUNTER_PET
            ? sCreatureFamilyStore.LookupEntry(pet->GetCreatureTemplate()->family) : nullptr;
        if (!family || family->petTalentType < 0)
        {
            message = "This pet has no talents.";
            return nullptr;
        }

        for (uint32 i = 0; i < sTalentTabStore.GetNumRows(); ++i)
            if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(i))
                if (tab->petTalentMask & (1 << family->petTalentType))
                    return tab;

        message = "This pet has no talents.";
        return nullptr;
    }

    /// Whether the tree still holds together with `talent` one rank lower: every other talent of the tree keeps
    /// its points in the rows above (the client's rule, stricter than LearnTalent's total), and every talent that
    /// depends on it still has the rank it needs. `rankOf` answers for the tree's talents.
    template <typename RankOf>
    bool CanLower(TalentEntry const* talent, uint32 tab, uint32 perRow, RankOf rankOf, std::string& message)
    {
        uint8 const rank = rankOf(talent);
        std::array<uint32, 16> rowPoints{};
        std::vector<TalentEntry const*> known;
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* other = sTalentStore.LookupEntry(i);
            if (!other || other->TalentTab != tab || other->Row >= rowPoints.size())
                continue;

            uint8 const otherRank = rankOf(other);
            if (!otherRank)
                continue;

            known.push_back(other);
            rowPoints[other->Row] += otherRank - (other == talent ? 1 : 0);
        }

        for (TalentEntry const* other : known)
        {
            if (other->DependsOn == talent->TalentID && rank - 1 < other->DependsOnRank + 1)
            {
                message = Acore::StringFormat("Another talent needs rank {} of this one.", other->DependsOnRank + 1);
                return false;
            }

            uint32 above = 0;
            for (uint32 row = 0; row < other->Row; ++row)
                above += rowPoints[row];
            if (other == talent && rank == 1)
                continue;
            if (above < other->Row * perRow)
            {
                message = "A talent further down the tree needs the points above it.";
                return false;
            }
        }
        return true;
    }
}

bool Animus::CompanionTalents::Learn(Player* bot, uint32 talentId, std::string& message)
{
    TalentEntry const* talent = TalentOf(bot, talentId, message);
    if (!talent)
        return false;

    uint8 const rank = RankOf(bot, talent);
    if (rank >= MAX_TALENT_RANK || !talent->RankID[rank])
    {
        message = "That talent is at its highest rank.";
        return false;
    }

    if (!bot->GetFreeTalentPoints())
    {
        message = "No talent points to spend.";
        return false;
    }

    // LearnTalent checks the rest (the row, the prerequisite) and says nothing when it refuses.
    uint32 const before = bot->GetFreeTalentPoints();
    bot->LearnTalent(talentId, rank);
    if (bot->GetFreeTalentPoints() == before)
    {
        message = "That talent cannot be learned yet: it needs the points above it, or another talent first.";
        return false;
    }
    return true;
}

bool Animus::CompanionTalents::Unlearn(Player* bot, uint32 talentId, std::string& message)
{
    TalentEntry const* talent = TalentOf(bot, talentId, message);
    if (!talent)
        return false;

    uint8 const rank = RankOf(bot, talent);
    if (!rank)
    {
        message = "That talent is not learned.";
        return false;
    }

    if (!CanLower(talent, talent->TalentTab, POINTS_PER_ROW,
        [bot](TalentEntry const* other) { return RankOf(bot, other); }, message))
        return false;

    // What resetTalents does for every talent, for this one rank: its auras and spells go, the client is told,
    // and the point comes back. A lower rank is learned again as a command (no point spent), so the talent map
    // holds exactly one spell of the talent, as LearnTalent keeps it.
    uint32 const spellId = talent->RankID[rank - 1];
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    bot->_removeTalentAurasAndSpells(spellId);
    bool removed = false;
    if (talent->addToSpellBook && spellInfo && !spellInfo->HasAttribute(SPELL_ATTR0_PASSIVE)
        && !spellInfo->HasEffect(SPELL_EFFECT_LEARN_SPELL))
    {
        bot->removeSpell(spellId, bot->GetActiveSpecMask(), false);
        removed = true;
    }
    if (!removed)
        bot->SendLearnPacket(spellId, false);
    if (spellInfo)
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_LEARN_SPELL
                && sSpellMgr->IsAdditionalTalentSpell(spellInfo->Effects[i].TriggerSpell))
                bot->removeSpell(spellInfo->Effects[i].TriggerSpell, bot->GetActiveSpecMask(), false);
    bot->_removeTalent(spellId, bot->GetActiveSpecMask());
    bot->SetFreeTalentPoints(bot->GetFreeTalentPoints() + 1);

    if (rank > 1)
        bot->LearnTalent(talentId, rank - 2, true);

    // The core's private count of points spent is now off by the ranks learned again; it only matters to
    // InitTalentForLevel, and CompanionParty::LevelUp resets the talents before that runs. See LevelUp.
    return true;
}

bool Animus::CompanionTalents::LearnPet(Player* bot, Pet* pet, uint32 talentId, std::string& message)
{
    if (!PetTabOf(pet, message))
        return false;

    TalentEntry const* talent = sTalentStore.LookupEntry(talentId);
    if (!talent)
    {
        message = Acore::StringFormat("There is no talent {}.", talentId);
        return false;
    }

    uint8 const rank = PetRankOf(pet, talent);
    if (rank >= MAX_PET_TALENT_RANK || !talent->RankID[rank])
    {
        message = "That talent is at its highest rank.";
        return false;
    }

    if (!pet->GetFreeTalentPoints())
    {
        message = "The pet has no talent points to spend.";
        return false;
    }

    uint32 const before = pet->GetFreeTalentPoints();
    bot->LearnPetTalent(pet->GetGUID(), talentId, rank);
    if (pet->GetFreeTalentPoints() == before)
    {
        message = "That talent cannot be learned yet: it needs the points above it, another talent first, or it "
            "is not of this pet's tree.";
        return false;
    }
    return true;
}

bool Animus::CompanionTalents::UnlearnPet(Player* /*bot*/, Pet* pet, uint32 talentId, std::string& message)
{
    TalentTabEntry const* tab = PetTabOf(pet, message);
    if (!tab)
        return false;

    TalentEntry const* talent = sTalentStore.LookupEntry(talentId);
    uint8 const rank = talent ? PetRankOf(pet, talent) : 0;
    if (!rank)
    {
        message = "That talent is not learned.";
        return false;
    }

    if (!CanLower(talent, tab->TalentTabID, PET_POINTS_PER_ROW,
        [pet](TalentEntry const* other) { return PetRankOf(pet, other); }, message))
        return false;

    // Pet::removeSpell refunds the rank's points and, asked to, learns the rank below (charging its own).
    pet->unlearnSpell(talent->RankID[rank - 1], true);
    return true;
}

Animus::CompanionTalents::Snapshot Animus::CompanionTalents::Take(Player const* bot)
{
    Snapshot talents;
    for (auto const& [spellId, talent] : bot->GetTalentMap())
    {
        if (talent->State == PLAYERSPELL_REMOVED || !(talent->specMask & bot->GetActiveSpecMask()))
            continue;
        if (TalentSpellPos const* pos = GetTalentSpellPos(spellId))
            talents[pos->talent_id] = std::max<uint8>(talents[pos->talent_id], pos->rank + 1);
    }
    return talents;
}

Animus::CompanionTalents::Snapshot Animus::CompanionTalents::TakePet(Pet const* pet)
{
    Snapshot talents;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        if (TalentEntry const* talent = sTalentStore.LookupEntry(i))
            if (uint8 const rank = PetRankOf(pet, talent))
                talents[talent->TalentID] = rank;
    return talents;
}

namespace
{
    /// Learn `talents` in passes, each pass taking what its prerequisites and rows allow, until nothing more can
    /// be. `learn` returns whether a talent reached its rank. Returns the ranks left.
    template <typename Learn>
    uint32 ReplayWith(Animus::CompanionTalents::Snapshot talents, Learn learn)
    {
        for (bool progress = true; progress && !talents.empty();)
        {
            progress = false;
            for (auto itr = talents.begin(); itr != talents.end();)
            {
                if (learn(itr->first, itr->second))
                {
                    itr = talents.erase(itr);
                    progress = true;
                }
                else
                    ++itr;
            }
        }

        uint32 left = 0;
        for (auto const& [talentId, rank] : talents)
            left += rank;
        return left;
    }
}

uint32 Animus::CompanionTalents::Replay(Player* bot, Snapshot const& talents)
{
    return ReplayWith(talents, [bot](uint32 talentId, uint8 rank)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(talentId);
        if (!talent)
            return true;
        bot->LearnTalent(talentId, rank - 1);
        return RankOf(bot, talent) >= rank;
    });
}

uint32 Animus::CompanionTalents::ReplayPet(Player* bot, Pet* pet, Snapshot const& talents)
{
    return ReplayWith(talents, [bot, pet](uint32 talentId, uint8 rank)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(talentId);
        if (!talent)
            return true;
        bot->LearnPetTalent(pet->GetGUID(), talentId, rank - 1);
        return PetRankOf(pet, talent) >= rank;
    });
}

std::vector<Animus::CompanionTalents::PetTalent> Animus::CompanionTalents::PetTree(Pet const* pet)
{
    std::string ignored;
    TalentTabEntry const* tab = PetTabOf(pet, ignored);
    if (!tab)
        return {};

    std::vector<PetTalent> tree;
    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(i);
        if (!talent || talent->TalentTab != tab->TalentTabID)
            continue;

        PetTalent& entry = tree.emplace_back();
        entry.TalentId = talent->TalentID;
        entry.Row = talent->Row;
        entry.Col = talent->Col;
        for (uint32 spell : talent->RankID)
            if (spell)
                entry.Spells.push_back(spell);
        entry.MaxRank = uint8(entry.Spells.size());
        entry.Rank = PetRankOf(pet, talent);
        entry.DependsOn = talent->DependsOn;
        entry.DependsOnRank = uint8(talent->DependsOnRank + 1);
    }

    std::sort(tree.begin(), tree.end(), [](PetTalent const& a, PetTalent const& b)
    {
        return a.Row != b.Row ? a.Row < b.Row : a.Col < b.Col;
    });
    return tree;
}

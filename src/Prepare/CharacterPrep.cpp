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

#include "CharacterPrep.h"
#include "AnimusMod.h"
#include "Bag.h"
#include "Capture.h"
#include "ClassAssets.h"
#include "ClassKit.h"
#include "ClassProfile.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Layout.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SeatCharacter.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldSession.h"
#include <algorithm>

namespace
{
    using namespace Animus;

    constexpr uint32 HEARTHSTONE = 6948;

    /// Every item the gear builder would destroy -- the equipment, the backpack, the keyring aside, and the bags
    /// with their contents -- mailed to the character in packages of MAX_MAIL_ITEMS. The bags go last, empty.
    uint32 MailEverything(Player* player, uint32& mails)
    {
        std::vector<std::pair<uint8, uint8>> positions;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (Bag const* bag = player->GetBagByPos(slot))
                for (uint32 inner = 0; inner < bag->GetBagSize(); ++inner)
                    if (bag->GetItemByPos(uint8(inner)))
                        positions.emplace_back(slot, uint8(inner));
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                positions.emplace_back(INVENTORY_SLOT_BAG_0, slot);
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                positions.emplace_back(INVENTORY_SLOT_BAG_0, slot);
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                positions.emplace_back(INVENTORY_SLOT_BAG_0, slot);

        mails = 0;
        uint32 items = 0;
        if (positions.empty())
            return 0;
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailSender const sender(MAIL_NORMAL, player->GetGUID().GetCounter(), MAIL_STATIONERY_GM);
        for (std::size_t from = 0; from < positions.size(); from += MAX_MAIL_ITEMS)
        {
            MailDraft draft("Your things before Prepare", "Animus Prepare set your character up for a recording. "
                "Everything you wore and carried is in these mails, bags included.");
            uint32 packed = 0;
            for (std::size_t i = from; i < positions.size() && i < from + MAX_MAIL_ITEMS; ++i)
            {
                auto const [bag, slot] = positions[i];
                Item* item = player->GetItemByPos(bag, slot);
                if (!item)
                    continue;
                player->MoveItemFromInventory(bag, slot, true);
                item->DeleteFromInventoryDB(trans);
                item->SaveToDB(trans);
                draft.AddItem(item);
                ++packed;
            }
            if (!packed)
                continue;
            draft.SendMailTo(trans, MailReceiver(player, player->GetGUID().GetCounter()), sender, MAIL_CHECK_MASK_COPIED);
            ++mails;
            items += packed;
        }
        CharacterDatabase.CommitTransaction(trans);
        return items;
    }

    /// The class kit's spells above `level` forgotten (a character prepared lower than it was): never the whole
    /// spell book, which would take its professions with it.
    uint32 ForgetAbove(Player* player, Curriculum::ClassKit const& kit, uint8 level)
    {
        std::vector<uint32> above;
        for (auto const& [spellId, spell] : player->GetSpellMap())
        {
            if (!spell || spell->State == PLAYERSPELL_REMOVED)
                continue;
            uint8 const taught = kit.LevelOf(spellId);
            if (taught && taught > level)
                above.push_back(spellId);
        }
        for (uint32 spellId : above)
            player->removeSpell(spellId, SPEC_MASK_ALL, false);
        return uint32(above.size());
    }

    uint32 GiveBags(Player* player, uint32 bagEntry)
    {
        uint32 given = 0;
        if (!bagEntry || !sObjectMgr->GetItemTemplate(bagEntry))
            return 0;
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;
            uint16 dest = 0;
            if (player->CanEquipNewItem(slot, dest, bagEntry, false) != EQUIP_ERR_OK)
                continue;
            if (player->EquipNewItem(dest, bagEntry, true))
                ++given;
        }
        return given;
    }

    void GiveHearthstone(Player* player)
    {
        if (player->GetItemCount(HEARTHSTONE))
            return;
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, HEARTHSTONE, 1) == EQUIP_ERR_OK)
            player->StoreNewItem(dest, HEARTHSTONE, true);
    }
}

bool Animus::Prepare::Allowed(Player const* player)
{
    return player && player->GetSession()
        && uint32(player->GetSession()->GetSecurity()) >= sAnimusMod->Config().PrepareSecurity;
}

std::vector<std::string> Animus::Prepare::Specs(Player const* player)
{
    std::vector<std::string> names;
    Curriculum::ClassProfile const* profile = player ? Curriculum::ClassAssets::FindProfile(player->getClass())
        : nullptr;
    if (!profile)
        return names;
    for (Curriculum::SpecProfile const& spec : profile->Specs)
        names.push_back(spec.Name);
    return names;
}

void Animus::Prepare::Levels(Player const* player, uint8& minLevel, uint8& maxLevel)
{
    minLevel = player ? Curriculum::ClassKit::MinLevelOf(player->getClass()) : 1;
    maxLevel = uint8(std::min<uint32>(DEFAULT_MAX_LEVEL, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));
    if (minLevel > maxLevel)
        minLevel = maxLevel;
}

bool Animus::Prepare::Prepare(Player* player, uint8 level, uint8 spec, std::string& message)
{
    if (!Allowed(player))
    {
        message = "Prepare is not for your account on this realm (Animus.Prepare.Security).";
        return false;
    }
    if (!player->IsInWorld() || player->IsBeingTeleported())
    {
        message = "Wait until you have arrived.";
        return false;
    }
    if (player->IsInCombat())
    {
        message = "Not in a fight.";
        return false;
    }
    if (!player->IsAlive())
    {
        message = "Not while dead.";
        return false;
    }
    if (player->IsInFlight())
    {
        message = "Not on a flight.";
        return false;
    }
    if (player->InBattleground())
    {
        message = "Not in a battleground or arena.";
        return false;
    }
    if (Capture::Status(player).On)
    {
        message = "Stop recording first: Prepare changes the character the recording is of.";
        return false;
    }
    Curriculum::ClassProfile const* profile = Curriculum::ClassAssets::FindProfile(player->getClass());
    if (!profile)
    {
        message = "Your class has no Animus profile.";
        return false;
    }
    uint8 minLevel = 1;
    uint8 maxLevel = DEFAULT_MAX_LEVEL;
    Levels(player, minLevel, maxLevel);
    if (level < minLevel || level > maxLevel)
    {
        message = Acore::StringFormat("A {} is level {} to {}.", profile->Name, minLevel, maxLevel);
        return false;
    }
    if (spec >= profile->Specs.size())
    {
        message = Acore::StringFormat("Spec {} is not one of {}'s {} specs.", spec, profile->Name,
            profile->Specs.size());
        return false;
    }

    uint8 const before = player->GetLevel();
    Curriculum::ClassAssets const& assets = Curriculum::ClassAssets::For(*profile);
    Curriculum::Layout const& layout = sAnimusMod->PrepareLayout(*profile);

    // 1. Everything worn and carried, into the mail (the gear builder destroys those slots, bags included).
    uint32 mails = 0;
    uint32 const mailed = MailEverything(player, mails);

    // 2. The pet away; the level; what a lower level must not know; talents from scratch.
    if (Pet* pet = player->GetPet())
        player->RemovePet(pet, PET_SAVE_AS_CURRENT);
    if (level != before)
    {
        player->GiveLevel(level);
        player->SetUInt32Value(PLAYER_XP, 0);
    }
    uint32 forgotten = 0;
    if (level < before && assets.Kit)
        forgotten = ForgetAbove(player, *assets.Kit, level);
    player->resetTalents(true);
    player->InitTalentForLevel();

    // 3. Built as a companion is: proficiencies, the spec's standard talents, the kit's spells, glyphs, gear,
    //    reagents and ammo (SeatCharacter::Configure), then full health and power.
    Curriculum::SeatCharacter::Built const built = Curriculum::SeatCharacter::Configure(player, layout, spec, false);

    // 4. Bags back (empty ones), a hearthstone, and the character saved as it stands.
    uint32 const bags = GiveBags(player, sAnimusMod->Config().PrepareBagItem);
    GiveHearthstone(player);
    player->SendTalentsInfoData(false);
    player->SaveToDB(false, false);

    LOG_INFO("module.animus", "Prepare: account {} character {} ({}) level {} -> {}, spec {} ({}), {} items worn, {} "
        "items mailed in {} mails, {} spells forgotten, {} bags given", player->GetSession()->GetAccountId(),
        player->GetName(), player->GetGUID().ToString(), before, level, spec, profile->Specs[spec].Name,
        built.EquippedItems, mailed, mails, forgotten, bags);
    message = Acore::StringFormat("Level {} {} {} ready: {} items worn{}{}.", level, profile->Specs[spec].Name,
        profile->Name, built.EquippedItems, mailed ? Acore::StringFormat("; {} mail{} hold{} your previous gear and bags",
        mails, mails == 1 ? "" : "s", mails == 1 ? "s" : "") : "", built.UnspentTalentPoints
        ? Acore::StringFormat("; {} talent points left to spend", built.UnspentTalentPoints) : "");
    return true;
}

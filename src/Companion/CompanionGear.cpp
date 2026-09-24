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

#include "CompanionGear.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "StringFormat.h"
#include <algorithm>

namespace
{
    /// The owner's bag and slot as the client numbers them, in the core's terms. False for a slot that is not one.
    bool Locate(uint8 bag, uint8 slot, uint8& outBag, uint8& outSlot)
    {
        if (bag == 0)
        {
            if (slot < 1 || slot > INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START)
                return false;
            outBag = INVENTORY_SLOT_BAG_0;
            outSlot = uint8(INVENTORY_SLOT_ITEM_START + slot - 1);
            return true;
        }

        if (bag > INVENTORY_SLOT_BAG_END - INVENTORY_SLOT_BAG_START || slot < 1)
            return false;
        outBag = uint8(INVENTORY_SLOT_BAG_START + bag - 1);
        outSlot = uint8(slot - 1);
        return true;
    }

    /// An item leaves a player (the owner or the companion, both saved characters): out of their bags and, as
    /// mail does it, out of their inventory in the database now, so the next save cannot bring it back. The item
    /// row stays; the taker's save moves it over.
    void TakeFrom(Player* player, Item* item)
    {
        player->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->DeleteFromInventoryDB(trans);
        CharacterDatabase.CommitTransaction(trans);
    }

    /// An item goes into `player`'s bags at `dest` (found with CanStoreItem). One never saved yet (a companion's
    /// gear generated at its last level-up) is written as new rather than updated where there is no row.
    void GiveTo(Player* player, Item* item, ItemPosCountVec const& dest)
    {
        player->MoveItemToInventory(dest, item, true, item->GetState() != ITEM_NEW);
    }

    /// Whether `player` may take `item` (CanEquipItem or CanStoreItem), asked as its owner. Every soulbound item
    /// is refused for anyone but its owner (Item::IsBindedNotWith), and everything here is bound: what the owner
    /// wore, what the companion equipped. The item is the other's for the question only; the moves that follow
    /// need the real owner back (RemoveFromUpdateQueueOf ignores an item that is not its player's).
    template <typename Check>
    InventoryResult AsOwner(Player* player, Item* item, Check check)
    {
        ObjectGuid const owner = item->GetOwnerGUID();
        item->SetOwnerGUID(player->GetGUID());
        InventoryResult const result = check();
        item->SetOwnerGUID(owner);
        return result;
    }
}

bool Animus::CompanionGear::Give(Player* owner, Player* bot, uint8 bag, uint8 slot, uint8 equipSlot,
    uint8& equipped, std::string& message)
{
    uint8 srcBag = 0;
    uint8 srcSlot = 0;
    if (!Locate(bag, slot, srcBag, srcSlot) || equipSlot < 1 || equipSlot > EQUIPMENT_SLOT_END)
    {
        message = "That is not a bag slot and an equipment slot.";
        return false;
    }

    Item* item = owner->GetItemByPos(srcBag, srcSlot);
    if (!item)
    {
        message = "There is no item in that bag slot.";
        return false;
    }

    if (item->IsBag())
    {
        message = "Companions carry their own bags.";
        return false;
    }

    uint8 const eslot = uint8(equipSlot - 1);
    uint16 dest = 0;
    Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, eslot);

    // HandleAutoEquipItemOpcode's swap: the worn item's enchantments are lifted while the fit is checked.
    if (worn)
        bot->ApplyEnchantment(worn, false);
    InventoryResult result = AsOwner(bot, item, [&] { return bot->CanEquipItem(eslot, dest, item, true); });
    if (worn)
        bot->ApplyEnchantment(worn, true);
    if (result != EQUIP_ERR_OK)
    {
        owner->SendEquipError(result, item, nullptr);
        message = Acore::StringFormat("{} cannot equip that there.", bot->GetName());
        return false;
    }

    ItemPosCountVec back;
    if (worn)
    {
        result = bot->CanUnequipItem(dest, true);
        if (result != EQUIP_ERR_OK)
        {
            owner->SendEquipError(result, worn, nullptr);
            message = Acore::StringFormat("{} cannot take off what it wears there.", bot->GetName());
            return false;
        }

        // The slot the dragged item leaves is the first place its replacement goes.
        result = AsOwner(owner, worn, [&]
        {
            InventoryResult fit = owner->CanStoreItem(srcBag, srcSlot, back, worn, true);
            if (fit != EQUIP_ERR_OK)
                fit = owner->CanStoreItem(NULL_BAG, NULL_SLOT, back, worn, true);
            return fit;
        });
        if (result != EQUIP_ERR_OK)
        {
            owner->SendEquipError(result, worn, item);
            message = Acore::StringFormat("No room in your bags for {}'s {}.", bot->GetName(),
                worn->GetTemplate()->Name1);
            return false;
        }
    }

    TakeFrom(owner, item);
    if (worn)
        TakeFrom(bot, worn);

    bot->EquipItem(dest, item, true);
    bot->AutoUnequipOffhandIfNeed();
    if (worn)
        GiveTo(owner, worn, back);

    equipped = uint8(dest & 255);
    LOG_INFO("module.animus", "{} gave companion {} {} ({}) for slot {}{}", owner->GetName(), bot->GetName(),
        item->GetTemplate()->Name1, item->GetEntry(), equipped,
        worn ? Acore::StringFormat(", taking back {}", worn->GetTemplate()->Name1) : "");
    return true;
}

std::vector<Item*> Animus::CompanionGear::Detach(Player* bot, std::vector<uint8> const& slots)
{
    std::vector<Item*> items;
    for (uint8 slot : slots)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            // Out of the slot and out of the world, still an object of its own (nothing destroys or deletes it).
            bot->MoveItemFromInventory(INVENTORY_SLOT_BAG_0, slot, true);
            items.push_back(item);
        }
    return items;
}

void Animus::CompanionGear::Reattach(Player* bot, Player* owner, std::vector<Item*> const& items,
    std::vector<uint8>& slots)
{
    slots.clear();
    for (Item* item : items)
    {
        uint16 dest = 0;
        if (bot->CanEquipItem(NULL_SLOT, dest, item, true) == EQUIP_ERR_OK)
        {
            if (Item* generated = bot->GetItemByPos(dest))
                bot->DestroyItem(generated->GetBagSlot(), generated->GetSlot(), true);
            bot->EquipItem(dest, item, true);
            slots.push_back(uint8(dest & 255));
            continue;
        }

        ItemPosCountVec back;
        if (owner && AsOwner(owner, item, [&] { return owner->CanStoreItem(NULL_BAG, NULL_SLOT, back, item, false); })
            == EQUIP_ERR_OK)
        {
            GiveTo(owner, item, back);
            LOG_INFO("module.animus", "Companion {} can no longer wear {}; returned to {}", bot->GetName(),
                item->GetTemplate()->Name1, owner->GetName());
            continue;
        }

        LOG_INFO("module.animus", "Companion {} can no longer wear {} and it could not be returned; it is lost",
            bot->GetName(), item->GetTemplate()->Name1);
        delete item;
    }
    bot->AutoUnequipOffhandIfNeed();
}

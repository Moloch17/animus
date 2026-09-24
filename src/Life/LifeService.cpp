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

#include "LifeService.h"
#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "CellImpl.h"
#include "Corpse.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SeatView.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldActions.h"
#include "WorldBlock.h"
#include <algorithm>
#include <list>
#include <vector>

namespace
{
    using namespace Animus;
    using Animus::Curriculum::WorldActions::INTERACT_YARDS;

    constexpr uint32 MAIL_EVERY_MS = 30 * IN_MILLISECONDS;
    constexpr uint32 AUCTION_EVERY_MS = 60 * IN_MILLISECONDS;
    constexpr uint32 CRAFT_EVERY_MS = 15 * IN_MILLISECONDS;
    constexpr uint32 TAXI_EVERY_MS = 10 * IN_MILLISECONDS;
    constexpr uint32 CORPSE_GIVE_UP_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr float AUCTIONEER_REACH = 10.0f;
    /// Listings run twelve hours; a green is listed at four times its vendor price when the house has no price.
    constexpr uint32 LISTING_HOURS = 12;
    constexpr uint32 VENDOR_MULTIPLE = 4;
    /// What an upgrade at the house has to gain over what is worn (GearScore).
    constexpr float UPGRADE_GAIN = 5.0f;

    template <typename Visit>
    void ForEachBagItem(Player* bot, Visit visit)
    {
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                visit(item);
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = bot->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    if (Item* item = bag->GetItemByPos(uint8(slot)))
                        visit(item);
    }

    /// The median buyout the house asks for `entry`, per item, or 0 when nobody sells it.
    uint32 HousePrice(AuctionHouseObject* house, uint32 entry)
    {
        std::vector<uint32> prices;
        for (auto const& [id, auction] : house->GetAuctions())
            if (auction->item_template == entry && auction->buyout && auction->itemCount)
                prices.push_back(auction->buyout / auction->itemCount);
        if (prices.empty())
            return 0;
        std::sort(prices.begin(), prices.end());
        return prices[prices.size() / 2];
    }
}

Animus::Life::LifeService* Animus::Life::LifeService::Instance()
{
    static LifeService instance;
    return &instance;
}

bool Animus::Life::LifeService::Toggle(std::string_view feature, bool on)
{
    if (feature == "all")
        _settings.Enable = on;
    else if (feature == "quests")
        _settings.Quests = on;
    else if (feature == "auction")
        _settings.Auction = on;
    else if (feature == "mail")
        _settings.Mail = on;
    else if (feature == "taxi")
        _settings.Taxi = on;
    else if (feature == "corpse")
        _settings.CorpseRun = on;
    else if (feature == "crafting")
        _settings.Crafting = on;
    else
        return false;
    return true;
}

std::vector<std::string> Animus::Life::LifeService::Status() const
{
    auto const onOff = [](bool value) { return value ? "on" : "off"; };
    return {
        Acore::StringFormat("life: {} (the model's world block decides looting, quests, gear, vendors)",
            onOff(_settings.Enable)),
        Acore::StringFormat("quests {}, auction {} (budget {} gold), mail {}, taxi {} (beyond {:.0f} yd), corpse run "
            "{}, crafting {}", onOff(_settings.Quests), onOff(_settings.Auction), _settings.AuctionBudgetCopper / 10000,
            onOff(_settings.Mail), onOff(_settings.Taxi), _settings.TaxiBeyondYards, onOff(_settings.CorpseRun),
            onOff(_settings.Crafting)),
    };
}

Animus::Life::LifeService::BotState& Animus::Life::LifeService::StateOf(Player const* bot)
{
    return _bots[bot->GetGUID()];
}

void Animus::Life::LifeService::Sense(Player* bot, Curriculum::WorldView& world) const
{
    // What the sim's life encounters sensed for a seat, from the real world: no phasing, everybody's NPCs.
    Curriculum::WorldActions::Sense(bot, Curriculum::WorldBlock::RANGE, world);

    // The quest the block reports is the one the companion holds that is furthest along: a complete one first,
    // else the most progressed active one. The sim had one quest an episode; a companion carries the owner's log.
    world.QuestState = Curriculum::WorldView::QUEST_NONE;
    world.QuestProgress = 0.0f;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 const questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        QuestStatus const status = bot->GetQuestStatus(questId);
        if (status == QUEST_STATUS_COMPLETE)
        {
            world.QuestState = Curriculum::WorldView::QUEST_COMPLETE;
            world.QuestProgress = 1.0f;
            break;
        }
        if (status == QUEST_STATUS_INCOMPLETE)
        {
            float const progress = Curriculum::WorldActions::QuestProgress(bot, questId);
            if (world.QuestState == Curriculum::WorldView::QUEST_NONE || progress > world.QuestProgress)
            {
                world.QuestState = Curriculum::WorldView::QUEST_ACTIVE;
                world.QuestProgress = progress;
            }
        }
    }
}

void Animus::Life::LifeService::OnOwnerQuestAccept(Player* owner, Quest const* quest,
    std::vector<Player*> const& bots) const
{
    if (!_settings.Enable || !_settings.Quests || !quest)
        return;

    for (Player* bot : bots)
    {
        if (!bot || !bot->IsInWorld() || bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE)
            continue;
        if (!bot->CanTakeQuest(quest, false) || !bot->CanAddQuest(quest, false))
            continue;
        // The owner is the giver as far as the companion is concerned: a shared quest.
        bot->AddQuestAndCheckCompletion(quest, owner);
    }
}

void Animus::Life::LifeService::OnOwnerQuestAbandon(Player* /*owner*/, uint32 questId,
    std::vector<Player*> const& bots) const
{
    if (!_settings.Enable || !_settings.Quests)
        return;

    for (Player* bot : bots)
    {
        if (!bot || !bot->IsInWorld() || bot->GetQuestStatus(questId) == QUEST_STATUS_NONE
            || bot->GetQuestStatus(questId) == QUEST_STATUS_REWARDED)
            continue;
        // As CMSG_QUESTLOG_REMOVE_QUEST does.
        bot->TakeQuestSourceItem(questId, true);
        bot->AbandonQuest(questId);
        bot->RemoveActiveQuest(questId);
    }
}

Creature* Animus::Life::LifeService::NearbyAuctioneer(Player* bot)
{
    std::list<Creature*> creatures;
    Acore::AllWorldObjectsInRange check(bot, AUCTIONEER_REACH);
    Acore::CreatureListSearcher<Acore::AllWorldObjectsInRange> searcher(bot, creatures, check);
    Cell::VisitObjects(bot, searcher, AUCTIONEER_REACH);
    for (Creature* creature : creatures)
        if (creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER)
            && creature->GetReactionTo(bot) > REP_UNFRIENDLY)
            return creature;
    return nullptr;
}

void Animus::Life::LifeService::Update(uint32 diff, Player* owner, std::vector<Companion> const& companions)
{
    if (!_settings.Enable || !owner)
        return;

    for (Companion const& companion : companions)
    {
        Player* bot = companion.Bot;
        if (!bot || !bot->IsInWorld() || !companion.HasWorldBlock)
            continue;

        BotState& state = StateOf(bot);
        state.SinceMailMs += diff;
        state.SinceAuctionMs += diff;
        state.SinceCraftMs += diff;
        state.SinceTaxiMs += diff;
        state.IdleMs = companion.Quiet && bot->IsAlive() ? state.IdleMs + diff : 0;
        if (!bot->IsAlive() || bot->IsInFlight())
            continue;

        // The flight: the owner far off on the same map, and a path between the nearest nodes.
        if (_settings.Taxi && state.SinceTaxiMs >= TAXI_EVERY_MS && companion.Quiet)
        {
            state.SinceTaxiMs = 0;
            if (Fly(bot, owner, state))
                continue;
        }

        bool const idle = state.IdleMs >= _settings.IdleSeconds * IN_MILLISECONDS;
        if (!idle)
            continue;

        if (_settings.Mail && state.SinceMailMs >= MAIL_EVERY_MS)
        {
            state.SinceMailMs = 0;
            CollectMail(bot, state);
        }
        if (_settings.Auction && state.SinceAuctionMs >= AUCTION_EVERY_MS)
        {
            if (Creature* auctioneer = NearbyAuctioneer(bot))
            {
                state.SinceAuctionMs = 0;
                Auction(bot, auctioneer, companion.Stats, state);
            }
        }
        if (_settings.Crafting && state.SinceCraftMs >= CRAFT_EVERY_MS && !bot->IsNonMeleeSpellCast(false))
        {
            state.SinceCraftMs = 0;
            Craft(bot, state);
        }
    }
}

void Animus::Life::LifeService::CollectMail(Player* bot, BotState& state) const
{
    // HandleMailTakeMoney and HandleMailTakeItem without the packets and without COD (nothing that costs is sent
    // to a companion: players cannot mail one, and the house's mail is free). Read mail with nothing left in it is
    // marked deleted; the save removes it.
    time_t const now = GameTime::GetGameTime().count();
    for (Mail* mail : bot->GetMails())
    {
        if (!mail || mail->state == MAIL_STATE_DELETED || mail->deliver_time > now || mail->COD)
            continue;

        bool changed = false;
        if (mail->money)
        {
            bot->ModifyMoney(int32(mail->money));
            mail->money = 0;
            changed = true;
        }

        std::vector<MailItemInfo> const items = mail->items;
        for (MailItemInfo const& info : items)
        {
            Item* item = bot->GetMItem(info.item_guid);
            if (!item)
                continue;
            ItemPosCountVec dest;
            if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                continue;
            mail->RemoveItem(info.item_guid);
            bot->RemoveMItem(item->GetGUID().GetCounter());
            item->SetState(ITEM_UNCHANGED);
            bot->MoveItemToInventory(dest, item, true);
            changed = true;
        }

        if (changed)
        {
            mail->state = mail->HasItems() || mail->money ? MAIL_STATE_CHANGED : MAIL_STATE_DELETED;
            bot->m_mailsUpdated = true;
            ++state.MailsTaken;
        }
    }
}

void Animus::Life::LifeService::Auction(Player* bot, Creature* auctioneer, Curriculum::StatProfile stats,
    BotState& state) const
{
    AuctionHouseEntry const* houseEntry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!houseEntry || !house)
        return;

    // Buying first: an upgrade for the build under the budget, at its buyout, as the buyout branch of
    // HandleAuctionPlaceBid does. One a visit.
    uint32 const budget = std::min<uint32>(_settings.AuctionBudgetCopper, bot->GetMoney());
    AuctionEntry* best = nullptr;
    float bestGain = UPGRADE_GAIN;
    for (auto const& [id, auction] : house->GetAuctions())
    {
        if (!auction->buyout || auction->buyout > budget || auction->owner == bot->GetGUID()
            || auction->itemCount != 1)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!proto || !Curriculum::WorldActions::CanWear(bot, proto))
            continue;
        uint8 const slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot == NULL_SLOT)
            continue;
        Item const* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        float const gain = Curriculum::WorldActions::GearScore(proto, stats)
            - (worn ? Curriculum::WorldActions::GearScore(worn->GetTemplate(), stats) : 0.0f);
        if (gain > bestGain)
        {
            bestGain = gain;
            best = auction;
        }
    }
    if (best)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        bot->ModifyMoney(-int32(best->buyout));
        if (best->bidder)
            sAuctionMgr->SendAuctionOutbiddedMail(best, best->buyout, bot, trans);
        best->bidder = bot->GetGUID();
        best->bid = best->buyout;
        sAuctionMgr->SendAuctionSalePendingMail(best, trans);
        sAuctionMgr->SendAuctionSuccessfulMail(best, trans);
        sAuctionMgr->SendAuctionWonMail(best, trans);
        best->DeleteFromDB(trans);
        sAuctionMgr->RemoveAItem(best->item_guid);
        house->RemoveAuction(best);
        CharacterDatabase.CommitTransaction(trans);
        ++state.Bought;
        LOG_INFO("module.animus", "Companion {} bought an upgrade at the auction house", bot->GetName());
    }

    // Selling: greens and better that are not bound, not an upgrade for the build and not a quest's, at the
    // house's median price for the item, else four times what a vendor pays. HandleAuctionSellItem's body for a
    // whole stack. A few a visit.
    std::vector<Item*> toList;
    ForEachBagItem(bot, [&](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (proto->Quality < ITEM_QUALITY_UNCOMMON || item->IsSoulBound() || item->IsBoundAccountWide()
            || proto->SellPrice == 0 || proto->Class == ITEM_CLASS_QUEST || item->IsNotEmptyBag()
            || bot->HasQuestForItem(proto->ItemId))
            return;
        if (Curriculum::WorldActions::CanWear(bot, proto))
        {
            uint8 const slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
            Item const* worn = slot != NULL_SLOT ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) : nullptr;
            if (!worn || Curriculum::WorldActions::GearScore(proto, stats)
                > Curriculum::WorldActions::GearScore(worn->GetTemplate(), stats))
                return;
        }
        if (toList.size() < 4)
            toList.push_back(item);
    });

    uint32 const etime = LISTING_HOURS * HOUR;
    for (Item* item : toList)
    {
        uint32 const perItem = std::max<uint32>(HousePrice(house, item->GetEntry()),
            item->GetTemplate()->SellPrice * VENDOR_MULTIPLE);
        uint32 const buyout = perItem * item->GetCount();
        uint32 const bid = std::max<uint32>(1, buyout * 3 / 4);
        uint32 const deposit = sAuctionMgr->GetAuctionDeposit(houseEntry, etime, item, item->GetCount());
        if (!bot->HasEnoughMoney(deposit))
            break;
        bot->ModifyMoney(-int32(deposit));

        AuctionEntry* AH = new AuctionEntry;
        AH->Id = sObjectMgr->GenerateAuctionID();
        AH->houseId = sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION)
            ? AuctionHouseId::Neutral : AuctionHouseId(houseEntry->houseId);
        AH->item_guid = item->GetGUID();
        AH->item_template = item->GetEntry();
        AH->itemCount = item->GetCount();
        AH->owner = bot->GetGUID();
        AH->startbid = bid;
        AH->bidder = ObjectGuid::Empty;
        AH->bid = 0;
        AH->buyout = buyout;
        AH->expire_time = GameTime::GetGameTime().count() + uint32(etime * sWorld->getRate(RATE_AUCTION_TIME));
        AH->deposit = deposit;
        AH->auctionHouseEntry = houseEntry;

        sAuctionMgr->AddAItem(item);
        house->AddAuction(AH);
        bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->DeleteFromInventoryDB(trans);
        item->SaveToDB(trans);
        AH->SaveToDB(trans);
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        ++state.Listed;
    }
}

bool Animus::Life::LifeService::Fly(Player* bot, Player* owner, BotState& state) const
{
    if (!owner->IsInWorld() || owner->GetMap() != bot->GetMap() || owner->IsInFlight() || bot->IsInCombat())
        return false;
    float const apart = bot->GetExactDist2d(owner);
    if (apart < _settings.TaxiBeyondYards)
        return false;

    // The nearest node to each; the flight is worth it when the far node is well nearer the owner than the bot is.
    uint32 const mapId = bot->GetMapId();
    uint32 const source = sObjectMgr->GetNearestTaxiNode(bot->GetPositionX(), bot->GetPositionY(),
        bot->GetPositionZ(), mapId, bot->GetTeamId());
    uint32 const destination = sObjectMgr->GetNearestTaxiNode(owner->GetPositionX(), owner->GetPositionY(),
        owner->GetPositionZ(), mapId, bot->GetTeamId());
    if (!source || !destination || source == destination)
        return false;
    TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(destination);
    if (!node || owner->GetExactDist2d(node->x, node->y) > apart / 2.0f)
        return false;

    uint32 path = 0;
    uint32 cost = 0;
    sObjectMgr->GetTaxiPath(source, destination, path, cost);
    if (!path || !bot->HasEnoughMoney(cost))
        return false;

    // A companion knows every flight point its owner would: the two it uses, at least.
    bot->m_taxi.SetTaximaskNode(source);
    bot->m_taxi.SetTaximaskNode(destination);
    if (!bot->ActivateTaxiPathTo({ source, destination }))
        return false;

    ++state.Flights;
    LOG_INFO("module.animus", "Companion {} takes a flight toward its owner ({:.0f} yd away)", bot->GetName(), apart);
    return true;
}

void Animus::Life::LifeService::Craft(Player* bot, BotState& state) const
{
    // A recipe the companion knows whose reagents it carries, cast once: what it gathered becomes what it can use.
    for (auto const& [spellId, spell] : bot->GetSpellMap())
    {
        if (!spell || spell->State == PLAYERSPELL_REMOVED || !spell->Active)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || info->IsPassive())
            continue;

        uint32 created = 0;
        for (SpellEffectInfo const& effect : info->Effects)
            if (effect.Effect == SPELL_EFFECT_CREATE_ITEM)
                created = effect.ItemType;
        if (!created)
            continue;

        bool reagents = false;
        bool have = true;
        for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        {
            if (info->Reagent[i] <= 0 || !info->ReagentCount[i])
                continue;
            reagents = true;
            if (!bot->HasItemCount(uint32(info->Reagent[i]), info->ReagentCount[i]))
                have = false;
        }
        if (!reagents || !have || bot->GetItemCount(created) >= 20)
            continue;

        SpellCastTargets targets;
        targets.SetUnitTarget(bot);
        Spell* cast = new Spell(bot, info, TRIGGERED_NONE);
        if (cast->prepare(&targets) == SPELL_CAST_OK)
        {
            ++state.Crafted;
            return;
        }
    }
}

bool Animus::Life::LifeService::RunToCorpse(uint32 diff, Player* bot, Player* owner, bool hasWorldBlock)
{
    if (!_settings.Enable || !_settings.CorpseRun || !hasWorldBlock || !bot || bot->IsAlive())
        return false;

    BotState& state = StateOf(bot);
    // Release, as CMSG_REPOP_REQUEST does, once the owner is alive and out of the fight that killed the companion.
    if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
    {
        if (!owner || !owner->IsAlive() || owner->IsInCombat())
            return true;
        bot->BuildPlayerRepop();
        bot->RepopAtGraveyard();
        state.GhostMs = 0;
        state.Released = true;
        return true;
    }

    state.GhostMs += diff;
    Corpse* corpse = bot->GetCorpse();
    // No corpse to walk to, or one it cannot reach in time: the spirit healer's terms, as a player would take.
    if (!corpse || corpse->GetMapId() != bot->GetMapId() || state.GhostMs >= CORPSE_GIVE_UP_MS)
    {
        bot->ResurrectPlayer(0.5f, true);
        bot->SpawnCorpseBones();
        state.Released = false;
        return true;
    }

    if (!bot->IsWithinDistInMap(corpse, CORPSE_RECLAIM_RADIUS))
    {
        if (bot->movespline->Finalized() || bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
            bot->GetMotionMaster()->MovePoint(0, corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());
        return true;
    }

    // At the corpse: reclaim it once the delay has run, as CMSG_RECLAIM_CORPSE does.
    if (corpse->GetGhostTime() + time_t(bot->GetCorpseReclaimDelay(false)) > GameTime::GetGameTime().count())
        return true;
    bot->ResurrectPlayer(0.5f);
    bot->SpawnCorpseBones();
    state.Released = false;
    return true;
}

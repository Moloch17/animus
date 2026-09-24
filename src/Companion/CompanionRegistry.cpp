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

#include "CompanionRegistry.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SRP6.h"
#include "StringFormat.h"
#include "Util.h"
#include "World.h"

namespace
{
    std::string AccountName(ObjectGuid owner)
    {
        return Acore::StringFormat("ANIMUS{}", owner.GetCounter());
    }

    std::string RandomPassword()
    {
        static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::string password;
        for (int i = 0; i < 16; ++i)
            password.push_back(ALPHABET[urand(0, sizeof(ALPHABET) - 2)]);
        return password;
    }
}

void Animus::CompanionRegistry::Load()
{
    _records.clear();
    _byBot.clear();

    QueryResult result = CharacterDatabase.Query("SELECT owner, account, guid, spec, edited, owner_gear "
        "FROM animus_companion");
    if (!result)
    {
        LOG_INFO("module.animus", "Animus knows no companions yet");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        Record record;
        record.Owner = ObjectGuid::Create<HighGuid::Player>(fields[0].Get<uint32>());
        record.Account = fields[1].Get<uint32>();
        record.Bot = ObjectGuid::Create<HighGuid::Player>(fields[2].Get<uint32>());
        record.Spec = fields[3].Get<uint8>();
        record.Edited = fields[4].Get<uint8>() != 0;
        record.OwnerGear = fields[5].Get<uint32>();

        // A companion whose character was deleted under it (by hand) is forgotten.
        if (!sCharacterCache->GetCharacterCacheByGuid(record.Bot))
        {
            LOG_WARN("module.animus", "Companion {} of {} has no character any more; forgetting it",
                record.Bot.ToString(), record.Owner.ToString());
            CharacterDatabase.DirectExecute("DELETE FROM animus_companion WHERE owner = {}",
                record.Owner.GetCounter());
            continue;
        }

        _byBot[record.Bot] = record.Owner;
        _records[record.Owner] = record;
    } while (result->NextRow());

    LOG_INFO("module.animus", "Animus knows {} companions", _records.size());
}

Animus::CompanionRegistry::Record* Animus::CompanionRegistry::Find(ObjectGuid owner)
{
    auto const itr = _records.find(owner);
    return itr == _records.end() ? nullptr : &itr->second;
}

Animus::CompanionRegistry::Record const* Animus::CompanionRegistry::Find(ObjectGuid owner) const
{
    auto const itr = _records.find(owner);
    return itr == _records.end() ? nullptr : &itr->second;
}

Animus::CompanionRegistry::Record const* Animus::CompanionRegistry::FindByBot(ObjectGuid bot) const
{
    auto const itr = _byBot.find(bot);
    return itr == _byBot.end() ? nullptr : Find(itr->second);
}

uint32 Animus::CompanionRegistry::AccountFor(Player const* owner) const
{
    std::string username = AccountName(owner->GetGUID());
    if (uint32 const existing = AccountMgr::GetId(username))
        return existing;

    // AccountMgr::CreateAccount inserts asynchronously and the id is needed now: the same rows, written directly.
    // The password is random and never shown; the account exists only to own the companion character.
    Utf8ToUpperOnlyLatin(username);
    auto const [salt, verifier] = Acore::Crypto::SRP6::MakeRegistrationData(username, RandomPassword());
    LoginDatabase.DirectExecute("INSERT INTO account (username, salt, verifier, expansion, joindate) "
        "VALUES ('{}', 0x{}, 0x{}, {}, NOW())", username, ByteArrayToHexStr(salt), ByteArrayToHexStr(verifier),
        sWorld->getIntConfig(CONFIG_EXPANSION));
    LoginDatabase.DirectExecute("INSERT INTO realmcharacters (realmid, acctid, numchars) SELECT realmlist.id, "
        "account.id, 0 FROM realmlist, account LEFT JOIN realmcharacters ON acctid = account.id WHERE acctid IS NULL");

    uint32 const account = AccountMgr::GetId(username);
    if (account)
        LOG_INFO("module.animus", "Created account {} ({}) for {}'s companion", username, account, owner->GetName());
    else
        LOG_ERROR("module.animus", "Could not create account {} for {}'s companion", username, owner->GetName());
    return account;
}

Animus::CompanionRegistry::Record& Animus::CompanionRegistry::Insert(ObjectGuid owner, uint32 account,
    ObjectGuid bot, uint8 spec)
{
    Record& record = _records[owner];
    record = Record{};
    record.Owner = owner;
    record.Account = account;
    record.Bot = bot;
    record.Spec = spec;
    _byBot[bot] = owner;

    CharacterDatabase.DirectExecute("REPLACE INTO animus_companion (owner, account, guid, spec, edited, owner_gear) "
        "VALUES ({}, {}, {}, {}, 0, 0)", owner.GetCounter(), account, bot.GetCounter(), spec);
    return record;
}

void Animus::CompanionRegistry::Update(Record const& record) const
{
    CharacterDatabase.DirectExecute("UPDATE animus_companion SET spec = {}, edited = {}, owner_gear = {} "
        "WHERE owner = {}", record.Spec, record.Edited ? 1 : 0, record.OwnerGear, record.Owner.GetCounter());
}

void Animus::CompanionRegistry::Erase(ObjectGuid owner, bool andAccount)
{
    auto const itr = _records.find(owner);
    if (itr == _records.end())
        return;

    uint32 const account = itr->second.Account;
    _byBot.erase(itr->second.Bot);
    _records.erase(itr);
    CharacterDatabase.DirectExecute("DELETE FROM animus_companion WHERE owner = {}",
        owner.GetCounter());

    if (andAccount)
        AccountMgr::DeleteAccount(account);
}

bool Animus::CompanionRegistry::CheckName(std::string& name, std::string& message)
{
    if (!normalizePlayerName(name))
    {
        message = "That is not a name.";
        return false;
    }

    if (ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS)
    {
        message = "That name is not allowed: two to twelve letters, no digits or spaces.";
        return false;
    }

    if (sObjectMgr->IsReservedName(name) || sObjectMgr->IsProfanityName(name))
    {
        message = "That name is reserved.";
        return false;
    }

    if (sCharacterCache->GetCharacterCacheByName(name))
    {
        message = "That name is taken.";
        return false;
    }

    return true;
}

void Animus::CompanionRegistry::Rename(Record const& record, std::string const& name)
{
    // CHAR_UPD_NAME_BY_GUID is asynchronous only, and the character must be renamed before it can load again.
    std::string escaped = name;
    CharacterDatabase.EscapeString(escaped);
    CharacterDatabase.DirectExecute("UPDATE characters SET name = '{}' WHERE guid = {}", escaped,
        record.Bot.GetCounter());
    sCharacterCache->UpdateCharacterData(record.Bot, name);
}

void Animus::CompanionRegistry::Purge(ObjectGuid bot)
{
    // Items sitting in its mail go with the mail (a loaded character's gear that no longer fits is mailed to it).
    uint32 const guid = bot.GetCounter();
    CharacterDatabase.DirectExecute("DELETE ii FROM item_instance ii INNER JOIN mail_items mi ON "
        "mi.item_guid = ii.guid INNER JOIN mail m ON m.id = mi.mail_id WHERE m.receiver = {}", guid);
    CharacterDatabase.DirectExecute("DELETE mi FROM mail_items mi INNER JOIN mail m ON "
        "m.id = mi.mail_id WHERE m.receiver = {}", guid);
    CharacterDatabase.DirectExecute("DELETE FROM mail WHERE receiver = {}", guid);
    CharacterDatabase.DirectExecute("DELETE FROM character_achievement WHERE guid = {}",
        guid);
    CharacterDatabase.DirectExecute("DELETE FROM character_achievement_progress WHERE guid = {}",
        guid);
}

void Animus::CompanionRegistry::DeleteCharacter(Record const& record)
{
    Player::DeleteFromDB(record.Bot.GetCounter(), record.Account, false, true);
}

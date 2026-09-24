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

#ifndef ANIMUS_COMPANION_REGISTRY_H
#define ANIMUS_COMPANION_REGISTRY_H

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <string_view>
#include <unordered_map>

class Player;

namespace Animus
{
    /// What a player character owns: one companion, a character of its own on an account made for it, remembered
    /// in `animus_companion` (characters database) between summons. The character itself is the core's (its
    /// `characters` row and everything hanging off it); this is the map from owner to it.
    class CompanionRegistry
    {
    public:
        struct Record
        {
            ObjectGuid Owner;
            uint32 Account = 0;
            ObjectGuid Bot;
            uint8 Spec = 0;
            bool Edited = false;
            uint32 OwnerGear = 0;               // bit per equipment slot
            bool Loading = false;               // a summon is loading it (not saved)
        };

        void Load();

        [[nodiscard]] Record* Find(ObjectGuid owner);
        [[nodiscard]] Record const* Find(ObjectGuid owner) const;
        [[nodiscard]] Record const* FindByBot(ObjectGuid bot) const;
        [[nodiscard]] bool IsCompanion(ObjectGuid guid) const { return _byBot.count(guid) != 0; }

        /// The account the owner's companion characters live on, made on first use: `ANIMUS<owner guid>`, with a
        /// password nobody is told. 0 when the login database refused.
        [[nodiscard]] uint32 AccountFor(Player const* owner) const;

        /// A companion character `bot` was created for `owner` (and saved): remember it.
        Record& Insert(ObjectGuid owner, uint32 account, ObjectGuid bot, uint8 spec);
        /// Write the fields the party changes (spec stays; edited, owner gear).
        void Update(Record const& record) const;
        /// The companion character is gone from the characters database: forget it. The account stays for the
        /// next one unless `andAccount`.
        void Erase(ObjectGuid owner, bool andAccount);
        /// Every record gone from memory, and the module's table dropped and made again empty (the accounts and
        /// characters are the caller's to delete).
        void Clear();

        /// A name the owner may give a companion: the client's rules, not reserved, not taken. False with
        /// `message` set.
        static bool CheckName(std::string& name, std::string& message);

        /// Rename the (offline) companion character in the database and the character cache.
        static void Rename(Record const& record, std::string const& name);

        /// The companion character's mail and achievements, gone: it gets neither. Run before it loads and after it
        /// saves.
        static void Purge(ObjectGuid bot);

        /// Delete the companion character from the characters database (offline), finally.
        static void DeleteCharacter(Record const& record);

    private:
        std::unordered_map<ObjectGuid, Record> _records;    // by owner
        std::unordered_map<ObjectGuid, ObjectGuid> _byBot;  // bot -> owner
    };
}

#endif

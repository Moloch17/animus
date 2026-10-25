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

#include "AnimusPlayerSettings.h"
#include "QueryResult.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "StringFormat.h"
#include <unordered_map>

namespace
{
    struct Settings
    {
        bool PartyFill = true;
    };

    std::unordered_map<ObjectGuid, Settings> g_settings;
    bool g_tableKnown = false;

    bool TableExists()
    {
        return CharacterDatabase.Query("SHOW TABLES LIKE 'animus_player_settings'") != nullptr;
    }

    /// Made the first time a setting is written: nothing of it is in the database before then.
    void CreateTable()
    {
        if (g_tableKnown)
            return;
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `animus_player_settings` ("
            "`guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid', "
            "`party_fill` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1: fill my dungeon party with bots', "
            "`updated` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, "
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
        g_tableKnown = true;
    }
}

void Animus::PlayerSettings::Load(ObjectGuid guid)
{
    Settings& settings = g_settings[guid];
    settings = Settings();
    if (!g_tableKnown && !TableExists())
        return;
    g_tableKnown = true;
    QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
        "SELECT party_fill FROM animus_player_settings WHERE guid = {}", guid.GetCounter()));
    if (!result)
        return;
    settings.PartyFill = result->Fetch()[0].Get<uint8>() != 0;
}

void Animus::PlayerSettings::Forget(ObjectGuid guid)
{
    g_settings.erase(guid);
}

bool Animus::PlayerSettings::PartyFill(ObjectGuid guid)
{
    auto const found = g_settings.find(guid);
    return found == g_settings.end() ? true : found->second.PartyFill;
}

void Animus::PlayerSettings::SetPartyFill(ObjectGuid guid, bool on)
{
    g_settings[guid].PartyFill = on;
    CreateTable();
    CharacterDatabase.DirectExecute(Acore::StringFormat(
        "REPLACE INTO animus_player_settings (guid, party_fill) VALUES ({}, {})", guid.GetCounter(), on ? 1 : 0));
}

void Animus::PlayerSettings::DropTable()
{
    CharacterDatabase.DirectExecute("DROP TABLE IF EXISTS animus_player_settings");
    g_tableKnown = false;
    g_settings.clear();
}

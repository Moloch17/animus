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

/*
 * Bot lifecycle on a live realm, without a client or a character row. Adapted from
 * mod-animus-forge/src/Bot/BotFactory.cpp.
 *
 * Mirrors the create path (CharacterHandler: new Player -> MotionMaster::Initialize -> Create) and
 * the in-world half of the login path (HandlePlayerLoginFromDB: SetPlayer -> SetMover ->
 * ObjectAccessor::AddObject -> Map::AddPlayerToMap), skipping every step that reads or writes the
 * character database and every step whose only effect is a packet to the bot's own client.
 *
 * Unlike the sim, real players can see and interact with the bot, so it also gets:
 *   - an empty social list:   whispers and invites read the receiver's PlayerSocial unchecked
 *   - a character cache entry: CMSG_NAME_QUERY answers from the cache, else clients show "Unknown"
 */

#include "BotFactory.h"
#include "CharacterCache.h"
#include "GameTime.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SocialMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

namespace
{
    /// Bot accounts live far above anything a real realm allocates.
    constexpr uint32 BOT_ACCOUNT_BASE = 0x7E000000;

    /// Distance from the owner the bot appears at, and the angle relative to the owner's facing.
    constexpr float SPAWN_DISTANCE = 2.0f;
    constexpr float SPAWN_ANGLE = float(M_PI) / 2;

    /// Bots created this run. Names carry the number: real character names cannot contain digits,
    /// so a bot never collides with a player in ObjectAccessor's name map.
    uint32 BotCounter = 0;

    /// Player has no setter for m_social; LoadFromDB assigns it from the login query. Explicit
    /// instantiation ignores access checks, so this names the private member without a core change.
    template <typename Tag, typename Tag::Type Member>
    struct PrivateMember
    {
        friend typename Tag::Type Access(Tag) { return Member; }
    };

    struct PlayerSocialTag
    {
        using Type = PlayerSocial* Player::*;
        friend Type Access(PlayerSocialTag);
    };

    template struct PrivateMember<PlayerSocialTag, &Player::m_social>;

    /// CharacterCreateInfo keeps its fields protected (it is filled from CMSG_CHAR_CREATE); a
    /// derived type may set them.
    class BotCreateInfo : public CharacterCreateInfo
    {
    public:
        BotCreateInfo(Animus::BotFactory::BotSpec const& spec, std::string const& name)
        {
            Name = name;
            Race = spec.Race;
            Class = spec.Class;
            Gender = spec.Gender;
        }
    };
}

Player* Animus::BotFactory::Create(BotSpec const& spec)
{
    uint32 const number = ++BotCounter;
    std::string const name = Acore::StringFormat("Animus{}", number);
    uint32 const accountId = BOT_ACCOUNT_BASE + number;

    // accountFlags 0: no collector's edition voucher mail (Player::Create's only DB write).
    WorldSession* session = new WorldSession(accountId, std::string(name), 0, nullptr, SEC_PLAYER,
        EXPANSION_WRATH_OF_THE_LICH_KING, 0, LOCALE_enUS, 0, false, false, 0);

    // Default permissions for the security level, in memory. Must precede new Player, whose
    // constructor checks a permission and would otherwise run a sync login DB query.
    session->InitRBACDataForTest();

    Player* bot = new Player(session);
    bot->GetMotionMaster()->Initialize();

    BotCreateInfo info(spec, name);
    if (!bot->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &info))
    {
        LOG_ERROR("module.animus", "Player::Create failed for bot {} (race {}, class {})", name, spec.Race,
            spec.Class);
        delete bot;
        delete session;
        return nullptr;
    }

    sInstanceSaveMgr->PlayerCreateBoundInstancesMaps(bot->GetGUID());
    session->SetPlayer(bot);

    // Never save: 0 disables the autosave countdown in Player::Update.
    bot->SetSaveTimer(0);

    // SetLevel, not GiveLevel: GiveLevel sends level-reward mail.
    if (spec.Level && bot->GetLevel() != spec.Level)
    {
        bot->SetLevel(spec.Level, false);
        bot->InitStatsForLevel(true);
    }

    bot->SetCanModifyStats(true);
    bot->UpdateAllStats();
    bot->SetFullHealth();

    // A null result gives an empty list. LogoutPlayer removes it again.
    bot->*Access(PlayerSocialTag{}) = sSocialMgr->LoadFromDB(nullptr, bot->GetGUID());
    sCharacterCache->AddCharacterCacheEntry(bot->GetGUID(), accountId, name, spec.Gender, spec.Race, spec.Class,
        bot->GetLevel());

    return bot;
}

bool Animus::BotFactory::PlaceNear(Player* bot, Player* owner)
{
    Map* map = owner->GetMap();
    if (map->Instanceable())
    {
        LOG_ERROR("module.animus", "Bot {} cannot be placed in instanced map {}", bot->GetName(), map->GetId());
        Discard(bot);
        return false;
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    owner->GetClosePoint(x, y, z, owner->GetCombatReach(), SPAWN_DISTANCE, SPAWN_ANGLE);

    // Player::Create parked the bot on its race's start continent; move it before entering.
    bot->ResetMap();
    bot->Relocate(x, y, z, owner->GetOrientation());
    bot->SetMap(map);
    bot->SetPhaseMask(owner->GetPhaseMask(), false);
    bot->SetFallInformation(GameTime::GetGameTime().count(), z);
    bot->SetMover(bot);

    ObjectAccessor::AddObject(bot);

    if (!map->AddPlayerToMap(bot))
    {
        LOG_ERROR("module.animus", "Could not add bot {} to map {}", bot->GetName(), map->GetId());
        ObjectAccessor::RemoveObject(bot);
        bot->ResetMap();
        Discard(bot);
        return false;
    }

    return true;
}

void Animus::BotFactory::Destroy(Player* bot)
{
    WorldSession* session = bot->GetSession();

    // A dead bot would be repopped at a graveyard (a far teleport) by LogoutPlayer.
    if (!bot->IsAlive())
        bot->ResurrectPlayer(1.0f);

    sCharacterCache->DeleteCharacterCacheEntry(bot->GetGUID(), bot->GetName());

    // Removes the player from its map, drops its social list and deletes it; false = no SaveToDB.
    session->LogoutPlayer(false);

    delete session;
}

void Animus::BotFactory::Discard(Player* bot)
{
    WorldSession* session = bot->GetSession();

    // ~Unit asserts that every aura (passives included) is gone; this is what removing it from a map would have done.
    // A failed placement may have set the map already.
    bot->CleanupsBeforeDelete();
    if (bot->FindMap())
        bot->ResetMap();

    sCharacterCache->DeleteCharacterCacheEntry(bot->GetGUID(), bot->GetName());
    sSocialMgr->RemovePlayerSocial(bot->GetGUID());

    session->SetPlayer(nullptr);
    delete bot;
    delete session;
}

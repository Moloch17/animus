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

#include "CompanionLoader.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MotionMaster.h"
#include "Player.h"
#include "QueryHolder.h"
#include "SocialMgr.h"
#include "World.h"
#include "WorldSession.h"
#include <chrono>
#include <future>
#include <memory>

namespace
{
    /// Player::LoadFromDB reads a holder filled exactly as the core's LoginQueryHolder::Initialize fills it
    /// (src/server/game/Handlers/CharacterHandler.cpp, 35 queries for MAX_PLAYER_LOGIN_QUERY). That class is local
    /// to its file, so this is a copy: when the core adds a login query, add it here or LoadFromDB reads nothing
    /// for it.
    class CompanionLoginHolder : public CharacterDatabaseQueryHolder
    {
    public:
        CompanionLoginHolder(uint32 account, ObjectGuid guid) : _account(account), _guid(guid) { }

        [[nodiscard]] ObjectGuid GetGuid() const { return _guid; }

        bool Initialize()
        {
            SetSize(MAX_PLAYER_LOGIN_QUERY);

            bool ok = true;
            uint64 const guid = _guid.GetRawValue();
            auto const byGuid = [&](CharacterDatabaseStatements statement, PlayerLoginQueryIndex index)
            {
                CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(statement);
                stmt->SetData(0, guid);
                ok &= SetPreparedQuery(index, stmt);
            };

            byGuid(CHAR_SEL_CHARACTER, PLAYER_LOGIN_QUERY_LOAD_FROM);
            byGuid(CHAR_SEL_CHARACTER_AURAS, PLAYER_LOGIN_QUERY_LOAD_AURAS);
            byGuid(CHAR_SEL_CHARACTER_SPELL, PLAYER_LOGIN_QUERY_LOAD_SPELLS);
            byGuid(CHAR_SEL_CHARACTER_QUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS);
            byGuid(CHAR_SEL_CHARACTER_DAILYQUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_DAILY_QUEST_STATUS);
            byGuid(CHAR_SEL_CHARACTER_WEEKLYQUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_WEEKLY_QUEST_STATUS);
            byGuid(CHAR_SEL_CHARACTER_MONTHLYQUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_MONTHLY_QUEST_STATUS);
            byGuid(CHAR_SEL_CHARACTER_SEASONALQUESTSTATUS, PLAYER_LOGIN_QUERY_LOAD_SEASONAL_QUEST_STATUS);
            byGuid(CHAR_SEL_CHARACTER_REPUTATION, PLAYER_LOGIN_QUERY_LOAD_REPUTATION);
            byGuid(CHAR_SEL_CHARACTER_INVENTORY, PLAYER_LOGIN_QUERY_LOAD_INVENTORY);
            byGuid(CHAR_SEL_CHARACTER_ACTIONS, PLAYER_LOGIN_QUERY_LOAD_ACTIONS);
            byGuid(CHAR_SEL_MAIL, PLAYER_LOGIN_QUERY_LOAD_MAILS);
            byGuid(CHAR_SEL_MAILITEMS, PLAYER_LOGIN_QUERY_LOAD_MAIL_ITEMS);
            byGuid(CHAR_SEL_CHARACTER_SOCIALLIST, PLAYER_LOGIN_QUERY_LOAD_SOCIAL_LIST);
            byGuid(CHAR_SEL_CHARACTER_HOMEBIND, PLAYER_LOGIN_QUERY_LOAD_HOME_BIND);
            byGuid(CHAR_SEL_CHARACTER_SPELLCOOLDOWNS, PLAYER_LOGIN_QUERY_LOAD_SPELL_COOLDOWNS);
            if (sWorld->getBoolConfig(CONFIG_DECLINED_NAMES_USED))
                byGuid(CHAR_SEL_CHARACTER_DECLINEDNAMES, PLAYER_LOGIN_QUERY_LOAD_DECLINED_NAMES);
            byGuid(CHAR_SEL_CHARACTER_ACHIEVEMENTS, PLAYER_LOGIN_QUERY_LOAD_ACHIEVEMENTS);
            byGuid(CHAR_SEL_CHARACTER_CRITERIAPROGRESS, PLAYER_LOGIN_QUERY_LOAD_CRITERIA_PROGRESS);
            byGuid(CHAR_SEL_CHARACTER_EQUIPMENTSETS, PLAYER_LOGIN_QUERY_LOAD_EQUIPMENT_SETS);
            byGuid(CHAR_SEL_CHARACTER_ENTRY_POINT, PLAYER_LOGIN_QUERY_LOAD_ENTRY_POINT);
            byGuid(CHAR_SEL_CHARACTER_GLYPHS, PLAYER_LOGIN_QUERY_LOAD_GLYPHS);
            byGuid(CHAR_SEL_CHARACTER_TALENTS, PLAYER_LOGIN_QUERY_LOAD_TALENTS);
            byGuid(CHAR_SEL_PLAYER_ACCOUNT_DATA, PLAYER_LOGIN_QUERY_LOAD_ACCOUNT_DATA);
            byGuid(CHAR_SEL_CHARACTER_SKILLS, PLAYER_LOGIN_QUERY_LOAD_SKILLS);
            byGuid(CHAR_SEL_CHARACTER_RANDOMBG, PLAYER_LOGIN_QUERY_LOAD_RANDOM_BG);
            byGuid(CHAR_SEL_CHARACTER_BANNED, PLAYER_LOGIN_QUERY_LOAD_BANNED);
            byGuid(CHAR_SEL_CHARACTER_QUESTSTATUSREW, PLAYER_LOGIN_QUERY_LOAD_QUEST_STATUS_REW);
            byGuid(CHAR_SEL_BREW_OF_THE_MONTH, PLAYER_LOGIN_QUERY_LOAD_BREW_OF_THE_MONTH);

            CharacterDatabasePreparedStatement* stmt =
                CharacterDatabase.GetPreparedStatement(CHAR_SEL_ACCOUNT_INSTANCELOCKTIMES);
            stmt->SetData(0, _account);
            ok &= SetPreparedQuery(PLAYER_LOGIN_QUERY_LOAD_INSTANCE_LOCK_TIMES, stmt);

            byGuid(CHAR_SEL_CORPSE_LOCATION, PLAYER_LOGIN_QUERY_LOAD_CORPSE_LOCATION);
            byGuid(CHAR_SEL_CHAR_SETTINGS, PLAYER_LOGIN_QUERY_LOAD_CHARACTER_SETTINGS);
            byGuid(CHAR_SEL_CHAR_PETS, PLAYER_LOGIN_QUERY_LOAD_PET_SLOTS);
            byGuid(CHAR_SEL_CHAR_ACHIEVEMENT_OFFLINE_UPDATES, PLAYER_LOGIN_QUERY_LOAD_OFFLINE_ACHIEVEMENTS_UPDATES);
            return ok;
        }

    private:
        uint32 _account;
        ObjectGuid _guid;
    };

    /// Player::m_social is private and only WorldSession sets it; the explicit instantiation of a template with
    /// a private member pointer as its argument is allowed access (as BotFactory does it).
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

    struct Pending
    {
        ObjectGuid Bot;
        uint32 Account = 0;
        std::string Name;
        Animus::CompanionLoader::Done Done;
        SQLQueryHolderCallback Callback;
    };

    std::vector<Pending> Loads;

    /// What HandlePlayerLoginFromDB does for a client, for a character with no client: the Player from its rows,
    /// its social list, its session. Null (logged, everything freed) when the rows do not load.
    Player* Build(Pending const& load, CompanionLoginHolder const& holder)
    {
        // As BotFactory::Create makes a session: no socket, default permissions in memory.
        auto* session = new WorldSession(load.Account, std::string(load.Name), 0, nullptr, SEC_PLAYER,
            EXPANSION_WRATH_OF_THE_LICH_KING, 0, LOCALE_enUS, 0, false, false, 0);
        session->InitRBACDataForTest();

        Player* bot = new Player(session);
        if (!bot->LoadFromDB(load.Bot, holder))
        {
            LOG_ERROR("module.animus", "Companion {} ({}) could not be loaded from the database", load.Name,
                load.Bot.ToString());
            session->SetPlayer(nullptr);
            delete bot;
            delete session;
            return nullptr;
        }

        bot->GetMotionMaster()->Initialize();
        bot->*Access(PlayerSocialTag{}) = sSocialMgr->LoadFromDB(
            holder.GetPreparedResult(PLAYER_LOGIN_QUERY_LOAD_SOCIAL_LIST), bot->GetGUID());
        session->SetPlayer(bot);

        // The core's own autosave: a crash loses minutes, not the session. Dismissing saves at once.
        bot->SetSaveTimer(sWorld->getIntConfig(CONFIG_INTERVAL_SAVE));
        return bot;
    }
}

void Animus::CompanionLoader::Begin(ObjectGuid bot, uint32 account, std::string const& name, Done done)
{
    auto holder = std::make_shared<CompanionLoginHolder>(account, bot);
    if (!holder->Initialize())
    {
        LOG_ERROR("module.animus", "Could not prepare the load of companion {}", bot.ToString());
        done(nullptr);
        return;
    }

    Loads.push_back({ bot, account, name, std::move(done), CharacterDatabase.DelayQueryHolder(holder) });
}

void Animus::CompanionLoader::Update()
{
    for (std::size_t i = 0; i < Loads.size();)
    {
        Pending& load = Loads[i];
        if (!load.Callback.m_future.valid() || load.Callback.m_future.wait_for(std::chrono::seconds(0))
            != std::future_status::ready)
        {
            ++i;
            continue;
        }

        auto const holder = std::static_pointer_cast<CompanionLoginHolder>(load.Callback.m_holder);
        Player* bot = Build(load, *holder);
        Done const done = std::move(load.Done);
        Loads.erase(Loads.begin() + i);
        done(bot);
    }
}

void Animus::CompanionLoader::Discard(Player* bot)
{
    WorldSession* session = bot->GetSession();

    // As BotFactory::DestroyUnplaced, without forgetting a character that still exists.
    bot->CleanupsBeforeDelete();
    if (bot->FindMap())
        bot->ResetMap();
    sSocialMgr->RemovePlayerSocial(bot->GetGUID());

    session->SetPlayer(nullptr);
    delete bot;
    delete session;
}

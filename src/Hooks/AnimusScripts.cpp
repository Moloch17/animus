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

#include "AnimusMod.h"
#include "AchievementMgr.h"
#include "AchievementScript.h"
#include "Chat.h"
#include "CompanionClient.h"
#include "CommandScript.h"
#include "Optional.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ServerScript.h"
#include "SharedDefines.h"
#include "UnitScript.h"
#include "WorldScript.h"

using namespace Acore::ChatCommands;

namespace
{
    bool Reply(ChatHandler* handler, bool ok, std::string const& message)
    {
        if (ok)
            handler->SendSysMessage(message);
        else
            handler->SendErrorMessage(message, false);

        return ok;
    }

    bool ReplyLines(ChatHandler* handler, std::vector<std::string> const& lines, std::string const& none)
    {
        if (lines.empty())
            return Reply(handler, false, none);

        for (std::string const& line : lines)
            handler->SendSysMessage(line);
        return true;
    }

    class AnimusCommandScript : public CommandScript
    {
    public:
        AnimusCommandScript() : CommandScript("AnimusCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable animusCommandTable =
            {
                { "follow",     HandleFollowCommand,    SEC_GAMEMASTER, Console::No },
                { "dismiss",    HandleDismissCommand,   SEC_GAMEMASTER, Console::No },
                { "models",     HandleModelsCommand,    SEC_GAMEMASTER, Console::Yes },
            };

            static ChatCommandTable commandTable =
            {
                { "animus", animusCommandTable },
            };

            return commandTable;
        }

        /// .animus follow <n>: n (1-4) followers join your party, played by the move4_follow models. With no count:
        /// the followers you have.
        static bool HandleFollowCommand(ChatHandler* handler, Optional<uint32> count)
        {
            Player* caller = handler->GetPlayer();
            if (!count)
                return ReplyLines(handler, sAnimusMod->List(caller), "You have no followers; `.animus follow <1-4>`.");

            std::string message;
            std::vector<std::string> warnings;
            bool const ok = sAnimusMod->Summon(caller, *count, message, warnings);
            for (std::string const& warning : warnings)
                handler->SendSysMessage(warning);
            return Reply(handler, ok, message);
        }

        /// .animus dismiss: your followers leave, unsaved.
        static bool HandleDismissCommand(ChatHandler* handler)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Dismiss(handler->GetPlayer(), message), message);
        }

        /// .animus models: every class's model for the configured stage, whether it loads (and if not, why), and
        /// the camera scenes loaded.
        static bool HandleModelsCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->ModelStatus(), "No classes.");
        }
    };

    class AnimusWorldScript : public WorldScript
    {
    public:
        AnimusWorldScript() : WorldScript("AnimusWorldScript",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

        void OnAfterConfigLoad(bool /*reload*/) override { sAnimusMod->LoadConfig(); }
        void OnStartup() override { sAnimusMod->OnStartup(); }
        /// After the maps have updated (World::Update runs the scripts' update after sMapMgr->Update): the followers
        /// observe the world after the movement, and their keys act at the next map update.
        void OnUpdate(uint32 diff) override { sAnimusMod->OnUpdate(diff); }
        void OnShutdown() override { sAnimusMod->OnShutdown(); }
    };

    class AnimusPlayerScript : public PlayerScript
    {
    public:
        AnimusPlayerScript() : PlayerScript("AnimusPlayerScript",
            { PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_GIVE_MAIL_REWARD_AT_GIVE_LEVEL,
                PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE, PLAYERHOOK_ON_AFTER_UPDATE }) { }

        /// Every player's update, on its map's thread: a follower's client steps its body and reports it, as a real
        /// client's movement is handled on that thread (CompanionClient). Any other player returns at once.
        void OnPlayerAfterUpdate(Player* player, uint32 diff) override
        {
            Animus::Client::Clients::OnPlayerAfterUpdate(player, diff);
        }

        /// A leader logging out takes its followers with it.
        void OnPlayerLogout(Player* player) override { sAnimusMod->OnPlayerLogout(player); }

        /// Followers get no level reward by mail ...
        bool OnPlayerCanGiveMailRewardAtGiveLevel(Player* player, uint8 /*level*/) override
        {
            return !sAnimusMod->IsBot(player->GetGUID());
        }

        /// ... and no achievements (their criteria are not even checked: AnimusAchievementScript).
        bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const* /*achievement*/) override
        {
            return !sAnimusMod->IsBot(player->GetGUID());
        }
    };

    class AnimusAchievementScript : public AchievementScript
    {
    public:
        AnimusAchievementScript() : AchievementScript("AnimusAchievementScript",
            { ACHIEVEMENTHOOK_CAN_CHECK_CRITERIA }) { }

        bool CanCheckCriteria(AchievementMgr* mgr, AchievementCriteriaEntry const* /*criteria*/) override
        {
            return !mgr->GetPlayer() || !sAnimusMod->IsBot(mgr->GetPlayer()->GetGUID());
        }
    };

    class AnimusServerScript : public ServerScript
    {
    public:
        AnimusServerScript() : ServerScript("AnimusServerScript", { SERVERHOOK_ON_PACKET_SENT }) { }

        /// Every packet queued for any session, sockets or not, on the thread that sends it: what the server tells a
        /// follower's client about its own movement goes into that client's inbox (CompanionClient).
        void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
        {
            Animus::Client::Clients::OnPacketSent(session, packet);
        }
    };

    class AnimusUnitScript : public UnitScript
    {
    public:
        AnimusUnitScript() : UnitScript("AnimusUnitScript") { }

        /// Called for every damage event, on map threads, before the victim's AI can change the amount: what a
        /// follower takes reaches its next observation.
        uint32 DealDamage(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType type) override
        {
            sAnimusMod->RecordDamage(attacker, victim, damage, type);
            return damage;
        }
    };
}

void AddSC_animus()
{
    new AnimusCommandScript();
    new AnimusWorldScript();
    new AnimusPlayerScript();
    new AnimusAchievementScript();
    new AnimusUnitScript();
    new AnimusServerScript();
}

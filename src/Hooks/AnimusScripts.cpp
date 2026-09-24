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

#include "AnimusAddon.h"
#include "AnimusMod.h"
#include "AchievementMgr.h"
#include "AchievementScript.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Optional.h"
#include "Player.h"
#include "PlayerScript.h"
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
            static ChatCommandTable stageCommandTable =
            {
                { "list",       HandleStageListCommand,     SEC_GAMEMASTER, Console::No },
            };

            static ChatCommandTable animusCommandTable =
            {
                { "create",     HandleCreateCommand,    SEC_GAMEMASTER, Console::No },
                { "summon",     HandleSummonCommand,    SEC_GAMEMASTER, Console::No },
                { "list",       HandleListCommand,      SEC_GAMEMASTER, Console::No },
                { "dismiss",    HandleDismissCommand,   SEC_GAMEMASTER, Console::No },
                { "rename",     HandleRenameCommand,    SEC_GAMEMASTER, Console::No },
                { "reroll",     HandleRerollCommand,    SEC_GAMEMASTER, Console::No },
                { "purge",      HandlePurgeCommand,     SEC_ADMINISTRATOR, Console::Yes },
                { "life",       HandleLifeCommand,      SEC_GAMEMASTER, Console::Yes },
                { "stage",      stageCommandTable },
            };

            static ChatCommandTable commandTable =
            {
                { "animus", animusCommandTable },
            };

            return commandTable;
        }

        /// .animus create <name> <race> <class>: your one companion character, of that name, race and class.
        static bool HandleCreateCommand(ChatHandler* handler, std::string name, std::string_view race,
            std::string_view playerClass)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Create(handler->GetPlayer(), std::move(name), race, playerClass,
                message), message);
        }

        /// .animus summon: your companion comes to you.
        static bool HandleSummonCommand(ChatHandler* handler)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Summon(handler->GetPlayer(), message), message);
        }

        /// .animus list: your companion and its model.
        static bool HandleListCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->List(handler->GetPlayer()), "You have no companion.");
        }

        /// .animus dismiss: your companion is saved and leaves.
        static bool HandleDismissCommand(ChatHandler* handler)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Dismiss(handler->GetPlayer(), message), message);
        }

        /// .animus rename <name>
        static bool HandleRenameCommand(ChatHandler* handler, std::string name)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Rename(handler->GetPlayer(), std::move(name), message), message);
        }

        /// .animus reroll <race> <class>: a new companion character of the same name.
        static bool HandleRerollCommand(ChatHandler* handler, std::string_view race, std::string_view playerClass)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Reroll(handler->GetPlayer(), race, playerClass, message), message);
        }

        /// .animus purge: every account and character the module made, deleted; every companion sent away unsaved.
        static bool HandlePurgeCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->PurgeAll(), "Nothing to purge.");
        }

        /// .animus life [<feature> on|off]: what the companions do outside the fight, and a switch for each.
        static bool HandleLifeCommand(ChatHandler* handler, Optional<std::string_view> feature,
            Optional<std::string_view> state)
        {
            if (!feature)
                return ReplyLines(handler, sAnimusMod->LifeStatus(), "Life is off.");
            std::string message;
            return Reply(handler, sAnimusMod->LifeToggle(*feature, state.value_or("on"), message), message);
        }

        /// .animus stage list: every curriculum stage and its arenas -- the names Animus.Curriculum.Stage takes.
        static bool HandleStageListCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->StageList(), "There are no stages.");
        }

    };

    class AnimusWorldScript : public WorldScript
    {
    public:
        AnimusWorldScript() : WorldScript("AnimusWorldScript",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

        void OnAfterConfigLoad(bool /*reload*/) override { sAnimusMod->LoadConfig(); }
        void OnStartup() override { sAnimusMod->OnStartup(); }
        void OnUpdate(uint32 diff) override { sAnimusMod->OnUpdate(diff); }
        void OnShutdown() override { sAnimusMod->OnShutdown(); }
    };

    class AnimusPlayerScript : public PlayerScript
    {
    public:
        AnimusPlayerScript() : PlayerScript("AnimusPlayerScript",
            { PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_DELETE,
                PLAYERHOOK_CAN_SEND_MAIL, PLAYERHOOK_CAN_GIVE_MAIL_REWARD_AT_GIVE_LEVEL,
                PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE, PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
                PLAYERHOOK_ON_QUEST_ABANDON }) { }

        void OnPlayerLogout(Player* player) override { sAnimusMod->OnPlayerLogout(player); }

        /// The owner's quests are its companions' too (Animus.Life.Quests): taken with the owner, dropped with it.
        void OnPlayerQuestAccept(Player* player, Quest const* quest) override
        {
            if (!sAnimusMod->IsCompanion(player->GetGUID()))
                sAnimusMod->OnOwnerQuestAccept(player, quest);
        }

        void OnPlayerQuestAbandon(Player* player, uint32 questId) override
        {
            if (!sAnimusMod->IsCompanion(player->GetGUID()))
                sAnimusMod->OnOwnerQuestAbandon(player, questId);
        }

        /// A player character deleted: its companion character and account go with it.
        void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override { sAnimusMod->OnOwnerDeleted(guid); }

        /// Companions get no mail: none sent to one by a player ...
        bool OnPlayerCanSendMail(Player* player, ObjectGuid receiver, ObjectGuid /*mailbox*/, std::string& /*subject*/,
            std::string& /*body*/, uint32 /*money*/, uint32 /*cod*/, Item* /*item*/) override
        {
            if (!sAnimusMod->IsCompanion(receiver))
                return true;

            ChatHandler(player->GetSession()).SendSysMessage("Companions receive no mail.");
            return false;
        }

        /// ... nor a level reward from the server.
        bool OnPlayerCanGiveMailRewardAtGiveLevel(Player* player, uint8 /*level*/) override
        {
            return !sAnimusMod->IsCompanion(player->GetGUID());
        }

        /// Companions earn no achievements (their criteria are not even checked: AnimusAchievementScript).
        bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const* /*achievement*/) override
        {
            return !sAnimusMod->IsCompanion(player->GetGUID());
        }

        /// An addon whisper a player sends to themselves is the Animus addon talking to the module (any player, no
        /// security): answered here and never delivered. Every other whisper goes on its way.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
        {
            if (lang != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player)
                return true;

            return !Animus::Addon::Handle(player, msg);
        }
    };

    class AnimusAchievementScript : public AchievementScript
    {
    public:
        AnimusAchievementScript() : AchievementScript("AnimusAchievementScript",
            { ACHIEVEMENTHOOK_CAN_CHECK_CRITERIA }) { }

        bool CanCheckCriteria(AchievementMgr* mgr, AchievementCriteriaEntry const* /*criteria*/) override
        {
            return !mgr->GetPlayer() || !sAnimusMod->IsCompanion(mgr->GetPlayer()->GetGUID());
        }
    };

    class AnimusUnitScript : public UnitScript
    {
    public:
        AnimusUnitScript() : UnitScript("AnimusUnitScript") { }

        /// Called for every damage event, on map threads, before the victim's AI can change the amount, so companions
        /// see what was dealt. (Stage seats are counted by animus-lib's own hooks.)
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
}

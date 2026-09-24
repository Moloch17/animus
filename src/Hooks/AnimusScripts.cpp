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
                { "summon",     HandleSummonCommand,    SEC_GAMEMASTER, Console::No },
                { "list",       HandleListCommand,      SEC_GAMEMASTER, Console::No },
                { "dismiss",    HandleDismissCommand,   SEC_GAMEMASTER, Console::No },
                { "stage",      stageCommandTable },
            };

            static ChatCommandTable commandTable =
            {
                { "animus", animusCommandTable },
            };

            return commandTable;
        }

        /// .animus summon <race> <class> <role>: a companion of that race, class and role (human priest heal, orc
        /// warrior tank, ...) at your level joins your party and plays its trained model.
        static bool HandleSummonCommand(ChatHandler* handler, std::string_view race, std::string_view playerClass,
            std::string_view role)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Summon(handler->GetPlayer(), race, playerClass, role, message), message);
        }

        /// .animus list: your class companions and their models.
        static bool HandleListCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->List(handler->GetPlayer()), "You have no class companions.");
        }

        /// .animus dismiss: remove all your companions.
        static bool HandleDismissCommand(ChatHandler* handler)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Dismiss(handler->GetPlayer(), message), message);
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
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

        void OnAfterConfigLoad(bool /*reload*/) override { sAnimusMod->LoadConfig(); }
        void OnUpdate(uint32 diff) override { sAnimusMod->OnUpdate(diff); }
        void OnShutdown() override { sAnimusMod->OnShutdown(); }
    };

    class AnimusPlayerScript : public PlayerScript
    {
    public:
        AnimusPlayerScript() : PlayerScript("AnimusPlayerScript",
            { PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT }) { }

        void OnPlayerLogout(Player* player) override { sAnimusMod->OnPlayerLogout(player); }

        /// An addon whisper a player sends to themselves is the Animus addon talking to the module (any player, no
        /// security): answered here and never delivered. Every other whisper goes on its way.
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
        {
            if (lang != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player)
                return true;

            return !Animus::Addon::Handle(player, msg);
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
    new AnimusUnitScript();
}

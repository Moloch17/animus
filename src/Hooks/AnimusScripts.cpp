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
#include "Chat.h"
#include "CommandScript.h"
#include "Optional.h"
#include "Player.h"
#include "PlayerScript.h"
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
                { "open",       HandleStageOpenCommand,     SEC_GAMEMASTER, Console::No },
                { "spawn",      HandleStageSpawnCommand,    SEC_GAMEMASTER, Console::No },
                { "start",      HandleStageStartCommand,    SEC_GAMEMASTER, Console::No },
                { "stop",       HandleStageStopCommand,     SEC_GAMEMASTER, Console::No },
                { "status",     HandleStageStatusCommand,   SEC_GAMEMASTER, Console::No },
                { "close",      HandleStageCloseCommand,    SEC_GAMEMASTER, Console::No },
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

        /// .animus list: your class/role companions and their models.
        static bool HandleListCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->List(handler->GetPlayer()), "You have no class/role companions.");
        }

        /// .animus dismiss: remove all your companions.
        static bool HandleDismissCommand(ChatHandler* handler)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Dismiss(handler->GetPlayer(), message), message);
        }

        /// .animus stage list: every curriculum stage, its arenas and how to start one.
        static bool HandleStageListCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->StageList(), "There are no stages.");
        }

        /// .animus stage open <stage> [policy] [arena]: teleport to where the stage happens, run it there without the
        /// learner, and spawn its first episode frozen. policy: model (the seats' exported models, Animus.Stage.Policy
        /// by default), random, greedy or fight; arena: only that arena of a stage that mixes several.
        static bool HandleStageOpenCommand(ChatHandler* handler, std::string_view stage,
            Optional<std::string_view> policy, Optional<std::string_view> arena)
        {
            std::string message;
            return Reply(handler, sAnimusMod->StageOpen(handler->GetPlayer(), stage, policy.value_or(""),
                arena.value_or(""), message), message);
        }

        /// .animus stage spawn [tier] [class_role] [level]: remove the episode and spawn a new one, frozen. tier: a
        /// difficulty tier of a stage that fights a creature or a pack; class_role: what the first seat plays
        /// (warlock_dps); level: every character's level. Each is "any" or left out for the curriculum's own, and holds
        /// for the episodes after it.
        static bool HandleStageSpawnCommand(ChatHandler* handler, Optional<std::string_view> tier,
            Optional<std::string_view> classRole, Optional<std::string_view> level)
        {
            std::string message;
            return Reply(handler, sAnimusMod->StageSpawn(handler->GetPlayer(), tier.value_or(""),
                classRole.value_or(""), level.value_or(""), message), message);
        }

        /// .animus stage start: let the stage play; episodes follow one another until `.animus stage stop`.
        static bool HandleStageStartCommand(ChatHandler* handler)
        {
            std::string message;
            return Reply(handler, sAnimusMod->StageRun(handler->GetPlayer(), message), message);
        }

        /// .animus stage stop: freeze the stage where it is.
        static bool HandleStageStopCommand(ChatHandler* handler)
        {
            std::string message;
            return Reply(handler, sAnimusMod->StageFreeze(handler->GetPlayer(), message), message);
        }

        /// .animus stage close: remove the stage you have open.
        static bool HandleStageCloseCommand(ChatHandler* handler)
        {
            std::string message;
            return Reply(handler, sAnimusMod->StageClose(handler->GetPlayer(), message), message);
        }

        /// .animus stage status: the stage you are watching, its episode and its seats.
        static bool HandleStageStatusCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->StageStatus(handler->GetPlayer()),
                "You have no stage open: `.animus stage open <stage>` opens one.");
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
        AnimusPlayerScript() : PlayerScript("AnimusPlayerScript", { PLAYERHOOK_ON_LOGOUT }) { }

        void OnPlayerLogout(Player* player) override { sAnimusMod->OnPlayerLogout(player); }
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

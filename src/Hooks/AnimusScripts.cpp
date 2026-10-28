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
#include "CharacterPrep.h"
#include "AchievementMgr.h"
#include "AchievementScript.h"
#include "Chat.h"
#include "CompanionClient.h"
#include "CommandScript.h"
#include "Log.h"
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

    /// Every `.animus` command: its name, who may use it, and its help -- the usage line a wrong call prints
    /// (Usage) and what `.animus help` lists. Kept in the code alone: the core's `command` table (where `.help`
    /// reads from) is not written (the user, 2026-10-26: nothing of the module's goes into the world database). The
    /// security here is the one the code table carries.
    struct CommandHelp
    {
        char const* Name;
        uint32 Security;
        char const* Help;
    };

    constexpr CommandHelp COMMAND_HELP[] =
    {
        { "animus", SEC_PLAYER,
          "Syntax: .animus <command>\r\n\r\nAnimus followers and settings. Commands: follow, dismiss, partyfill, "
          "prepare; game masters also models. .animus help lists them with their syntax." },
        { "animus help", SEC_PLAYER, "Syntax: .animus help\r\n\r\nEvery .animus command with its syntax." },
        { "animus follow", SEC_PLAYER,
          "Syntax: .animus follow [$count]\r\n\r\n$count (1-4) followers of your level and faction join your party, "
          "which you lead (made when you have none; never a raid), each played by its class's follow model. Alone: "
          "the followers you have. Needs Animus.Follow.Security and a map with a camera scene (Animus.Vision.Maps)." },
        { "animus dismiss", SEC_PLAYER, "Syntax: .animus dismiss\r\n\r\nYour followers leave (they are never "
          "saved)." },
        { "animus partyfill", SEC_PLAYER, "Syntax: .animus partyfill [on|off]\r\n\r\nWhether your own dungeon parties "
          "are filled with bots (saved per character); alone, the current setting and the realm's." },
        { "animus partyfill realm", SEC_GAMEMASTER, "Syntax: .animus partyfill realm on|off\r\n\r\nThe realm's "
          "dungeon party fill for every player, until the next .reload config." },
        { "animus prepare", SEC_PLAYER, "Syntax: .animus prepare $level $spec\r\n\r\nYour character set to that "
          "level, geared, talented and taught for the spec (its name or index: .animus prepare 18 arms). Everything "
          "you wear and carry is mailed to you first. Needs the account security Animus.Prepare.Security." },
        { "animus models", SEC_GAMEMASTER, "Syntax: .animus models\r\n\r\nEvery class's model for the configured "
          "stage, whether it loads, and the camera scenes loaded." },
    };

    /// The usage line of a command, as the help table has it (its first line), for a wrong call.
    bool Usage(ChatHandler* handler, char const* name)
    {
        for (CommandHelp const& command : COMMAND_HELP)
            if (std::string_view(command.Name) == name)
            {
                std::string_view const text(command.Help);
                handler->SendErrorMessage(std::string(text.substr(0, text.find("\r\n"))), false);
                return false;
            }
        return false;
    }

    class AnimusCommandScript : public CommandScript
    {
    public:
        AnimusCommandScript() : CommandScript("AnimusCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable partyFillCommandTable =
            {
                { "realm",      HandlePartyFillRealmCommand, SEC_GAMEMASTER, Console::No },
                { "",           HandlePartyFillCommand,      SEC_PLAYER,     Console::No },
            };

            // Every player's: what the addon does with buttons, typed. Game masters' and the administrator's
            // commands stay theirs. The `command` table (InstallCommandHelp) carries the same securities with the
            // help text.
            static ChatCommandTable animusCommandTable =
            {
                { "help",       HandleHelpCommand,      SEC_PLAYER,     Console::Yes },
                { "partyfill",  partyFillCommandTable },
                { "prepare",    HandlePrepareCommand,   SEC_PLAYER,     Console::No },
                { "follow",     HandleFollowCommand,    SEC_PLAYER,     Console::No },
                { "dismiss",    HandleDismissCommand,   SEC_PLAYER,     Console::No },
                { "models",     HandleModelsCommand,    SEC_GAMEMASTER, Console::Yes },
            };

            static ChatCommandTable commandTable =
            {
                { "animus", animusCommandTable },
            };

            return commandTable;
        }

        /// .animus help: every command with its syntax, from the help table (what the player may use).
        static bool HandleHelpCommand(ChatHandler* handler)
        {
            uint32 const security = handler->GetSession() ? uint32(handler->GetSession()->GetSecurity())
                : uint32(SEC_CONSOLE);
            handler->SendSysMessage("Animus commands (.help animus <command> for the details):");
            for (CommandHelp const& command : COMMAND_HELP)
            {
                std::string_view const name(command.Name);
                std::string_view const text(command.Help);
                if (name == "animus" || name == "animus help" || command.Security > security)
                    continue;
                // The usage line without its "Syntax: " prefix.
                std::string_view usage = text.substr(0, text.find("\r\n"));
                if (usage.rfind("Syntax: ", 0) == 0)
                    usage.remove_prefix(8);
                handler->SendSysMessage(std::string(usage));
            }
            return true;
        }

        /// .animus follow <n>: n (1-4) followers join your party, played by the follow models. With no count: the
        /// followers you have. Animus.Follow.Security says who may.
        static bool HandleFollowCommand(ChatHandler* handler, Optional<uint32> count)
        {
            Player* caller = handler->GetPlayer();
            if (!count)
                return ReplyLines(handler, sAnimusMod->List(caller), "You have no followers; .animus follow <1-4>.");
            if (!sAnimusMod->MaySummon(caller))
                return Reply(handler, false, "Followers are not for your account on this realm (Animus.Follow.Security).");
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

        /// .animus partyfill [on|off]: whether your own dungeon party is filled with bots.
        static bool HandlePartyFillCommand(ChatHandler* handler, Optional<std::string_view> state)
        {
            Player* player = handler->GetPlayer();
            std::string message;
            if (!state)
            {
                Animus::AnimusMod::PartyFillStatus const fill = sAnimusMod->PartyFillState(player);
                return Reply(handler, true, Acore::StringFormat("Your dungeon party fill is {}; the realm's is {}{}.",
                    fill.Mine ? "on" : "off", fill.Realm ? "on" : "off", fill.Overridden ? " (a game master's override "
                    "until the next config reload)" : ""));
            }
            if (*state != "on" && *state != "off")
                return Usage(handler, "animus partyfill");
            return Reply(handler, sAnimusMod->SetPartyFill(player, *state == "on", message), message);
        }

        /// .animus partyfill realm on|off: the realm's dungeon party fill, for every player, until the next config
        /// reload.
        static bool HandlePartyFillRealmCommand(ChatHandler* handler, Optional<std::string_view> state)
        {
            if (!state || (*state != "on" && *state != "off"))
                return Usage(handler, "animus partyfill realm");
            std::string message;
            return Reply(handler, sAnimusMod->PartyFillRealm(*state == "on", message), message);
        }

        /// .animus prepare <level> <spec>: your character at that level, geared and talented for the spec (an index
        /// or a name of your class's specs).
        static bool HandlePrepareCommand(ChatHandler* handler, Optional<uint8> level, Optional<std::string_view> spec)
        {
            if (!level || !spec)
                return Usage(handler, "animus prepare");
            Player* player = handler->GetPlayer();
            std::vector<std::string> const specs = Animus::Prepare::Specs(player);
            uint8 index = 0;
            bool found = false;
            for (std::size_t i = 0; i < specs.size(); ++i)
                if (specs[i] == *spec || std::to_string(i) == *spec)
                {
                    index = uint8(i);
                    found = true;
                }
            if (!found)
            {
                std::string names;
                for (std::size_t i = 0; i < specs.size(); ++i)
                    names += Acore::StringFormat("{}{} {}", i ? ", " : "", i, specs[i]);
                return Reply(handler, false, Acore::StringFormat("The spec is one of: {}.", names));
            }
            std::string message;
            return Reply(handler, Animus::Prepare::Prepare(player, *level, index, message), message);
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
            { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
                PLAYERHOOK_CAN_SEND_MAIL, PLAYERHOOK_CAN_GIVE_MAIL_REWARD_AT_GIVE_LEVEL,
                PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE, PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_AFTER_UPDATE }) { }

        /// Every player's update, on its map's thread: a follower's client steps its body and reports it, as a real
        /// client's movement is handled on that thread (CompanionClient). Any other player returns at once.
        void OnPlayerAfterUpdate(Player* player, uint32 diff) override
        {
            Animus::Client::Clients::OnPlayerAfterUpdate(player, diff);
        }

        void OnPlayerLogin(Player* player) override { sAnimusMod->OnPlayerLogin(player); }
        void OnPlayerLogout(Player* player) override { sAnimusMod->OnPlayerLogout(player); }

        /// Into a five-player dungeon alone or as the group's leader, the group is filled with followers
        /// (Animus.PartyFill.Enable, the player's setting); out of it, they go.
        void OnPlayerMapChanged(Player* player) override { sAnimusMod->OnPlayerMapChanged(player); }

        /// Followers get no mail: none sent to one by a player ...
        bool OnPlayerCanSendMail(Player* player, ObjectGuid receiver, ObjectGuid /*mailbox*/, std::string& /*subject*/,
            std::string& /*body*/, uint32 /*money*/, uint32 /*cod*/, Item* /*item*/) override
        {
            if (!sAnimusMod->IsBot(receiver))
                return true;

            ChatHandler(player->GetSession()).SendSysMessage("Followers receive no mail.");
            return false;
        }

        /// ... nor a level reward from the server.
        bool OnPlayerCanGiveMailRewardAtGiveLevel(Player* player, uint8 /*level*/) override
        {
            return !sAnimusMod->IsBot(player->GetGUID());
        }

        /// Followers earn no achievements (their criteria are not even checked: AnimusAchievementScript).
        bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const* /*achievement*/) override
        {
            return !sAnimusMod->IsBot(player->GetGUID());
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
            return !mgr->GetPlayer() || !sAnimusMod->IsBot(mgr->GetPlayer()->GetGUID());
        }
    };

    class AnimusServerScript : public ServerScript
    {
    public:
        AnimusServerScript() : ServerScript("AnimusServerScript", { SERVERHOOK_ON_PACKET_SENT }) { }

        /// Every packet queued for any session, sockets or not, on the thread that sends it: what the server tells a
        /// companion's client about its own movement goes into that client's inbox (CompanionClient).
        void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
        {
            Animus::Client::Clients::OnPacketSent(session, packet);
        }
    };

    class AnimusUnitScript : public UnitScript
    {
    public:
        AnimusUnitScript() : UnitScript("AnimusUnitScript") { }

        /// Called for every damage event, on map threads, before the victim's AI can change the amount, so followers
        /// see what they took.
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

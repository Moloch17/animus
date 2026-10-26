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
#include "Capture.h"
#include "CharacterPrep.h"
#include "AchievementMgr.h"
#include "AchievementScript.h"
#include "Chat.h"
#include "CompanionClient.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
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

    /// Every `.animus` command: its name in the core's `command` table, who may use it, and its help -- the usage
    /// line the core prints on a wrong syntax and `.help animus <command>` shows. Written into the world database
    /// at startup (the table is where the core reads help and security from), so the chat commands work and explain
    /// themselves without a separate SQL step. The security here is the one the code table carries.
    struct CommandHelp
    {
        char const* Name;
        uint32 Security;
        char const* Help;
    };

    constexpr CommandHelp COMMAND_HELP[] =
    {
        { "animus", SEC_PLAYER,
          "Syntax: .animus <command>\r\n\r\nAnimus companions, recordings and settings. Commands: create, summon, "
          "dismiss, list, rename, reroll, talent, rate, record, note, partyfill, prepare, god; game masters also "
          "life, stage list, models, purge. .animus help lists them with their syntax." },
        { "animus help", SEC_PLAYER, "Syntax: .animus help\r\n\r\nEvery .animus command with its syntax." },
        { "animus create", SEC_PLAYER,
          "Syntax: .animus create $name $race $class\r\n\r\nYour one companion character, of that name, race and "
          "class (race and class words as the game spells them, one word: nightelf, deathknight). Example: .animus "
          "create Tarn orc warrior" },
        { "animus summon", SEC_PLAYER, "Syntax: .animus summon\r\n\r\nYour companion comes to you." },
        { "animus dismiss", SEC_PLAYER, "Syntax: .animus dismiss\r\n\r\nYour companion is saved and sent away." },
        { "animus list", SEC_PLAYER, "Syntax: .animus list\r\n\r\nYour companion and whether its model is loaded." },
        { "animus rename", SEC_PLAYER, "Syntax: .animus rename $name\r\n\r\nYour companion's new name; nothing else "
          "changes." },
        { "animus reroll", SEC_PLAYER, "Syntax: .animus reroll $race $class\r\n\r\nYour companion becomes a new "
          "character of the same name, with default talents and gear. Everything you changed on it is lost." },
        { "animus talent", SEC_PLAYER, "Syntax: .animus talent $companion learn|unlearn $talentId\r\n\r\nOne rank of "
          "your companion's talent learned or unlearned, by Talent.dbc id (the id in a talent link). The addon's "
          "talent pane does this by clicking." },
        { "animus rate", SEC_PLAYER, "Syntax: .animus rate +|- [movement|combat|healing|tanking|stuck|other]\r\n\r\n"
          "Your verdict on your companion's play, recorded for training while the realm records play." },
        { "animus record", SEC_PLAYER, "Syntax: .animus record [on | off [keep|discard] [$title]]\r\n\r\nA recording "
          "of your own play: on starts one, off ends it with your verdict and an optional title; alone, its status. "
          "Notes go in with .animus note." },
        { "animus note", SEC_PLAYER, "Syntax: .animus note $text\r\n\r\nFree text into your running recording, with "
          "where you stand, your target and the state of the moment." },
        { "animus partyfill", SEC_PLAYER, "Syntax: .animus partyfill [on|off]\r\n\r\nWhether your own dungeon parties "
          "are filled with bots (saved per character); alone, the current setting and the realm's." },
        { "animus partyfill realm", SEC_GAMEMASTER, "Syntax: .animus partyfill realm on|off\r\n\r\nThe realm's "
          "dungeon party fill for every player, until the next .reload config." },
        { "animus prepare", SEC_PLAYER, "Syntax: .animus prepare $level $spec\r\n\r\nYour character set to that "
          "level, geared, talented and taught for the spec (its name or index: .animus prepare 18 arms). Everything "
          "you wear and carry is mailed to you first. Needs the account security Animus.Prepare.Security." },
        { "animus god", SEC_PLAYER, "Syntax: .animus god on|off\r\n\r\nGod mode for a recording, flagged in it and "
          "switched off when the recording ends. Needs Animus.Capture.Recording.GodSecurity." },
        { "animus life", SEC_GAMEMASTER, "Syntax: .animus life [$feature on|off]\r\n\r\nWhat companions do outside "
          "the fight; a switch for each of all, quests, auction, mail, taxi, crafting (until the next .reload "
          "config). Alone, the current switches." },
        { "animus stage", SEC_GAMEMASTER, "Syntax: .animus stage list\r\n\r\nThe curriculum stages." },
        { "animus stage list", SEC_GAMEMASTER, "Syntax: .animus stage list\r\n\r\nEvery curriculum stage and its "
          "arenas: the names Animus.Curriculum.Stage accepts." },
        { "animus models", SEC_GAMEMASTER, "Syntax: .animus models\r\n\r\nEvery class's model for the configured "
          "stage and whether it loads." },
        { "animus purge", SEC_ADMINISTRATOR, "Syntax: .animus purge\r\n\r\nEvery account and character the module "
          "made deleted, every companion sent away unsaved, the module's tables dropped." },
    };

    /// The help of every `.animus` command into the world database's `command` table (help and security), and the
    /// command map reloaded so the running server has it. World thread, at startup.
    void InstallCommandHelp()
    {
        for (CommandHelp const& command : COMMAND_HELP)
        {
            std::string help(command.Help);
            WorldDatabase.EscapeString(help);
            WorldDatabase.DirectExecute(Acore::StringFormat("INSERT INTO `command` (`name`, `security`, `help`) VALUES "
                "('{}', {}, '{}') ON DUPLICATE KEY UPDATE `security` = VALUES(`security`), `help` = VALUES(`help`)",
                command.Name, command.Security, help));
        }
        Acore::ChatCommands::InvalidateCommandMap();
        LOG_INFO("module.animus", "Animus: {} .animus commands with their help in the command table",
            std::size(COMMAND_HELP));
    }

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
            static ChatCommandTable stageCommandTable =
            {
                { "list",       HandleStageListCommand,     SEC_GAMEMASTER, Console::No },
            };

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
                { "record",     HandleRecordCommand,    SEC_PLAYER,     Console::No },
                { "note",       HandleNoteCommand,      SEC_PLAYER,     Console::No },
                { "partyfill",  partyFillCommandTable },
                { "prepare",    HandlePrepareCommand,   SEC_PLAYER,     Console::No },
                { "god",        HandleGodCommand,       SEC_PLAYER,     Console::No },
                { "create",     HandleCreateCommand,    SEC_PLAYER,     Console::No },
                { "summon",     HandleSummonCommand,    SEC_PLAYER,     Console::No },
                { "list",       HandleListCommand,      SEC_PLAYER,     Console::No },
                { "dismiss",    HandleDismissCommand,   SEC_PLAYER,     Console::No },
                { "rename",     HandleRenameCommand,    SEC_PLAYER,     Console::No },
                { "reroll",     HandleRerollCommand,    SEC_PLAYER,     Console::No },
                { "talent",     HandleTalentCommand,    SEC_PLAYER,     Console::No },
                { "purge",      HandlePurgeCommand,     SEC_ADMINISTRATOR, Console::Yes },
                { "life",       HandleLifeCommand,      SEC_GAMEMASTER, Console::Yes },
                { "models",     HandleModelsCommand,    SEC_GAMEMASTER, Console::Yes },
                { "rate",       HandleRateCommand,      SEC_PLAYER,     Console::No },
                { "stage",      stageCommandTable },
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
                if (name == "animus" || name == "animus help" || name == "animus stage" || command.Security > security)
                    continue;
                // The usage line without its "Syntax: " prefix.
                std::string_view usage = text.substr(0, text.find("\r\n"));
                if (usage.rfind("Syntax: ", 0) == 0)
                    usage.remove_prefix(8);
                handler->SendSysMessage(std::string(usage));
            }
            return true;
        }

        /// .animus create <name> <race> <class>: your one companion character, of that name, race and class.
        static bool HandleCreateCommand(ChatHandler* handler, Optional<std::string> name,
            Optional<std::string_view> race, Optional<std::string_view> playerClass)
        {
            if (!name || !race || !playerClass)
                return Usage(handler, "animus create");
            std::string message;
            return Reply(handler, sAnimusMod->Create(handler->GetPlayer(), std::move(*name), *race, *playerClass,
                message), message);
        }

        /// .animus talent <companion> learn|unlearn <talent id>: one rank of its talent.
        static bool HandleTalentCommand(ChatHandler* handler, Optional<std::string_view> companion,
            Optional<std::string_view> action, Optional<uint32> talentId)
        {
            if (!companion || !action || !talentId || (*action != "learn" && *action != "unlearn"))
                return Usage(handler, "animus talent");
            std::string message;
            return Reply(handler, sAnimusMod->Talent(handler->GetPlayer(), *companion, *talentId, *action == "learn",
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
        static bool HandleRenameCommand(ChatHandler* handler, Optional<std::string> name)
        {
            if (!name)
                return Usage(handler, "animus rename");
            std::string message;
            return Reply(handler, sAnimusMod->Rename(handler->GetPlayer(), std::move(*name), message), message);
        }

        /// .animus reroll <race> <class>: a new companion character of the same name.
        static bool HandleRerollCommand(ChatHandler* handler, Optional<std::string_view> race,
            Optional<std::string_view> playerClass)
        {
            if (!race || !playerClass)
                return Usage(handler, "animus reroll");
            std::string message;
            return Reply(handler, sAnimusMod->Reroll(handler->GetPlayer(), *race, *playerClass, message), message);
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

        /// .animus models: every class's model for the configured stage, and whether it loads (and if not, why).
        static bool HandleModelsCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->ModelStatus(), "No classes.");
        }

        /// .animus rate <+|-> [movement|combat|healing|tanking|stuck|other]: your verdict on your companion's play,
        /// recorded for training (Animus.Capture). Any player may rate their own companion.
        static bool HandleRateCommand(ChatHandler* handler, Optional<std::string_view> sign,
            Optional<std::string_view> reason)
        {
            if (!sign || (*sign != "+" && *sign != "-"))
                return Usage(handler, "animus rate");
            std::string message;
            return Reply(handler, sAnimusMod->Rate(handler->GetPlayer(), *sign, reason.value_or(""), message),
                message);
        }

        /// .animus stage list: every curriculum stage and its arenas -- the names Animus.Curriculum.Stage takes.
        static bool HandleStageListCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->StageList(), "There are no stages.");
        }

        /// .animus record [on | off [keep|discard] [title...]]: a recording of your own play (the addon's Record).
        static bool HandleRecordCommand(ChatHandler* handler, Optional<std::string_view> state,
            Optional<std::string_view> verdict, Tail title)
        {
            Player* player = handler->GetPlayer();
            std::string message;
            if (!state)
            {
                Animus::Capture::RecordingStatus const status = Animus::Capture::Status(player);
                if (!status.Available)
                    return Reply(handler, false, "Recording is not available on this realm.");
                return Reply(handler, true, status.On ? Acore::StringFormat("Recording {} for {}m{:02}s, {} notes.",
                    status.Id, status.Seconds / 60, status.Seconds % 60, status.Notes) : "Not recording.");
            }
            if (*state == "on")
                return Reply(handler, Animus::Capture::StartRecording(player, message), message);
            if (*state == "off")
            {
                std::string_view const given = verdict.value_or("");
                if (!given.empty() && given != "keep" && given != "discard")
                    return Reply(handler, false, "A recording is kept or discarded.");
                return Reply(handler, Animus::Capture::StopRecording(player, Animus::Capture::StopReason::Stop, given,
                    std::string_view(title), message), message);
            }
            return Usage(handler, "animus record");
        }

        /// .animus note <text...>: a note into your running recording.
        static bool HandleNoteCommand(ChatHandler* handler, Tail text)
        {
            if (std::string_view(text).empty())
                return Usage(handler, "animus note");
            std::string message;
            uint32 seq = 0;
            return Reply(handler, Animus::Capture::Note(handler->GetPlayer(), std::string_view(text), 0, seq, message),
                message);
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

        /// .animus god on|off: invulnerable while your recording runs (the account security Prepare needs).
        static bool HandleGodCommand(ChatHandler* handler, Optional<std::string_view> state)
        {
            Player* player = handler->GetPlayer();
            if (!state || (*state != "on" && *state != "off"))
                return Usage(handler, "animus god");
            if (!player->GetSession() || uint32(player->GetSession()->GetSecurity())
                < sAnimusMod->Config().PrepareSecurity)
                return Reply(handler, false, "God mode is not for your account on this realm.");
            std::string message;
            return Reply(handler, Animus::Capture::God(player, *state == "on", message), message);
        }

    };

    class AnimusWorldScript : public WorldScript
    {
    public:
        AnimusWorldScript() : WorldScript("AnimusWorldScript",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

        void OnAfterConfigLoad(bool /*reload*/) override { sAnimusMod->LoadConfig(); }
        void OnStartup() override
        {
            sAnimusMod->OnStartup();
            InstallCommandHelp();
        }
        void OnUpdate(uint32 diff) override { sAnimusMod->OnUpdate(diff); }
        void OnShutdown() override { sAnimusMod->OnShutdown(); }
    };

    class AnimusPlayerScript : public PlayerScript
    {
    public:
        AnimusPlayerScript() : PlayerScript("AnimusPlayerScript",
            { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_DELETE,
                PLAYERHOOK_CAN_SEND_MAIL, PLAYERHOOK_CAN_GIVE_MAIL_REWARD_AT_GIVE_LEVEL,
                PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE, PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
                PLAYERHOOK_ON_QUEST_ABANDON, PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_AFTER_UPDATE }) { }

        /// Every player's update, on its map's thread: a companion's client steps its body and reports it, as a real
        /// client's movement is handled on that thread (CompanionClient). Any other player returns at once.
        void OnPlayerAfterUpdate(Player* player, uint32 diff) override
        {
            Animus::Client::Clients::OnPlayerAfterUpdate(player, diff);
        }

        void OnPlayerLogin(Player* player) override { sAnimusMod->OnPlayerLogin(player); }
        void OnPlayerLogout(Player* player) override { sAnimusMod->OnPlayerLogout(player); }

        /// Into a five-player dungeon, the group is filled out to a tank, a healer and damage (Animus.PartyFill.Enable);
        /// out of it, the temporary members stay behind.
        void OnPlayerMapChanged(Player* player) override { sAnimusMod->OnPlayerMapChanged(player); }

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
    new AnimusServerScript();
}

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
            static ChatCommandTable stageCommandTable =
            {
                { "list",       HandleStageListCommand,     SEC_GAMEMASTER, Console::No },
            };

            static ChatCommandTable partyFillCommandTable =
            {
                { "realm",      HandlePartyFillRealmCommand, SEC_GAMEMASTER, Console::No },
                { "",           HandlePartyFillCommand,      SEC_PLAYER,     Console::No },
            };

            static ChatCommandTable animusCommandTable =
            {
                { "record",     HandleRecordCommand,    SEC_PLAYER,     Console::No },
                { "note",       HandleNoteCommand,      SEC_PLAYER,     Console::No },
                { "partyfill",  partyFillCommandTable },
                { "prepare",    HandlePrepareCommand,   SEC_PLAYER,     Console::No },
                { "god",        HandleGodCommand,       SEC_PLAYER,     Console::No },
                { "create",     HandleCreateCommand,    SEC_GAMEMASTER, Console::No },
                { "summon",     HandleSummonCommand,    SEC_GAMEMASTER, Console::No },
                { "list",       HandleListCommand,      SEC_GAMEMASTER, Console::No },
                { "dismiss",    HandleDismissCommand,   SEC_GAMEMASTER, Console::No },
                { "rename",     HandleRenameCommand,    SEC_GAMEMASTER, Console::No },
                { "reroll",     HandleRerollCommand,    SEC_GAMEMASTER, Console::No },
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

        /// .animus models: every class's model for the configured stage, and whether it loads (and if not, why).
        static bool HandleModelsCommand(ChatHandler* handler)
        {
            return ReplyLines(handler, sAnimusMod->ModelStatus(), "No classes.");
        }

        /// .animus rate <+|-> [movement|combat|healing|tanking|stuck|other]: your verdict on your companion's play,
        /// recorded for training (Animus.Capture). Any player may rate their own companion.
        static bool HandleRateCommand(ChatHandler* handler, std::string_view sign, Optional<std::string_view> reason)
        {
            std::string message;
            return Reply(handler, sAnimusMod->Rate(handler->GetPlayer(), sign, reason.value_or(""), message),
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
            return Reply(handler, false, ".animus record on, or .animus record off keep|discard [title].");
        }

        /// .animus note <text...>: a note into your running recording.
        static bool HandleNoteCommand(ChatHandler* handler, Tail text)
        {
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
                return Reply(handler, false, ".animus partyfill on|off.");
            return Reply(handler, sAnimusMod->SetPartyFill(player, *state == "on", message), message);
        }

        /// .animus partyfill realm on|off: the realm's dungeon party fill, for every player, until the next config
        /// reload.
        static bool HandlePartyFillRealmCommand(ChatHandler* handler, std::string_view state)
        {
            if (state != "on" && state != "off")
                return Reply(handler, false, ".animus partyfill realm on|off.");
            std::string message;
            return Reply(handler, sAnimusMod->PartyFillRealm(state == "on", message), message);
        }

        /// .animus prepare <level> <spec>: your character at that level, geared and talented for the spec (an index
        /// or a name of your class's specs).
        static bool HandlePrepareCommand(ChatHandler* handler, uint8 level, std::string_view spec)
        {
            Player* player = handler->GetPlayer();
            std::vector<std::string> const specs = Animus::Prepare::Specs(player);
            uint8 index = 0;
            bool found = false;
            for (std::size_t i = 0; i < specs.size(); ++i)
                if (specs[i] == spec || std::to_string(i) == spec)
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
            return Reply(handler, Animus::Prepare::Prepare(player, level, index, message), message);
        }

        /// .animus god on|off: invulnerable while your recording runs (the account security Prepare needs).
        static bool HandleGodCommand(ChatHandler* handler, std::string_view state)
        {
            Player* player = handler->GetPlayer();
            if (state != "on" && state != "off")
                return Reply(handler, false, ".animus god on|off.");
            if (!player->GetSession() || uint32(player->GetSession()->GetSecurity())
                < sAnimusMod->Config().PrepareSecurity)
                return Reply(handler, false, "God mode is not for your account on this realm.");
            std::string message;
            return Reply(handler, Animus::Capture::God(player, state == "on", message), message);
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

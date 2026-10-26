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

#ifndef ANIMUS_MOD_H
#define ANIMUS_MOD_H

#include "AnimusConfig.h"
#include "FollowParty.h"
#include "Layout.h"
#include "ClassProfile.h"
#include "ModelLibrary.h"
#include "ObjectGuid.h"
#include "Unit.h"
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace Animus
{
    /// **Module root.** The settings, the model library, the camera scenes baked at startup, and the follow parties:
    /// bots of the caller's level and faction played by the `<class>_follow` models (Follow::FollowParty), summoned
    /// with `.animus follow <n>` or the addon's Summon, and summoned by themselves to fill a player's five-player
    /// dungeon group (Animus.PartyFill.Enable, the player's own setting, a game master's realm override). Beside them:
    /// the play capture with its recordings (Capture), Prepare (Prepare::Prepare) and each player's settings
    /// (PlayerSettings).
    ///
    /// The v1 class companion -- a character saved with yours, its director, life outside the fight -- is not in
    /// this build: it cannot run on the current runtime (the forge's Runtime cut of 2026-10-08 dropped its blocks)
    /// and no model of its format exists any more; its code is in attic/ (outside src/, never built), for reference.
    ///
    /// Threads: everything runs on the world thread (config load, startup, commands, the world update after the maps,
    /// shutdown), apart from RecordDamage and IsBot, which map threads call and which only read the bot index (changed
    /// on the world thread alone, never while maps update).
    class AnimusMod
    {
    public:
        static AnimusMod* Instance();

        void LoadConfig();
        /// The world is up: the camera scenes of Animus.Vision.Maps are baked when missing or stale, and loaded.
        void OnStartup();
        void OnUpdate(uint32 diff);
        void OnShutdown();

        /// `.animus follow <n>`: n (1-4) followers of the caller's level and faction join the caller's party, which
        /// the caller leads (made when there is none), each of a class with a loaded `<class>_follow` model. False with
        /// `message` set when refused; on success `message` says who came. `warnings` are said either way.
        bool Summon(Player* caller, uint32 count, std::string& message, std::vector<std::string>& warnings);
        /// `.animus dismiss`: the caller's followers leave, unsaved.
        bool Dismiss(Player* caller, std::string& message);
        /// `.animus follow` with no count: the caller's followers.
        [[nodiscard]] std::vector<std::string> List(Player* caller) const;
        /// `.animus models`: every class's model for Animus.Curriculum.Stage and whether it loads, and the camera
        /// scenes loaded.
        [[nodiscard]] std::vector<std::string> ModelStatus();

        /// A player logged out: a leader's followers go with it, and their settings are forgotten.
        void OnPlayerLogout(Player* player);
        /// A player logged in: their own settings are read (PlayerSettings).
        void OnPlayerLogin(Player* player);
        /// A player arrived on a map (Animus.PartyFill): entering a five-player dungeon alone or as its group's leader,
        /// the group is filled with followers two seconds later; leaving it alive, they go.
        void OnPlayerMapChanged(Player* player);

        /// **Dungeon party fill, per player and for the realm** (2026-10-25). Whether `player`'s group is filled on
        /// entering a dungeon: the module on, the realm's switch (Animus.PartyFill.Enable, or the GM override that
        /// lasts until the next config reload), and the player's own setting.
        [[nodiscard]] bool PartyFillFor(Player const* player) const;
        /// The player's own setting (`.animus partyfill on|off`, the addon's checkbox), kept per character.
        bool SetPartyFill(Player* player, bool on, std::string& message);
        /// The realm-wide override (`.animus partyfill realm on|off`, GM), in memory until the config is reloaded.
        bool PartyFillRealm(bool on, std::string& message);
        /// Both, for the addon's SETTINGS: the player's, the realm's now, whether the realm's is an override, and
        /// the config's own value.
        struct PartyFillStatus
        {
            bool Mine = true;
            bool Realm = true;
            bool Overridden = false;
            bool Config = true;
        };
        [[nodiscard]] PartyFillStatus PartyFillState(Player const* player) const;

        /// Whether `player`'s account may summon followers (Animus.Follow.Security).
        [[nodiscard]] bool MaySummon(Player const* player) const;
        /// The stage whose models the followers play (Animus.Curriculum.Stage).
        [[nodiscard]] std::string const& CurrentStage() const { return _config.CurriculumStage; }
        /// The layout Prepare builds a character with: the class's at the configured stage (its gear, talents and
        /// spells are the stage's seats').
        [[nodiscard]] Curriculum::Layout const& PrepareLayout(Curriculum::ClassProfile const& profile)
        {
            return LayoutFor(profile);
        }

        /// Whether `guid` is a follower (the capture marks its records a companion's). Safe on map threads.
        [[nodiscard]] bool IsBot(ObjectGuid guid) const { return _partyByBot.contains(guid); }

        /// Damage a follower took, for its next observation (UnitScript::DealDamage, map threads).
        void RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type);

        [[nodiscard]] bool IsEnabled() const { return _config.Enable; }
        [[nodiscard]] AnimusConfig const& Config() const { return _config; }
        /// Where the models are (Animus.ModelDir, resolved), and the library that reads them.
        [[nodiscard]] std::string const& ModelDir() const { return _config.ModelDir; }
        [[nodiscard]] ModelLibrary& Models() { return _models; }

    private:
        AnimusMod() = default;

        /// The layout of a class at the configured stage, built on first use and kept (followers point at it).
        Curriculum::Layout const& LayoutFor(Curriculum::ClassProfile const& profile);
        void RemoveParty(ObjectGuid leader);
        void RemoveAll();

        AnimusConfig _config;
        bool _visionConfigured = false;
        ModelLibrary _models;
        std::unordered_map<std::string, Curriculum::Layout> _layouts;       // by model name

        std::unordered_map<ObjectGuid, std::unique_ptr<Follow::FollowParty>> _parties;     // by leader
        std::unordered_map<ObjectGuid, Follow::FollowParty*> _partyByBot;
        /// Party fill: the instance each leader's group was filled for, the leaders whose fill is due (a short delay
        /// after the map change, so the group and the map are settled), and the realm-wide GM override (cleared by
        /// LoadConfig).
        std::unordered_map<ObjectGuid, uint32> _filledInstance;
        std::unordered_map<ObjectGuid, int32> _pendingFill;
        std::optional<bool> _partyFillOverride;
        /// Summon `count` followers for `leader` as the party fill would (no security, no scene message to the chat).
        void FillParty(Player* leader);
        uint32 _nextAccount = 0;
    };
}

#define sAnimusMod Animus::AnimusMod::Instance()

#endif

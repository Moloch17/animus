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
#include "ModelLibrary.h"
#include "ObjectGuid.h"
#include "Unit.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

namespace Animus
{
    /// **Module root, live follow build** (the live-follow plan): the settings, the model library, the camera scenes
    /// baked at startup, and the follow parties GMs summon with `.animus follow <n>`. The v1 features -- the class
    /// companion, the director, party fill, life outside the fight and the addon -- cannot run on the current runtime;
    /// their code is in attic/ (outside src/, so nothing builds it), kept for reference.
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

        /// A player logged out: a leader's followers go with it.
        void OnPlayerLogout(Player* player);

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
        uint32 _nextAccount = 0;
    };
}

#define sAnimusMod Animus::AnimusMod::Instance()

#endif

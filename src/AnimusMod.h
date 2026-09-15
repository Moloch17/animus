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
#include "CompanionParty.h"
#include "MlpPolicy.h"
#include "ModelLibrary.h"
#include "MovementTree.h"
#include "ObjectGuid.h"
#include "Unit.h"
#include "WarriorCompanion.h"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Player;
class Unit;

namespace Animus
{
    /// Module root: settings, the models, every player's warrior_dummy companion and class/role party.
    ///
    /// Everything except RecordDamage runs on the world thread (config load, commands, world
    /// update, shutdown), and none of it while maps are updating. RecordDamage runs on map threads
    /// and only reads the bot index, which is only changed on the world thread.
    class AnimusMod
    {
    public:
        static AnimusMod* Instance();

        void LoadConfig();
        void OnUpdate(uint32 diff);
        void OnShutdown();

        /// Command handlers. Return false with `message` set when the request is refused.
        bool Spawn(Player* owner, std::string& message);
        bool Attack(Player* owner, Unit* target, std::string& message);
        bool Dismiss(Player* owner, std::string& message);

        /// `classRole` is a class/role profile name (priest_heal). The companion plays that class/role's model for
        /// Animus.Curriculum.Stage and joins the owner's group.
        bool Summon(Player* owner, std::string_view classRole, std::string& message);

        /// One line per class/role companion of `owner`.
        [[nodiscard]] std::vector<std::string> List(Player* owner);

        /// A player logged out: remove the companion they own, if any.
        void OnPlayerLogout(Player* player);

        void RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type);

        [[nodiscard]] bool IsEnabled() const { return _config.Enable; }

    private:
        AnimusMod() = default;

        void LoadModel();
        void Remove(ObjectGuid owner);
        void RemoveParty(ObjectGuid owner);
        void RemoveAll();

        /// The layout of a profile at the configured stage, built on first use and kept (companions point at it).
        Curriculum::Layout const& LayoutFor(Curriculum::ClassRoleProfile const& profile);

        AnimusConfig _config;
        MlpPolicy _policy;
        std::string _modelError;
        MovementTree _tree;
        ModelLibrary _models;
        std::unordered_map<std::string, Curriculum::Layout> _layouts;        // by model name

        std::unordered_map<ObjectGuid, std::unique_ptr<WarriorCompanion>> _companions;  // by owner
        std::unordered_map<ObjectGuid, WarriorCompanion*> _byBot;

        std::unordered_map<ObjectGuid, std::unique_ptr<CompanionParty>> _parties;   // by owner
        std::unordered_map<ObjectGuid, CompanionParty*> _partyByBot;
    };
}

#define sAnimusMod Animus::AnimusMod::Instance()

#endif

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
#include "MlpPolicy.h"
#include "MovementTree.h"
#include "ObjectGuid.h"
#include "Unit.h"
#include "WarriorCompanion.h"
#include <memory>
#include <string>
#include <unordered_map>

class Player;
class Unit;

namespace Animus
{
    /// Module root: settings, the loaded model and every player's companion.
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

        /// A player logged out: remove the companion they own, if any.
        void OnPlayerLogout(Player* player);

        void RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type);

        [[nodiscard]] bool IsEnabled() const { return _config.Enable; }

    private:
        AnimusMod() = default;

        void LoadModel();
        void Remove(ObjectGuid owner);
        void RemoveAll();

        AnimusConfig _config;
        MlpPolicy _policy;
        std::string _modelError;
        MovementTree _tree;

        std::unordered_map<ObjectGuid, std::unique_ptr<WarriorCompanion>> _companions;  // by owner
        std::unordered_map<ObjectGuid, WarriorCompanion*> _byBot;
    };
}

#define sAnimusMod Animus::AnimusMod::Instance()

#endif

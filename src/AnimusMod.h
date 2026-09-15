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
#include "Layout.h"
#include "ModelLibrary.h"
#include "ObjectGuid.h"
#include "StageViewer.h"
#include "Unit.h"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Player;
class Unit;

namespace Animus
{
    /// Module root: settings, the models, every player's class/role party and every game master's stage viewer.
    ///
    /// Everything except RecordDamage runs on the world thread (config load, commands, world update, shutdown), and
    /// none of it while maps are updating. RecordDamage runs on map threads and only reads the bot index, which is
    /// only changed on the world thread.
    class AnimusMod
    {
    public:
        static AnimusMod* Instance();

        void LoadConfig();
        void OnUpdate(uint32 diff);
        void OnShutdown();

        /// Command handlers. Return false with `message` set when the request is refused.
        bool Dismiss(Player* owner, std::string& message);

        /// `classRole` is a class/role profile name (priest_heal). The companion plays that class/role's model for
        /// Animus.Curriculum.Stage and joins the owner's group.
        bool Summon(Player* owner, std::string_view classRole, std::string& message);

        /// One line per class/role companion of `owner`.
        [[nodiscard]] std::vector<std::string> List(Player* owner);

        /// Stage viewer commands: every stage and its arenas; start one where the stage happens (see StageViewer);
        /// stop it, start a new episode, or describe it.
        [[nodiscard]] std::vector<std::string> StageList() const;
        bool StageStart(Player* viewer, std::string_view stage, std::string_view policy, std::string_view arena,
            std::string& message);
        bool StageStop(Player* viewer, std::string& message);
        bool StageReset(Player* viewer, std::string& message);
        [[nodiscard]] std::vector<std::string> StageStatus(Player* viewer);

        /// A player logged out: remove the companions they own and the stage they watch.
        void OnPlayerLogout(Player* player);

        void RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type);

        [[nodiscard]] bool IsEnabled() const { return _config.Enable; }

    private:
        AnimusMod() = default;

        void RemoveParty(ObjectGuid owner);
        void RemoveViewer(ObjectGuid viewer);
        void RemoveAll();

        /// The layout of a profile at the configured stage, built on first use and kept (companions point at it).
        Curriculum::Layout const& LayoutFor(Curriculum::ClassRoleProfile const& profile);

        AnimusConfig _config;
        ModelLibrary _models;
        std::unordered_map<std::string, Curriculum::Layout> _layouts;       // by model name

        std::unordered_map<ObjectGuid, std::unique_ptr<CompanionParty>> _parties;   // by owner
        std::unordered_map<ObjectGuid, CompanionParty*> _partyByBot;

        std::unordered_map<ObjectGuid, std::unique_ptr<StageViewer>> _viewers;      // by viewer
    };
}

#define sAnimusMod Animus::AnimusMod::Instance()

#endif

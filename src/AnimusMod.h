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
    /// Module root: settings, the models and every player's class party.
    ///
    /// Everything except RecordDamage runs on the world thread (config load, commands, world update, shutdown), and
    /// none of it while maps are updating. RecordDamage runs on map threads and only reads the bot index and the
    /// parties' pulls, which are only changed on the world thread.
    class AnimusMod
    {
    public:
        static AnimusMod* Instance();

        void LoadConfig();
        void OnUpdate(uint32 diff);
        void OnShutdown();

        /// Command handlers. Return false with `message` set when the request is refused.
        bool Dismiss(Player* owner, std::string& message);
        /// One companion of `owner`, by name.
        bool DismissOne(Player* owner, std::string_view name, std::string& message);

        /// The addon's inspect-window edits of one companion of `owner` (CompanionParty::Talent, PetTalent, Equip,
        /// Pet).
        bool Talent(Player* owner, std::string_view name, uint32 talentId, bool learn, std::string& message);
        bool PetTalent(Player* owner, std::string_view name, uint32 talentId, bool learn, std::string& message);
        bool Equip(Player* owner, std::string_view name, uint8 bag, uint8 slot, uint8 equipSlot,
            std::string& message);
        bool Pet(Player* owner, std::string_view name, CompanionParty::PetView& view, std::string& message);

        /// A companion of `race` (human, nightelf, ...), `playerClass` (priest, deathknight, ...) and `role` (dps, tank,
        /// heal) joins the owner's group and plays its class's model for Animus.Curriculum.Stage. Refused for a
        /// name that is not one, a class that has no such role, a race the class does not allow, or a race of the
        /// other faction.
        bool Summon(Player* owner, std::string_view race, std::string_view playerClass, std::string_view role,
            std::string& message);

        /// One line per class companion of `owner`.
        [[nodiscard]] std::vector<std::string> List(Player* owner);

        /// The companions of `owner`, in the order they were summoned; none when they have no party.
        [[nodiscard]] std::vector<CompanionParty::Summary> Companions(Player* owner);

        /// A race a summon may ask for, with the classes it can be, in the names Summon accepts.
        struct RaceChoice
        {
            std::string Race;
            std::vector<std::string> Classes;
        };

        /// The races of `owner`'s faction, each with the classes it can be: everything Summon takes as its first
        /// two words. Cheap (player info only), unlike what a class can be asked for, which needs its assets built.
        [[nodiscard]] static std::vector<RaceChoice> RaceChoices(Player const* owner);

        /// The words Summon's third argument takes, one per demand (tank, heal, dps).
        [[nodiscard]] static std::vector<std::string> WantChoices();

        /// The stage whose models companions play (Animus.Curriculum.Stage).
        [[nodiscard]] std::string const& CurrentStage() const { return _config.CurriculumStage; }

        /// Every curriculum stage with its arenas: the names Animus.Curriculum.Stage accepts, and so which models a
        /// companion will look for.
        [[nodiscard]] std::vector<std::string> StageList() const;

        /// A player logged out: remove the companions they own.
        void OnPlayerLogout(Player* player);

        void RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type);

        [[nodiscard]] bool IsEnabled() const { return _config.Enable; }

    private:
        AnimusMod() = default;

        /// The party of `owner`, else null with `message` set.
        CompanionParty* PartyOf(Player* owner, std::string& message);

        void RemoveParty(ObjectGuid owner);
        void RemoveAll();

        /// The layout of a profile at the configured stage, built on first use and kept (companions point at it).
        Curriculum::Layout const& LayoutFor(Curriculum::ClassProfile const& profile);

        AnimusConfig _config;
        ModelLibrary _models;
        std::unordered_map<std::string, Curriculum::Layout> _layouts;       // by model name

        std::unordered_map<ObjectGuid, std::unique_ptr<CompanionParty>> _parties;   // by owner
        std::unordered_map<ObjectGuid, CompanionParty*> _partyByBot;
    };
}

#define sAnimusMod Animus::AnimusMod::Instance()

#endif

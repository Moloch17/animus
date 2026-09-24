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
#include "CompanionRegistry.h"
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
    /// Module root: settings, the models, the registry of every player's companion and the parties of those out.
    ///
    /// Everything except RecordDamage runs on the world thread (config load, commands, world update, shutdown), and
    /// none of it while maps are updating. RecordDamage runs on map threads and only reads the bot index and the
    /// parties' pulls, which are only changed on the world thread.
    class AnimusMod
    {
    public:
        static AnimusMod* Instance();

        void LoadConfig();
        /// The world is up: the companions on record are read (the character cache exists by then).
        void OnStartup();
        void OnUpdate(uint32 diff);
        void OnShutdown();

        /// Everything a player does with their one companion, from the addon or the commands. Each returns false
        /// with `message` set when refused; on success `message` says what happened.

        /// A companion character called `name`, of `race` (human, nightelf, ...) and `playerClass` (priest,
        /// deathknight, ...), on an account made for the owner, saved, and in the owner's party. Refused when the
        /// owner has one already, for a name the game would not accept or has taken, a race the class does not
        /// allow, or a race of the other faction.
        bool Create(Player* owner, std::string name, std::string_view race, std::string_view playerClass,
            std::string& message);
        /// The saved companion comes back beside the owner. Its rows load on the database thread; `message` says
        /// it is on its way, and the addon is told again (Addon::Push) once it stands there.
        bool Summon(Player* owner, std::string& message);
        /// Saved and out of the world; the character stays.
        bool Dismiss(Player* owner, std::string& message);
        /// A new name for the companion character, nothing else changed. Out, it is dismissed and summoned again
        /// under the new name (the client shows a name it has seen).
        bool Rename(Player* owner, std::string name, std::string& message);
        /// A new race and class: the companion character is deleted and a new one of the same name created.
        bool Reroll(Player* owner, std::string_view race, std::string_view playerClass, std::string& message);
        /// The owner character was deleted: their companion character and its account go too.
        void OnOwnerDeleted(ObjectGuid owner);

        /// Every account and character the module ever made, gone: companions out of the world unsaved, every
        /// `ANIMUS<guid>` account deleted with its characters (orphans of older runs included), the registry
        /// emptied. Returns what it did, one line per account.
        [[nodiscard]] std::vector<std::string> PurgeAll();

        /// The addon's inspect-window edits of the companion (CompanionParty::Talent, PetTalent, Equip, Pet).
        bool Talent(Player* owner, std::string_view name, uint32 talentId, bool learn, std::string& message);
        bool PetTalent(Player* owner, std::string_view name, uint32 talentId, bool learn, std::string& message);
        bool Equip(Player* owner, std::string_view name, uint8 bag, uint8 slot, uint8 equipSlot,
            std::string& message);
        bool Pet(Player* owner, std::string_view name, CompanionParty::PetView& view, std::string& message);

        /// What the owner has, for the window: nothing, or a character (its name, race and class words, level and
        /// spec), out beside them or waiting in the database.
        struct Companion
        {
            bool Exists = false;
            std::string Name;
            std::string Race;
            std::string Class;
            uint8 Level = 0;
            std::string Spec;
            bool Out = false;                   // in the world (or loading)
            bool Loading = false;
            bool Parked = false;
            std::string ModelName;
            std::string Model;                  // "loaded", or why not, when out
        };
        [[nodiscard]] Companion Describe(Player* owner);

        /// One line per fact of Describe, for `.animus list`.
        [[nodiscard]] std::vector<std::string> List(Player* owner);

        /// Whether `guid` is a companion character (of anyone): it gets no mail and no achievements.
        [[nodiscard]] bool IsCompanion(ObjectGuid guid) const { return _registry.IsCompanion(guid); }

        /// A race a companion may be, with the classes it can be, in the words Create accepts.
        struct RaceChoice
        {
            std::string Race;
            std::vector<std::string> Classes;
        };

        /// The races of `owner`'s faction, each with the classes it can be: everything Create takes after the name.
        [[nodiscard]] static std::vector<RaceChoice> RaceChoices(Player const* owner);

        /// The stage whose models companions play (Animus.Curriculum.Stage).
        [[nodiscard]] std::string const& CurrentStage() const { return _config.CurriculumStage; }

        /// Every curriculum stage with its arenas: the names Animus.Curriculum.Stage accepts, and so which models a
        /// companion will look for.
        [[nodiscard]] std::vector<std::string> StageList() const;

        /// A player logged out: their companion is saved and removed.
        void OnPlayerLogout(Player* player);

        void RecordDamage(Unit const* attacker, Unit const* victim, uint32 damage, DamageEffectType type);

        [[nodiscard]] bool IsEnabled() const { return _config.Enable; }

    private:
        AnimusMod() = default;

        /// The party of `owner` with its companion out, else null with `message` set.
        CompanionParty* PartyOf(Player* owner, std::string& message);

        /// A companion character that exists (created or loaded) joins the owner's party under `record`; false with
        /// `message` when it could not, the record left as it was.
        bool Adopt(Player* owner, Player* bot, CompanionRegistry::Record& record, std::string& message);
        /// Save the party's companion and the registry's record of it, then take it out of the world.
        void SaveParty(ObjectGuid owner);
        /// The words of a race and class, checked against each other and the owner's faction.
        bool ResolveRaceClass(Player const* owner, std::string_view race, std::string_view playerClass,
            uint8& raceId, uint8& classId, std::string& message) const;

        void RemoveParty(ObjectGuid owner);
        void RemoveAll();

        /// The layout of a profile at the configured stage, built on first use and kept (companions point at it).
        Curriculum::Layout const& LayoutFor(Curriculum::ClassProfile const& profile);

        AnimusConfig _config;
        ModelLibrary _models;
        CompanionRegistry _registry;
        std::unordered_map<std::string, Curriculum::Layout> _layouts;       // by model name

        std::unordered_map<ObjectGuid, std::unique_ptr<CompanionParty>> _parties;   // by owner
        std::unordered_map<ObjectGuid, CompanionParty*> _partyByBot;
    };
}

#define sAnimusMod Animus::AnimusMod::Instance()

#endif

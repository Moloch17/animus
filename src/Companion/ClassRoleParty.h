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

#ifndef ANIMUS_CLASS_ROLE_PARTY_H
#define ANIMUS_CLASS_ROLE_PARTY_H

#include "ClassRoleLayout.h"
#include "ObjectGuid.h"
#include "SeatEncoder.h"
#include "TalentBuilder.h"
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

class Player;
class Unit;

namespace Animus
{
    class MlpPolicy;
    class ModelLibrary;

    /// A player's class/role companions: characters of a class and role at the player's level, with the kit,
    /// talents, gear and supplies they were trained with, in the player's group. Each plays its class/role model for
    /// the configured stage (ModelLibrary) through the SeatEncoder.
    ///
    /// The party stands in for a training env: the owner is the scripted owner, the companions are the seats, and
    /// the enemies attacking any of them (or attacked by them or the owner) are the current pull, kept in stable
    /// enemy slots until the pull is over. A new episode (pulls cleared counted from 0, bags restocked) starts with
    /// the first pull after a quiet spell.
    ///
    /// World thread, except RecordDamage (map threads). Holds GUIDs; objects are resolved every update.
    class ClassRoleParty
    {
    public:
        enum class Status : uint8
        {
            Active,
            Dismiss,    // the owner is gone; the caller destroys the party
        };

        struct Settings
        {
            uint32 DecisionMs = 100;
        };

        explicit ClassRoleParty(ObjectGuid owner);
        ~ClassRoleParty();

        ClassRoleParty(ClassRoleParty const&) = delete;
        ClassRoleParty& operator=(ClassRoleParty const&) = delete;

        /// Create a companion of `layout` beside the owner and add it to the owner's group (creating the group when
        /// the owner has none). False with `message` set when refused.
        bool Add(Player* owner, ClassRole::Layout const& layout, std::string& message);

        Status Update(uint32 diff, Settings const& settings, ModelLibrary& models);

        /// Take every companion out of the group and the world.
        void DestroyAll();

        /// Map threads (UnitScript::DealDamage): `dealt` by `attacker` and `taken` by `victim`, where they are
        /// companions.
        void RecordDamage(ObjectGuid attacker, ObjectGuid victim, uint32 dealt, uint32 taken);

        [[nodiscard]] ObjectGuid GetOwnerGUID() const { return _owner; }
        [[nodiscard]] std::vector<ObjectGuid> GetBotGUIDs() const;
        [[nodiscard]] std::size_t Size() const { return _members.size(); }

        /// One line per companion: name, class/role, level, model state.
        [[nodiscard]] std::vector<std::string> Describe(ModelLibrary& models) const;

    private:
        struct Member
        {
            ObjectGuid Bot;
            std::string Name;
            ClassRole::Layout const* L = nullptr;
            uint8 Race = 0;
            uint8 Level = 1;
            uint8 Spec = 0;
            ClassRole::TalentBuilder::Build Build;
            uint32 FoodItem = 0;
            uint32 DrinkItem = 0;
            std::vector<uint32> Stable;

            uint32 TargetSlot = 0;
            uint32 SinceDecisionMs = 0;
            uint32 DeadMs = 0;
            bool InCombat = false;
            uint64 CombatStartMs = 0;
            uint32 LastPower = 0;
            float LastStepDamage = 0.0f;
            float LastStepPowerDelta = 0.0f;
            float LastStepDamageTaken = 0.0f;
            std::atomic<uint64> StepDamage{ 0 };
            std::atomic<uint64> StepDamageTaken{ 0 };

            std::vector<float> Obs;
            std::vector<uint8> Mask;
            bool ModelErrorLogged = false;
        };

        /// Refresh the pull: new enemies into free (or dead) slots, and the end of the pull.
        void UpdatePull(Player* owner, std::vector<Player*> const& bots);
        void UpdateMember(Member& member, Player* bot, Player* owner, uint32 diff, Settings const& settings,
            ModelLibrary& models);
        void Decide(Member& member, Player* bot, Player* owner, MlpPolicy& policy);
        /// Forge's CurrentTarget: the selected enemy, else the nearest living one (which becomes the selection).
        [[nodiscard]] Unit* CurrentTarget(Member& member, Player* bot) const;
        [[nodiscard]] ClassRole::SeatView View(Member const& member, Player* bot, Player* owner, Unit* target) const;
        void StartEpisode();
        static void Destroy(Player* bot);

        ObjectGuid _owner;
        std::vector<std::unique_ptr<Member>> _members;

        // The current pull, and the episode it belongs to.
        std::array<Unit*, ClassRole::LayoutConstants::PACK_SLOTS> _enemyUnits{};   // resolved this update
        std::vector<ObjectGuid> _enemies;                   // slot order
        uint64 _nowMs = 0;
        uint64 _pullStartMs = 0;
        uint64 _quietSinceMs = 0;
        bool _foughtBefore = false;                         // quiet time counts from the first fight's end
        bool _episodeStarted = false;
        uint32 _pullsCleared = 0;
    };
}

#endif

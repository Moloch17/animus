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

#ifndef ANIMUS_COMPANION_PARTY_H
#define ANIMUS_COMPANION_PARTY_H

#include "CompanionTalents.h"
#include "Layout.h"
#include "MlpPolicy.h"
#include "ObjectGuid.h"
#include "CurriculumTuning.h"
#include "SeatMemory.h"
#include "SeatView.h"
#include "Supplies.h"
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

    /// A player's class companions: characters of a class and role at the player's level, built as the forge
    /// builds a stage's seats (Curriculum::SeatCharacter), in the player's group. Each plays its class model for
    /// the configured stage (ModelLibrary) through the SeatEncoder.
    ///
    /// The party stands in for a training env: the owner is the scripted owner, the companions are the seats, and
    /// the enemies attacking any of them (or attacked by them or the owner) are the current pull, kept in stable
    /// enemy slots until the pull is over. A new episode (pulls cleared counted from 0, bags restocked) starts with
    /// the first pull after a quiet spell.
    ///
    /// World thread, except RecordDamage (map threads). Holds GUIDs; objects are resolved every update.
    class CompanionParty
    {
    public:
        enum class Status : uint8
        {
            Active,
            Dismiss,    // the owner is gone; the caller destroys the party
        };

        struct Settings
        {
            uint32 DecisionMs = 250;
            Curriculum::CurriculumTuning::ActionTuning Actions;     // pacing and locks, as the forge's seats have
            Curriculum::CurriculumTuning::OptionTuning Options;     // how long each durative action may run
        };

        /// What a companion is, for `.animus list` and the addon.
        struct Summary
        {
            std::string Name;
            std::string Class;          // the profile's name (hunter, deathknight)
            std::string Spec;
            uint8 Level = 1;
            std::string ModelName;      // the model it plays (hunter_duel)
            std::string Model;          // "loaded", or why its model is not
            bool Parked = false;        // out of the world while the owner flies or rides a vehicle
        };

        /// How many companions a party holds at most.
        [[nodiscard]] static std::size_t MaxSize();

        explicit CompanionParty(ObjectGuid owner);
        ~CompanionParty();

        CompanionParty(CompanionParty const&) = delete;
        CompanionParty& operator=(CompanionParty const&) = delete;

        /// Create a companion of `race` and `layout` beside the owner -- in the open world, an instance or on a
        /// transport -- and add it to the owner's group (creating the group when the owner has none). It is
        /// the owner's level, or its class's first level when that is higher (death knights: 55), and levels up with
        /// the owner. The race must be one the class allows. False with `message` set when refused.
        bool Add(Player* owner, Curriculum::Layout const& layout, Curriculum::AptitudeDemand demand, uint8 race,
            std::string& message);

        Status Update(uint32 diff, Settings const& settings, ModelLibrary& models);

        /// Take every companion out of the group and the world.
        void DestroyAll();

        /// Take one companion, by name, out of the group and the world, giving the owner back the gear they put on
        /// it. False with `message` set for a name that is not a companion's.
        bool Remove(std::string_view name, std::string& message);

        /// The owner edits a companion from the inspect window: one talent rank learned or unlearned, on the
        /// companion or its pet, or an item of theirs put on it (CompanionGear::Give). An edited companion keeps its
        /// talents and that gear through its level-ups. False with `message` set when refused.
        bool Talent(std::string_view name, uint32 talentId, bool learn, std::string& message);
        bool PetTalent(std::string_view name, uint32 talentId, bool learn, std::string& message);
        bool Equip(Player* owner, std::string_view name, uint8 bag, uint8 slot, uint8 equipSlot,
            std::string& message);

        /// A companion's pet and its talent tree, for the addon's pet tab.
        struct PetView
        {
            std::string PetName;
            uint8 Level = 0;
            uint32 FreePoints = 0;
            std::vector<CompanionTalents::PetTalent> Talents;
        };
        /// False with `message` set for a companion without a hunter pet out.
        bool Pet(std::string_view name, PetView& view, std::string& message) const;

        /// Map threads (UnitScript::DealDamage), counted as a forge seat's step damage: `damage` companion `bot` or its
        /// pet, guardian or totem dealt to `victim`, which counts only on an enemy of the current pull (a forge seat's
        /// env targets); `damage` companion `bot` took itself (its pets take their own). The pull is only changed on
        /// the world thread, never while maps update.
        void RecordDamageDealt(ObjectGuid bot, ObjectGuid victim, uint32 damage);
        void RecordDamageTaken(ObjectGuid bot, uint32 damage);

        [[nodiscard]] ObjectGuid GetOwnerGUID() const { return _owner; }
        [[nodiscard]] std::vector<ObjectGuid> GetBotGUIDs() const;
        [[nodiscard]] bool HasBot(ObjectGuid bot) const;
        [[nodiscard]] std::size_t Size() const { return _members.size(); }

        /// Every companion, in the order they were added.
        [[nodiscard]] std::vector<Summary> Summarize(ModelLibrary& models) const;
        /// One line per companion: name, class, level, model state.
        [[nodiscard]] std::vector<std::string> Describe(ModelLibrary& models) const;

    private:
        struct Member
        {
            ObjectGuid Bot;
            std::string Name;
            Curriculum::Layout const* L = nullptr;
            uint8 Race = 0;
            uint8 Level = 1;
            uint8 Spec = 0;
            /// The role it was added as, and the role of the spec it drew. Its layout is its class and covers
            /// every role the class plays, so the layout cannot answer this any more.
            /// What this companion can actually do, read off the build it ended up with once its talents were
            /// spent and its gear was on (Aptitude::Of). Everything that used to ask for a role asks this.
            Curriculum::Aptitude Apt;
            Curriculum::TalentBuilder::Build Build;
            Curriculum::BattleSupplies Supplies;
            uint32 FoodItem = 0;
            uint32 DrinkItem = 0;
            std::vector<uint32> Stable;
            ObjectGuid LastPetGuid;             // the pet given its default stance (PetBlock::DefaultStance)

            uint32 TargetSlot = 0;
            uint32 FriendSlot = Curriculum::FRIEND_SELF;    // the support block's selected friend and heal rank tier
            uint32 RankTier = 0;
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
            Curriculum::SeatMemory Memory;      // pacing, and what it has been doing (as a forge seat's)
            Curriculum::SeatOptionSet Option;   // the durative actions it is running (a bearing, hold an interrupt)
            /// Where it has been (MovementTrail), sampled in place by the move block as a forge seat's is: the
            /// model plays with what it trained with. Mutable because it is a cache and View reads a const member.
            mutable Curriculum::MovementTrail Trail;
            MlpPolicy::State Policy;            // what its model carries between decisions (memory, goal)
            int32 Goal = Curriculum::NO_GOAL;   // ... the goal of it, as its teammates see it
            bool ModelErrorLogged = false;
            bool Parked = false;                // out of the world while the owner flies or rides a vehicle
            /// The owner edited its talents, its pet's or its gear: LevelUp keeps them (and only spends the new
            /// points) instead of building the character again from scratch.
            bool Edited = false;
            std::vector<uint8> OwnerGear;       // equipment slots holding an item the owner gave it
        };

        [[nodiscard]] Member* Find(std::string_view name) const;
        /// A member's bot resolved and in the world, else null with `message` set.
        [[nodiscard]] Player* BotOf(Member const& member, std::string& message) const;
        void Destroy(Member& member, Player* owner);

        /// Refresh the pull: new enemies into free (or dead) slots, and the end of the pull.
        void UpdatePull(Player* owner, std::vector<Player*> const& bots);
        void UpdateMember(Member& member, Player* bot, Player* owner, uint32 diff, Settings const& settings,
            ModelLibrary& models);
        void Decide(Member& member, Player* bot, Player* owner, MlpPolicy& policy, Settings const& settings);
        /// The forge's pull target: the selected enemy, else the nearest living one (which becomes the selection).
        [[nodiscard]] Unit* CurrentTarget(Member& member, Player* bot) const;
        [[nodiscard]] Curriculum::SeatView View(Member const& member, Player* bot, Player* owner, Unit* target,
        Settings const& settings) const;
        /// Potions, bandages, stones and food for the member, topped up (the forge stocks every episode).
        void Restock(Member& member, Player* bot, Player* owner) const;
        /// The owner's level (or its class's first) when it has passed the member's: the character is built again at
        /// it -- talents, trainer spells, gear, supplies -- as a forge seat of that level is.
        void LevelUp(Member& member, Player* bot, Player* owner) const;
        [[nodiscard]] uint8 LevelFor(Member const& member, Player* owner) const;
        void StartEpisode(Player* owner);
        static void Destroy(Player* bot);
        /// LevelUp for a companion the owner edited: talents and pet talents taken again at the new level, new
        /// points spent along the standard build, the owner's gear kept.
        void LevelUpEdited(Member& member, Player* bot, Player* owner) const;

        ObjectGuid _owner;
        std::vector<std::unique_ptr<Member>> _members;

        // The current pull, and the episode it belongs to.
        std::array<Unit*, Curriculum::PACK_SLOTS> _enemyUnits{};    // resolved this update
        std::vector<ObjectGuid> _enemies;                   // slot order
        uint64 _nowMs = 0;
        uint64 _pullStartMs = 0;
        uint64 _episodeStartMs = 0;
        uint64 _quietSinceMs = 0;
        bool _foughtBefore = false;                         // quiet time counts from the first fight's end
        bool _episodeStarted = false;
        uint32 _pullsCleared = 0;
    };
}

#endif

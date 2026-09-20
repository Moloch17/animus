/*
 * This file is part of the Animus Forge project, based on AzerothCore.
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

#ifndef ANIMUS_LIB_CURRICULUM_STAGE_DEFINITION_H
#define ANIMUS_LIB_CURRICULUM_STAGE_DEFINITION_H

#include "Block.h"
#include "ClassRoleProfile.h"
#include "Position.h"
#include <string>
#include <string_view>
#include <vector>

namespace Animus::Curriculum
{
    /// Who the learned agents of an env are.
    enum class SeatPlan : uint8
    {
        Solo,           // one seat
        Party,          // one group (1-GROUP_SEATS with a character each episode): a tank, a healer and damage
        Mirror,         // two seats that fight each other (self-play)
        Raid,           // the arena's seats as RAID_GROUPS groups of GROUP_SEATS: a tank and a healer per group
        Teams,          // TEAM_COUNT sides of TEAM_SEATS against each other (self-play), each side a group
    };

    /// What the seats fight.
    enum class Opposition : uint8
    {
        Creature,       // one same-level creature spawned out of aggro range
        Pulls,          // packs of creatures (see PullSchedule)
        ScriptedPlayer, // an enemy player played by a script
        MirrorSeat,     // the other seat (SeatPlan::Mirror)
        Ambush,         // only ambushers: scripted enemy players attacking the owner (ArenaDefinition::Ambushers)
        Travel,         // a place to get to (ArenaDefinition::Flying for one best reached in the air)
        Flag,           // Warsong Gulch's rules between the two mirror seats: take the other's flag home
    };

    enum class PullSchedule : uint8
    {
        None,
        SinglePack,     // one pack; the episode ends when it is cleared
        Gauntlet,       // pull after pull with a break between, until the episode ends
        Sequence,       // a known run of pulls in a fixed order, the same every episode, won by clearing the last
    };

    /// Most arenas a stage can mix (the critic state has one column per arena).
    constexpr uint32 MAX_ARENAS = 8;

    /// Most ambushers an arena can have; they take enemy slots the pulls leave free.
    constexpr uint32 MAX_AMBUSHERS = 2;

    /// One situation an episode of a stage can be: who the seats are, what they fight, and how long it lasts. Every
    /// episode of a stage draws one of its arenas by weight, so one stage (and one policy) can train PvE and PvP
    /// together. A stage with a single arena is a stage of one situation.
    struct ArenaDefinition
    {
        std::string Name;               // unique in the stage: episode info, stage.json, tuning keys
        uint32 Weight = 1;              // share of episodes; <TuningPrefix>Arena.<stage>.<name>.Weight
        SeatPlan Seats = SeatPlan::Solo;
        Opposition Against = Opposition::Creature;
        PullSchedule Schedule = PullSchedule::None;
        bool Owner = false;             // a scripted owner the seats fight for
        bool PartyGroup = false;        // the owner and seats form a core group
        bool Pvp = false;               // against players: resilience gear, no resurrecting oneself
        uint32 EpisodeSeconds = 0;      // episode length; 0 = StageSettings::EpisodeSeconds
        /// Most scripted enemy players that ambush the owner (1 to this many, MAX_AMBUSHERS at most): mid-episode
        /// beside pulls, or from the start against Opposition::Ambush. 0 = none.
        uint32 Ambushers = 0;
        /// Roles the first seats must play (entry i is seat i); the rest are drawn as usual. A drill stage fixes
        /// the seat it is about -- a tank that has to hold what it pulls, a healer that has to keep a group up --
        /// where the ordinary party draws every role and the lesson is smeared over whoever happened to play it.
        std::vector<Role> SeatRoles{};
        /// A director commands each side: one more agent a side, choosing the team's posture, the enemy it
        /// concentrates on, the shape it takes and whose turn the next duty is. Off by default -- a solo arena
        /// would pay for an agent with nothing to say.
        bool Directed = false;
        /// The director is an agent that learns rather than the scripted one (DirectorEncounter). Costs
        /// TEAM_COUNT more agents an env across the whole stage -- the spec is fixed, so a stage with one
        /// learned-directed arena carries the pair in every episode and marks them absent where they are not
        /// used. Ignored unless Directed.
        bool DirectorLearned = false;
        /// The director may name a place to go to (PlaceAnchor, PlaceOffset, PlaceRing). Off by default: the
        /// thirteen actions stay masked in an arena with nowhere worth sending anyone, so no stage pays
        /// exploration for a vocabulary it cannot use. Ignored unless Directed.
        bool Places = false;
        /// Seats a side in a Teams arena: 2 and 3 are the arena formats, 10 a battleground side. Ignored by
        /// every other seat plan.
        uint32 TeamSeats = TEAM_SEATS;
        /// Every pull contains a creature that puts something on the ground (OpponentPool::RandomHazardCaster),
        /// whatever rung the ladder is on. The pack ladder only reaches hazards at rung 3, so a class/role that
        /// stalls below it never meets one; this makes stepping out of a hazard learnable on its own.
        bool Hazards = false;
        /// Travel: the objective is far enough that flying beats riding (the stage's map must allow flight).
        bool Flying = false;
        /// Travel: no mount may be summoned, so the trip is made on the seat's own legs. What is left to learn
        /// is what a player does before it can ride: the speed cooldowns (Sprint, Dash, Travel Form, Aspect of
        /// the Cheetah), not stopping, and not wandering off the path. Mounting is masked, not merely unpaid,
        /// because a masked action cannot be explored into and the lesson stays clean.
        bool OnFoot = false;
        /// Levels added to the scripted enemy player's own, on top of Opponent.LevelSpread. A drill about
        /// getting away needs a fight the seat cannot win; every other arena wants an even match and leaves
        /// this at 0. Ignored unless the opposition is a scripted player.
        int32 OpponentLevelBonus = 0;

        [[nodiscard]] uint32 SeatCount() const;
    };

    /// One curriculum stage: its own scenario (`stage1_duel`, ...), its blocks and the arenas its episodes are.
    ///
    /// A stage extends one earlier stage, whose best model seeds it: the base's blocks this stage keeps are seeded
    /// block by block (their features and actions may move), dropped ones are left behind and new ones start fresh.
    /// Several stages may extend the same base, so the curriculum is a tree. A merge stage also lists other earlier
    /// stages (Merges): the blocks only they have are seeded from them, and the learner can distill each of their
    /// arenas from their models, joining branches of the tree again.
    struct StageDefinition
    {
        std::string Name;               // the scenario name
        std::string Suffix;             // added to a class/role's name for the stage's models (warrior_dps_duel)
        std::string Extends;            // the stage it builds on and seeds from (the trunk); empty for the first
        std::vector<std::string> Merges{}; // further stages it seeds the blocks only they have from
        std::string Summary;
        std::vector<BlockId> Blocks;    // in layout order: every block any of its arenas needs
        std::vector<ArenaDefinition> Arenas;
        bool InDefaultQueue = true;     // trained by an empty AnimusForge.Queue (false: only when named)
        /// Where its envs are: 0 = the host's StageSettings::SpawnMapId and SpawnPosition. A continent (not
        /// instanceable) is shared by every env, so each env gets its own phase and one of SpawnPoints by env index.
        uint32 MapId = 0;
        std::vector<Position> SpawnPoints{};
        /// Where a flag arena's bases are, one per side. Empty: the second base is searched for, BaseMin-BaseMax
        /// from the first, which is what a stage with no map of its own has to do. Warsong Gulch has real ones.
        std::vector<Position> FlagBases{};
        /// The lowest level its characters may be (flying needs 60), raising a host's fixed level too.
        uint8 MinLevel = 0;

        [[nodiscard]] bool Has(BlockId block) const;
        /// Seats per env: the largest arena's.
        [[nodiscard]] uint32 SeatCount() const;
        /// Whether any arena of the stage satisfies `predicate` (encounters, info columns and pools it needs).
        template <typename Predicate>
        [[nodiscard]] bool AnyArena(Predicate predicate) const
        {
            for (ArenaDefinition const& arena : Arenas)
                if (predicate(arena))
                    return true;
            return false;
        }
    };

    /// Every curriculum stage, every base before the stages that extend it. Invalid definitions (an unknown or later
    /// base, a repeated block, parts that need a missing block) are logged and left out.
    [[nodiscard]] std::vector<StageDefinition> const& CurriculumStages();

    [[nodiscard]] StageDefinition const* FindStage(std::string_view name);
}

#endif

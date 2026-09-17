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

/*
 * The curriculum, a tree: every stage extends one earlier stage (and seeds from it), keeping the base's
 * blocks it needs and adding its own.
 *
 *   duel ─┬─ pack ─ gauntlet ─ companion ─ party ─┬─ crossroads     (PvE ...
 *         ├─ pvp ─ arena ─────────────────────────┘                  ... and PvP, merged)
 *         └─ travel ─┬─ flight                                       (getting somewhere)
 *                    └─ (with arena) flag                            (Warsong Gulch's rules)
 *
 * Scenario names carry the stage's number (stage1_duel ... stage8_crossroads), model names only its suffix (_duel). The
 * duel is the first stage: nothing seeds it.
 *
 * A stage's episodes are its arenas (see ArenaDefinition): each episode draws one by weight, so a stage can mix PvE
 * and PvP situations over the union of their blocks. Stages 1-7 have one arena each.
 *
 * Adding a stage is one entry here (plus new blocks or encounters only if it needs new features) and a learner
 * config, configs/<name>.yaml.
 */

#include "StageDefinition.h"
#include "Log.h"
#include "AreaDefines.h"
#include <algorithm>

namespace
{
    using namespace Animus::Curriculum;

    std::vector<StageDefinition> Definitions()
    {
        using enum BlockId;

        std::vector<StageDefinition> stages;

        stages.push_back({
            .Name = "stage1_duel",
            .Suffix = "_duel",
            .Extends = "",
            .Summary = "a same-level creature out of aggro range: close in and kill it fast, taking little damage",
            .Blocks = { Core, Duel, Pet },
            // 90 s: running out of time is a lost fight (Duel.Timeout), and a healer or tank against a creature with
            // twice the usual health needs half a minute to kill it after a few seconds of closing in.
            .Arenas = { { .Name = "duel", .Against = Opposition::Creature, .EpisodeSeconds = 90 } },
        });

        stages.push_back({
            .Name = "stage2_pack",
            .Suffix = "_pack",
            .Extends = "stage1_duel",
            .Summary = "a pack of 2-4, casters included, usually linked: targets, interrupts, crowd control",
            .Blocks = { Core, Duel, Pet, Pack },
            // 150 s: running out of time is a lost fight (Pulls.Timeout), and a pack is up to four of the duel's
            // creatures. stage1_duel's policy took 17 s a kill and its baseline 23 s, so four take 70-90 s before the
            // approach; the duel's 90 s (or the host's 60) would lose packs to the clock that play could win.
            .Arenas = { { .Name = "pack", .Against = Opposition::Pulls, .Schedule = PullSchedule::SinglePack,
                .EpisodeSeconds = 150 } },
        });

        stages.push_back({
            .Name = "stage3_gauntlet",
            .Suffix = "_gauntlet",
            .Extends = "stage2_pack",
            .Summary = "pull after pull with short breaks: heals, food and drink",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet },
            // 450 s: pull after pull is the point. At the host's 60 s a break of 8-20 s before each pull left two or
            // three of them, with nothing to recover for. Pulls come to the seat when it waits too long and come
            // sooner as it clears them, so seven and a half minutes hold eight or more, and the solo gauntlet is won
            // by lasting to the end with Pulls.SoloGauntletWinPulls cleared (PullTuning::SoloGauntlet*).
            .Arenas = { { .Name = "gauntlet", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                .EpisodeSeconds = 450 } },
        });

        stages.push_back({
            .Name = "stage4_companion",
            .Suffix = "_companion",
            .Extends = "stage3_gauntlet",
            .Summary = "the gauntlet beside a scripted owner: follow, assist, guard and heal it",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion },
            .Arenas = { { .Name = "companion", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                .Owner = true } },
        });

        stages.push_back({
            .Name = "stage5_party",
            .Suffix = "_party",
            .Extends = "stage4_companion",
            .Summary = "four learned seats and the scripted owner against elite-heavy pulls",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion, Party },
            .Arenas = { { .Name = "party", .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .Owner = true, .PartyGroup = true } },
        });

        // The PvP branch: off the duel, without the PvE blocks it would never fill.
        stages.push_back({
            .Name = "stage6_pvp",
            .Suffix = "_pvp",
            .Extends = "stage1_duel",
            .Summary = "one-on-one against a scripted enemy player",
            .Blocks = { Core, Duel, Pet, Pvp },
            .Arenas = { { .Name = "pvp_scripted", .Against = Opposition::ScriptedPlayer, .Pvp = true } },
        });

        stages.push_back({
            .Name = "stage7_arena",
            .Suffix = "_arena",
            .Extends = "stage6_pvp",
            .Summary = "self-play one-on-one: two learned seats of any classes",
            .Blocks = { Core, Duel, Pet, Pvp },
            .Arenas = { { .Name = "arena_1v1", .Seats = SeatPlan::Mirror, .Against = Opposition::MirrorSeat,
                .Pvp = true } },
        });

        // The crossroads: both branches join. It extends the party (the trunk and every PvE block), takes the pvp
        // block from the arena, and each parent teaches the arenas it trained on. Two new situations need PvE and PvP in one
        // episode: an ambush of the owner in the middle of the gauntlet, and a lone enemy player attacking the owner.
        // Every PvE arena plays long episodes; the one-on-ones stay short.
        stages.push_back({
            .Name = "stage8_crossroads",
            .Suffix = "_crossroads",
            .Extends = "stage5_party",
            .Merges = {
                "stage7_arena", "stage6_pvp", "stage4_companion", "stage3_gauntlet", "stage1_duel",
            },
            .Summary = "PvE and PvP in one policy: every earlier situation, an ambush mid-gauntlet and a ganked owner",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion, Party, Pvp, Context, Hostiles },
            .Arenas = {
                { .Name = "companion", .Weight = 20, .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                    .Owner = true, .EpisodeSeconds = 300 },
                { .Name = "party", .Weight = 20, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                    .Schedule = PullSchedule::Gauntlet, .Owner = true, .PartyGroup = true, .EpisodeSeconds = 300 },
                { .Name = "arena_1v1", .Weight = 15, .Seats = SeatPlan::Mirror, .Against = Opposition::MirrorSeat,
                    .Pvp = true, .EpisodeSeconds = 60 },
                { .Name = "pvp_scripted", .Weight = 10, .Against = Opposition::ScriptedPlayer, .Pvp = true,
                    .EpisodeSeconds = 60 },
                { .Name = "gauntlet", .Weight = 10, .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                    .EpisodeSeconds = 450 },
                { .Name = "duel", .Weight = 5, .Against = Opposition::Creature, .EpisodeSeconds = 60 },
                { .Name = "ambush", .Weight = 15, .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                    .Owner = true, .EpisodeSeconds = 300, .Ambushers = 2 },
                { .Name = "escort_duel", .Weight = 5, .Against = Opposition::Ambush, .Owner = true,
                    .EpisodeSeconds = 90, .Ambushers = 1 },
            },
        });

        // Travel: getting somewhere, off the duel. Characters of 20 and up ride; the policy learns when a trip is worth
        // a mount's cast time, and to arrive on foot, ready to fight.
        stages.push_back({
            .Name = "stage9_travel",
            .Suffix = "_travel",
            .Extends = "stage1_duel",
            .Summary = "a place 60-320 yd away by path: mount when it pays, get there, arrive on foot",
            .Blocks = { Core, Duel, Pet, Travel },
            .Arenas = { { .Name = "travel", .Against = Opposition::Travel, .EpisodeSeconds = 150 } },
            .MinLevel = 20,
        });

        // Flight: Outland's Nagrand, where flying mounts fly (a battleground never allows them). The envs share the
        // continent, each in its own phase, spread over open ground.
        stages.push_back({
            .Name = "stage10_flight",
            .Suffix = "_flight",
            .Extends = "stage9_travel",
            .Summary = "a place 350-700 yd away in Nagrand: take off, fly over what is in the way, land, dismount",
            .Blocks = { Core, Duel, Pet, Travel },
            .Arenas = { { .Name = "flight", .Against = Opposition::Travel, .EpisodeSeconds = 180, .Flying = true } },
            .MapId = MAP_OUTLAND,
            .SpawnPoints = {
                { -1684.0f, 7167.0f, 2.0f, 0.0f }, { -1060.0f, 7618.0f, 28.0f, 0.0f },
                { -1820.0f, 8828.0f, 28.0f, 0.0f }, { -1226.0f, 8834.0f, 46.0f, 0.0f },
                { -2581.0f, 6582.0f, 11.0f, 0.0f }, { -1308.0f, 6816.0f, 36.0f, 0.0f },
                { -1092.0f, 7304.0f, 33.0f, 0.0f }, { -2533.0f, 7693.0f, -23.0f, 0.0f },
            },
            .MinLevel = 60,
        });

        // Warsong Gulch's rules between two learned seats (self-play): take the other side's flag home, return one's
        // own, stop the carrier. Mounting between the bases and being dismounted by the flag come from travel; the
        // fight from the arena.
        stages.push_back({
            .Name = "stage11_flag",
            .Suffix = "_flag",
            .Extends = "stage7_arena",
            .Merges = { "stage9_travel" },
            .Summary = "capture the flag one-on-one: bases 100-180 yd apart, first to three captures",
            .Blocks = { Core, Duel, Pet, Pvp, Travel, Flag },
            .Arenas = { { .Name = "flag", .Seats = SeatPlan::Mirror, .Against = Opposition::Flag, .Pvp = true,
                .EpisodeSeconds = 300 } },
            .MinLevel = 20,
        });

        // A pilot of arena mixing and merging, not part of the curriculum: the duel and the scripted enemy player in
        // one stage, merging the two stages that trained them (each teaches its arena). Trained only when named
        // (forge start mix_duel_pvp).
        stages.push_back({
            .Name = "mix_duel_pvp",
            .Suffix = "_mix",
            .Extends = "stage6_pvp",
            .Merges = { "stage1_duel" },
            .Summary = "pilot arena mix: half the episodes a creature duel, half a scripted enemy player",
            .Blocks = { Core, Duel, Pet, Pvp },
            .Arenas = {
                { .Name = "duel", .Against = Opposition::Creature },
                { .Name = "pvp_scripted", .Against = Opposition::ScriptedPlayer, .Pvp = true },
            },
            .InDefaultQueue = false,
        });

        return stages;
    }

    /// Why `arena` cannot be played with `stage`'s blocks, or empty.
    std::string ArenaProblem(StageDefinition const& stage, ArenaDefinition const& arena)
    {
        bool const pulls = arena.Against == Opposition::Pulls;
        bool const ambushOnly = arena.Against == Opposition::Ambush;
        bool const flag = arena.Against == Opposition::Flag;
        bool const duelPlayer = arena.Against == Opposition::ScriptedPlayer || arena.Against == Opposition::MirrorSeat
            || flag;
        bool const player = duelPlayer || arena.Ambushers > 0;

        if (pulls != (arena.Schedule != PullSchedule::None))
            return "a pull schedule goes with pulls, and only with pulls";
        if (pulls && !stage.Has(BlockId::Pack))
            return "pulls need the pack block";
        if (arena.Schedule == PullSchedule::Gauntlet && !stage.Has(BlockId::Gauntlet))
            return "the gauntlet schedule needs the gauntlet block";
        if (arena.Owner && (!(pulls || ambushOnly) || !stage.Has(BlockId::Companion)))
            return "an owner needs pulls or an ambush, and the companion block";
        if (arena.PartyGroup && (!arena.Owner || arena.Seats != SeatPlan::Party || !stage.Has(BlockId::Party)))
            return "a party group needs an owner, party seats and the party block";
        if ((arena.Seats == SeatPlan::Mirror) != (arena.Against == Opposition::MirrorSeat || flag))
            return "mirror seats go with fighting the mirror seat or a flag match, and only with them";
        if (arena.Ambushers > MAX_AMBUSHERS)
            return "at most " + std::to_string(MAX_AMBUSHERS) + " ambushers";
        if (arena.Ambushers > 0 && !(pulls || ambushOnly))
            return "ambushers join pulls, or are the whole fight (Opposition::Ambush)";
        if (arena.Ambushers > 0 && (!arena.Owner || !stage.Has(BlockId::Pack)))
            return "ambushers attack an owner and take enemy slots (the pack block)";
        if (ambushOnly && arena.Ambushers != 1)
            return "an ambush without pulls has exactly one ambusher (a one-on-one reward)";
        if (player && !stage.Has(BlockId::Pvp))
            return "fighting a player needs the pvp block";
        if (duelPlayer && !arena.Pvp)
            return "a one-on-one against a player is pvp";
        if (arena.Pvp && !player)
            return "a pvp arena fights a player";
        bool const travel = arena.Against == Opposition::Travel;
        if (travel && !stage.Has(BlockId::Travel))
            return "travel needs the travel block";
        if (travel && (arena.Seats != SeatPlan::Solo || arena.Owner || arena.Pvp || arena.Ambushers > 0))
            return "travel is one seat on its own";
        if (arena.Flying && !travel)
            return "only a travel arena flies";
        if (flag && (!stage.Has(BlockId::Travel) || !stage.Has(BlockId::Flag)))
            return "a flag match needs the travel and flag blocks";

        return {};
    }

    /// Why `stage` cannot be used, or empty. `valid` holds the stages accepted so far.
    std::string Problem(StageDefinition const& stage, std::vector<StageDefinition> const& valid)
    {
        if (stage.Blocks.empty() || stage.Blocks.front() != BlockId::Core)
            return "its blocks must start with core";

        for (std::size_t i = 0; i < stage.Blocks.size(); ++i)
            if (std::find(stage.Blocks.begin() + i + 1, stage.Blocks.end(), stage.Blocks[i]) != stage.Blocks.end())
                return "a block is listed twice";

        // The base only has to exist: seeding maps the base's blocks to this stage's by name (stage.json spans), so a
        // stage may drop base blocks it does not need and several stages may share a base.
        auto const earlier = [&valid](std::string const& name)
        {
            return std::any_of(valid.begin(), valid.end(), [&name](StageDefinition const& other)
            {
                return other.Name == name;
            });
        };

        if (!stage.Extends.empty() && !earlier(stage.Extends))
            return "it extends " + stage.Extends + ", which is not an earlier valid stage";

        for (std::string const& merge : stage.Merges)
        {
            if (stage.Extends.empty())
                return "a merge needs a stage it extends (the trunk)";
            if (merge == stage.Extends || std::count(stage.Merges.begin(), stage.Merges.end(), merge) > 1)
                return "it merges " + merge + " twice";
            if (!earlier(merge))
                return "it merges " + merge + ", which is not an earlier valid stage";
        }

        if (!stage.Has(BlockId::Duel))
            return "every stage fights something that fights back, which needs the duel block";

        if (stage.Arenas.empty() || stage.Arenas.size() > MAX_ARENAS)
            return "it needs 1 to " + std::to_string(MAX_ARENAS) + " arenas";

        for (std::size_t i = 0; i < stage.Arenas.size(); ++i)
        {
            ArenaDefinition const& arena = stage.Arenas[i];
            if (arena.Name.empty())
                return "an arena has no name";

            for (std::size_t j = i + 1; j < stage.Arenas.size(); ++j)
                if (stage.Arenas[j].Name == arena.Name)
                    return "arena " + arena.Name + " is listed twice";

            if (std::string const problem = ArenaProblem(stage, arena); !problem.empty())
                return "arena " + arena.Name + ": " + problem;
        }

        return {};
    }
}

uint32 Animus::Curriculum::ArenaDefinition::SeatCount() const
{
    switch (Seats)
    {
        case SeatPlan::Party:  return MAX_SEATS;
        case SeatPlan::Mirror: return 2;
        case SeatPlan::Solo:   break;
    }

    return 1;
}

bool Animus::Curriculum::StageDefinition::Has(BlockId block) const
{
    return std::find(Blocks.begin(), Blocks.end(), block) != Blocks.end();
}

uint32 Animus::Curriculum::StageDefinition::SeatCount() const
{
    uint32 seats = 1;
    for (ArenaDefinition const& arena : Arenas)
        seats = std::max(seats, arena.SeatCount());

    return seats;
}

std::vector<Animus::Curriculum::StageDefinition> const& Animus::Curriculum::CurriculumStages()
{
    static std::vector<StageDefinition> const stages = []()
    {
        std::vector<StageDefinition> valid;
        for (StageDefinition& stage : Definitions())
        {
            if (std::string const problem = Problem(stage, valid); !problem.empty())
            {
                LOG_ERROR("module.animus", "Stage {} is left out: {}", stage.Name, problem);
                continue;
            }

            valid.push_back(std::move(stage));
        }

        return valid;
    }();

    return stages;
}

Animus::Curriculum::StageDefinition const* Animus::Curriculum::FindStage(std::string_view name)
{
    for (StageDefinition const& stage : CurriculumStages())
        if (stage.Name == name)
            return &stage;

    return nullptr;
}

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
 * Scenario names carry the stage's number (stage1_duel ... stage18_warsong), model names only its suffix
 * (_duel). The
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

        // Something on the ground, in every pull. Hazards exist already -- the pack ladder draws a hazard caster
        // from rung 3 and self-play produces them by the spell (stage15_arena measured 3.1 s of hazard an episode,
        // stage4_gauntlet 1.1 s) -- but a class/role that stalls below rung 3 never meets one, and a second an
        // episode is thin to learn from. Here every pull has one, at stage 2's difficulty, so walking out of it is
        // the thing being learned rather than a detail of a harder fight.
        stages.push_back({
            .Name = "stage3_hazards",
            .Suffix = "_hazards",
            .Extends = "stage2_pack",
            .Summary = "a pack with something on the ground in every pull: see it, and walk out of it",
            .Blocks = { Core, Duel, Pet, Pack, Support },
            .Arenas = { { .Name = "hazards", .Against = Opposition::Pulls, .Schedule = PullSchedule::SinglePack,
                .EpisodeSeconds = 120, .Hazards = true } },
            .InDefaultQueue = false,
        });

        stages.push_back({
            .Name = "stage4_gauntlet",
            .Suffix = "_gauntlet",
            .Extends = "stage2_pack",
            .Summary = "pull after pull with short breaks: heals, food and drink",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Support },
            // 450 s: pull after pull is the point. At the host's 60 s a break of 8-20 s before each pull left two or
            // three of them, with nothing to recover for. Pulls come to the seat when it waits too long and come
            // sooner as it clears them, so seven and a half minutes hold eight or more, and the solo gauntlet is won
            // by lasting to the end with Pulls.SoloGauntletWinPulls cleared (PullTuning::SoloGauntlet*).
            .Arenas = { { .Name = "gauntlet", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                .EpisodeSeconds = 450 } },
        });

        // A planned run: the same eight pulls in the same order every episode, ending on an elite pack two levels
        // above. Nothing about the fights is left to learn -- stage 3 taught them -- so what is left is the plan:
        // what to spend on the opener, what to keep for the last pull, when the breather is a rest and when it is a
        // chance to get ahead. Won by clearing the last pull alive; the clock running out is a loss however far it
        // got. Trained by name, after stage 3.
        stages.push_back({
            .Name = "stage5_endurance",
            .Suffix = "_endurance",
            .Extends = "stage4_gauntlet",
            .Summary = "a known run of eight pulls, won by finishing it",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Support },
            .Arenas = { { .Name = "endurance", .Against = Opposition::Pulls, .Schedule = PullSchedule::Sequence,
                .EpisodeSeconds = 900 } },
            .InDefaultQueue = false,
        });

        // Travel: getting somewhere, off the duel. Characters of 20 and up ride; the policy learns when a trip is worth
        // a mount's cast time, and to arrive on foot, ready to fight.
        //
        // The Barrens, not the arena the fighting stages spawn in: a trip needs open, pathable ground in every
        // direction for a few hundred yards, and that arena is a corner pocket with none -- the nearest walkable
        // ground outside it is 350 yd off and 50 yd up a hillside, past the objective search's reach, so no episode
        // could ever be built there. The envs share the continent, each in its own phase, spread over the flats.
        stages.push_back({
            .Name = "stage6_travel",
            .Suffix = "_travel",
            .Extends = "stage1_duel",
            .Summary = "a place 60-320 yd away by path: mount when it pays, get there, arrive on foot",
            .Blocks = { Core, Duel, Pet, Travel },
            .Arenas = { { .Name = "travel", .Against = Opposition::Travel, .EpisodeSeconds = 150 } },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = {
                { -872.0f, -2642.0f, 92.0f, 0.0f }, { -2298.0f, -1948.0f, 96.0f, 0.0f },
                { -1967.0f, -2544.0f, 94.0f, 0.0f }, { -2605.0f, -2286.0f, 92.0f, 0.0f },
                { -609.0f, -1614.0f, 94.0f, 0.0f }, { -881.0f, -3221.0f, 92.0f, 0.0f },
                { -3077.0f, -1786.0f, 92.0f, 0.0f }, { -3115.0f, -2352.0f, 94.0f, 0.0f },
            },
            .MinLevel = 20,
        });

        // Flight: Outland's Nagrand, where flying mounts fly (a battleground never allows them). The envs share the
        // continent, each in its own phase, spread over open ground.
        stages.push_back({
            .Name = "stage7_flight",
            .Suffix = "_flight",
            .Extends = "stage6_travel",
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

        stages.push_back({
            .Name = "stage8_companion",
            .Suffix = "_companion",
            .Extends = "stage4_gauntlet",
            .Summary = "the gauntlet beside a scripted owner: follow, assist, guard and heal it",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion, Support },
            // 450 s, as the solo gauntlet: without its own length the arena took the host's 60 s, two or three pulls
            // with nothing to recover for and no win to reach (Pulls.OwnerWinPulls).
            .Arenas = { { .Name = "companion", .Against = Opposition::Pulls, .Schedule = PullSchedule::Gauntlet,
                .Owner = true, .EpisodeSeconds = 450 } },
        });

        stages.push_back({
            .Name = "stage9_party",
            .Suffix = "_party",
            .Extends = "stage8_companion",
            .Summary = "four learned seats and the scripted owner against elite-heavy pulls",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "party", .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .Owner = true, .PartyGroup = true, .EpisodeSeconds = 450 } },
        });

        // Holding what the group pulls. Tanks exist in stages 5, 8, 13 and 14, but the stage is won by the clear,
        // so a tank that loses an add to the healer and takes it back is scored the same as one that never lost it.
        // Here seat 0 is always the tank (ArenaDefinition::SeatRoles) and the pulls are a party's, so what the
        // episode is about is the threat table -- which the seat can now read (Encoding::ThreatShare).
        //
        // Read its scores knowing that the hazard charge lands about four times harder on a tank than on a ranged
        // seat (stage15_arena: paladin_tank -0.834 an episode against priest_dps -0.198), because a tank cannot walk
        // out of what it is holding an enemy in. That is Hazards.Standing being tuned for a seat with a choice.
        stages.push_back({
            .Name = "stage10_tanking",
            .Suffix = "_tanking",
            .Extends = "stage9_party",
            .Summary = "a fixed tank seat beside its group: hold what the pull brings, and keep it off the others",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "tanking", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .Owner = true, .PartyGroup = true, .EpisodeSeconds = 300,
                .SeatRoles = { Role::Tank } } },
            .InDefaultQueue = false,
        });

        // Keeping a group up when the damage outruns one heal. Stage 5 has healers, but its win is the clear and
        // stage 4 measured the seat putting 13% of its healing into the owner: triage is never the episode.
        //
        // Before reading its numbers, know that stage 5 carries a resurrection exploit this drill inherits: nothing
        // clears m_resurrectGUID, so one landed Rebirth makes every later death of that ally an instant free
        // resurrect that pays RewardTerm::Revive again (stage8_companion measured druid_dps at 38.4 revives an
        // episode, 88% of its return). A forced healer seat will find it faster than anything else in the
        // curriculum. Fix that before trusting a triage score.
        stages.push_back({
            .Name = "stage11_triage",
            .Suffix = "_triage",
            .Extends = "stage9_party",
            .Summary = "a fixed healer seat beside its group: keep the hurt one up, and spend mana to do it",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "triage", .Weight = 1, .Seats = SeatPlan::Party, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .Owner = true, .PartyGroup = true, .EpisodeSeconds = 300,
                .SeatRoles = { Role::Heal } } },
            .InDefaultQueue = false,
        });

        // The raid branch: MAX_SEATS learned seats as RAID_GROUPS groups of GROUP_SEATS, each group with its own
        // tank and healer (SeatPlan::Raid). A raid is not a bigger party -- it is many seats around one large enemy,
        // which is why the opponents are an elite and its adds rather than a pack per seat, and why the mechanics a
        // seat can now read (a cast worth interrupting, something on the ground, where it stands on the threat
        // table) matter far more here than they do alone.
        //
        // NOT in the default queue, and not runnable at the usual env count: 40 seats an env is 40 bots an env, so
        // AnimusForge.Envs has to come down roughly in proportion (a few dozen envs, not 128) before either of these
        // is started. Train by name: `forge start stage12_raid_single`.
        stages.push_back({
            .Name = "stage12_raid_single",
            .Suffix = "_raid",
            .Extends = "stage9_party",
            .Summary = "a raid of eight groups against one elite and its adds, won or lost as the single pack is",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "raid_single", .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::SinglePack, .EpisodeSeconds = 300 } },
            .InDefaultQueue = false,
        });

        // The raid's endurance: pull after pull with recovery between, which is what a wing of a raid instance is
        // before the boss of it. Seeded from the single fight, as the gauntlet is from the pack.
        stages.push_back({
            .Name = "stage13_raid_gauntlet",
            .Suffix = "_raidrun",
            .Extends = "stage12_raid_single",
            .Summary = "a raid clearing pull after pull, recovering between them",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion, Party, Support },
            .Arenas = { { .Name = "raid_gauntlet", .Seats = SeatPlan::Raid, .Against = Opposition::Pulls,
                .Schedule = PullSchedule::Gauntlet, .EpisodeSeconds = 600 } },
            .InDefaultQueue = false,
        });

        // The PvP branch: off the duel, without the PvE blocks it would never fill.
        stages.push_back({
            .Name = "stage14_pvp",
            .Suffix = "_pvp",
            .Extends = "stage1_duel",
            .Summary = "one-on-one against a scripted enemy player",
            .Blocks = { Core, Duel, Pet, Pvp },
            .Arenas = { { .Name = "pvp_scripted", .Against = Opposition::ScriptedPlayer, .Pvp = true } },
        });

        stages.push_back({
            .Name = "stage15_arena",
            .Suffix = "_arena",
            .Extends = "stage14_pvp",
            .Summary = "self-play one-on-one: two learned seats of any classes",
            .Blocks = { Core, Duel, Pet, Pvp },
            .Arenas = { { .Name = "arena_1v1", .Seats = SeatPlan::Mirror, .Against = Opposition::MirrorSeat,
                .Pvp = true } },
        });

        // The crossroads: both branches join. It extends the party (the trunk and every PvE block), takes the pvp
        // block from the arena, and each parent teaches the arenas it trained on. Two new situations need PvE
        // and PvP in one
        // episode: an ambush of the owner in the middle of the gauntlet, and a lone enemy player attacking the owner.
        // Every PvE arena plays long episodes; the one-on-ones stay short.
        stages.push_back({
            .Name = "stage16_crossroads",
            .Suffix = "_crossroads",
            .Extends = "stage9_party",
            .Merges = {
                "stage15_arena", "stage14_pvp", "stage8_companion", "stage4_gauntlet", "stage1_duel",
            },
            .Summary = "PvE and PvP in one policy: every earlier situation, an ambush mid-gauntlet and a ganked owner",
            .Blocks = { Core, Duel, Pet, Pack, Gauntlet, Companion, Party, Pvp, Context, Hostiles, Support },
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

        // Warsong Gulch's rules between two learned seats (self-play): take the other side's flag home, return one's
        // own, stop the carrier. Mounting between the bases and being dismounted by the flag come from travel; the
        // fight from the arena.
        //
        // The Barrens, for stage 9's reason: the second base is placed by the same objective search, 100-180 yd from
        // the first, and only open ground has room for it.
        stages.push_back({
            .Name = "stage17_flag",
            .Suffix = "_flag",
            .Extends = "stage15_arena",
            .Merges = { "stage6_travel" },
            .Summary = "capture the flag one-on-one: bases 100-180 yd apart, first to three captures",
            .Blocks = { Core, Duel, Pet, Pvp, Travel, Flag },
            .Arenas = { { .Name = "flag", .Seats = SeatPlan::Mirror, .Against = Opposition::Flag, .Pvp = true,
                .EpisodeSeconds = 300 } },
            .MapId = MAP_KALIMDOR,
            .SpawnPoints = {
                { -872.0f, -2642.0f, 92.0f, 0.0f }, { -2298.0f, -1948.0f, 96.0f, 0.0f },
                { -1967.0f, -2544.0f, 94.0f, 0.0f }, { -2605.0f, -2286.0f, 92.0f, 0.0f },
                { -609.0f, -1614.0f, 94.0f, 0.0f }, { -881.0f, -3221.0f, 92.0f, 0.0f },
                { -3077.0f, -1786.0f, 92.0f, 0.0f }, { -3115.0f, -2352.0f, 94.0f, 0.0f },
            },
            .MinLevel = 20,
        });

        // Warsong Gulch at its proper size: ten a side, both sides learned. The flag rules are stage 11's; what
        // is new is that a side is ten seats and a group, so the objective has to be shared -- a carrier to escort
        // home, a base somebody has to hold, and an enemy carrier ten of them can chase. Trained by name, after
        // stage 11, which it seeds from.
        // Two a side, and a director. The seats already fight one on one from the arena; what is new is being
        // told what the pair is doing -- concentrate on that one, you take the next interrupt -- and learning
        // that following it pays. The director is scripted here and deliberately legible: the lowest enemy is the
        // focus, the duty goes round the side in turn. A learned director comes next, and meets seats that
        // already know how to be commanded rather than seats that have never heard an order.
        stages.push_back({
            .Name = "stage19_duo_led",
            .Suffix = "_duo",
            .Extends = "stage15_arena",
            .Summary = "two against two, told who to kill and whose turn it is: follow the call",
            .Blocks = { Core, Duel, Pack, Pet, Pvp, Context, Hostiles, Support, Order },
            .Arenas = { { .Name = "duo", .Seats = SeatPlan::Teams, .Against = Opposition::MirrorSeat,
                .Pvp = true, .EpisodeSeconds = 180, .Directed = true, .TeamSeats = 2 } },
            .InDefaultQueue = false,
            .MinLevel = 20,
        });

        stages.push_back({
            .Name = "stage18_warsong",
            .Suffix = "_warsong",
            .Extends = "stage17_flag",
            .Summary = "ten against ten for the flag: escort the carrier, hold the base, stop theirs",
            .Blocks = { Core, Duel, Pet, Pvp, Travel, Flag, Party },
            .Arenas = { { .Name = "warsong", .Seats = SeatPlan::Teams, .Against = Opposition::Flag, .Pvp = true,
                .EpisodeSeconds = 420 } },
            .InDefaultQueue = false,
            .MapId = MAP_WARSONG_GULCH,
            // Silverwing Hold and the Warsong Lumber Mill, as game_graveyard 769 and 770 put them: the real
            // battleground's own arrival points, which are also where its flags stand.
            .SpawnPoints = { { 1523.8f, 1481.8f, 352.0f, 3.1416f } },
            .FlagBases = {
                { 1523.8f, 1481.8f, 352.0f, 3.1416f },
                { 933.3f, 1433.7f, 345.5f, 0.1516f },
            },
            .MinLevel = 20,
        });

        // A pilot of arena mixing and merging, not part of the curriculum: the duel and the scripted enemy player in
        // one stage, merging the two stages that trained them (each teaches its arena). Trained only when named
        // (forge start mix_duel_pvp).
        stages.push_back({
            .Name = "mix_duel_pvp",
            .Suffix = "_mix",
            .Extends = "stage14_pvp",
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
        if ((arena.Schedule == PullSchedule::Gauntlet || arena.Schedule == PullSchedule::Sequence)
            && !stage.Has(BlockId::Gauntlet))
            return "the gauntlet schedule needs the gauntlet block";
        if (arena.Owner && (!(pulls || ambushOnly) || !stage.Has(BlockId::Companion)))
            return "an owner needs pulls or an ambush, and the companion block";
        if (arena.PartyGroup && (!arena.Owner || arena.Seats != SeatPlan::Party || !stage.Has(BlockId::Party)))
            return "a party group needs an owner, party seats and the party block";
        // Self-play: one seat a side in a Mirror, TEAM_SEATS of them in a Teams arena, and a team match is a
        // flag match -- there is nothing else for two learned sides of ten to be playing.
        bool const selfPlay = arena.Seats == SeatPlan::Mirror || arena.Seats == SeatPlan::Teams;
        if (selfPlay != (arena.Against == Opposition::MirrorSeat || flag))
            return "self-play seats go with fighting the mirror seat or a flag match, and only with them";
        if (arena.Seats == SeatPlan::Teams && !flag && arena.Against != Opposition::MirrorSeat)
            return "team seats fight the other team, at a flag or in an arena";
        if (arena.Seats == SeatPlan::Teams && (arena.TeamSeats < 1 || arena.TeamSeats > TEAM_SEATS))
            return "a side is between one seat and TEAM_SEATS";
        if (arena.Directed && !stage.Has(BlockId::Order))
            return "a director needs the order block: its seats have to read what it asks";
        if (arena.Directed && arena.Seats != SeatPlan::Teams)
            return "a director commands a side, so its arena needs team seats";
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
        // A party is the owner and its companions: GROUP_MEMBERS learned seats beside it, which is what this
        // returned when MAX_SEATS was 4 and is what it has to keep returning now that MAX_SEATS is a raid.
        case SeatPlan::Party:  return GROUP_MEMBERS;
        case SeatPlan::Raid:   return MAX_SEATS;
        case SeatPlan::Teams:  return std::min(TeamSeats, TEAM_SEATS) * TEAM_COUNT;
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

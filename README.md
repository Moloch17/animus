# mod-animus

Bots for a stock AzerothCore realm, played by models trained in [Animus Forge](https://github.com/Moloch17/animus-forge)
exactly as the forge trains them. A player (or a game master; `Animus.Follow.Security`) summons up to four
**followers** of their level and faction into their party, each played by its class's `move4_follow` model -- a
ray-cast third-person camera, a mental map, party frames that remember where each member was, goal and look heads --
moving only through the shared player controller, as a client would; they follow through dungeons, loading screens and
anywhere else. Entering a five-player dungeon alone fills the party with them (**dungeon party fill**, switchable per
player). Beside them: the **play capture** the forge trains on, with opt-in **recordings** of a player's own run and
their notes; **Prepare**, a character set up at a typed level for a recording; and the **Animus addon** for all of it.
The module builds against a stock AzerothCore with no core changes, together with its bundled copy of the forge's
runtime (`animus-lib/`).

**Not in this build.** The v1 class companion -- a character saved with yours, its director, life outside the fight
(quests, auction, mail, taxi, crafting) -- cannot run on the current runtime (the forge's Runtime cut of 2026-10-08
dropped its blocks) and no model of its format exists any more. Its code is in `attic/` (outside `src/`, so nothing
builds it), kept for reference.

## Installing

1. Put the module in a stock AzerothCore's `modules/` and build it static (the default). Nothing is fetched.
2. Installing creates `mod_animus.conf` from `conf/mod_animus.conf.dist` when there is none (never overwriting one).
   AzerothCore reads a module's settings from the `.conf` only.
3. Models: each `<class>_follow.amdl` **with its `.json` manifest**, exported from the forge's `move4_follow` run, in
   `models/` (installed to `<config dir>/modules/animus`) or in `<DataDir>/animus`, searched first.
4. Camera scenes: at startup the worldserver bakes (when missing or stale) and loads the static world of each map in
   `Animus.Vision.Maps` into `<Animus.DataDir>/scenes` (default `<config dir>/modules/animus/scenes`; it must be
   writable). The log gives each map's time; a bake is once per map and data revision.

Don't build it into the forge core; a forge build refuses it. The module compiles with `ANIMUS_STOCK_CORE`
(`mod-animus.cmake`): on a stock core the camera draws doors and other game objects from the stock collision tree,
without their surface normal (live-follow decision 2).

## Commands

Everything the addon does with buttons can be typed. `.animus help` lists the commands you may use with their
syntax, and a wrong call prints its usage line (the help lives in the module's code; nothing of it is written to the
world database, so `.help animus <command>` has no entry).

| Command | Who | Effect |
|---|---|---|
| `.animus help` | Any player | Every `.animus` command you may use, with its syntax |
| `.animus follow <n>` | `Animus.Follow.Security` (every player by default) | `n` (1-4) followers of your level and faction join your party, which you lead (made when you have none; never a raid). Each is a class with a loaded `<class>_follow` model, a race of your faction and a random spec. Refused on a map with no camera scene; a level outside 13-25 (what the models trained at) is warned about. Followers are never saved |
| `.animus follow` | Any player | Your followers, their models and the episode clock |
| `.animus dismiss` | Any player | Your followers leave (also when you log out or leave the dungeon they filled) |
| `.animus record [on\|off [keep\|discard] [title]]` | Any player | A recording of your own play started, stopped with a verdict and a title, or its status (play capture, "Recordings") |
| `.animus note <text>` | Any player | A note into your running recording |
| `.animus partyfill [on\|off]` | Any player | Whether your own dungeon parties are filled with followers (saved per character) |
| `.animus partyfill realm [on\|off]` | Game master | The realm's dungeon party fill until the next `.reload config` |
| `.animus prepare <level> <spec>` | `Animus.Prepare.Security` | This character set to that level, geared, talented and taught for the spec (its name or index), everything it wore and carried mailed to it first |
| `.animus god on\|off` | `Animus.Capture.Recording.GodSecurity` | God mode, flagged in the recording and switched off when the recording that switched it on stops |
| `.animus models` | Game master | Console too: every class's model for `Animus.Curriculum.Stage`, whether it loads (and if not why), and the camera scenes loaded |

## How a follower is played

Everything runs on the world thread after the maps update (`WorldScript::OnUpdate`), the forge's order: the
observation reads the world after the movement, and the keys act at the next map update, where each follower's client
(`src/Client/CompanionClient`) steps its body and reports it through its session's own movement handlers. Every
`Animus.Curriculum.DecisionMs` (250) the driver (`src/Follow/FollowParty`) reproduces the forge's per-decision pipeline
for a `move4_follow` seat (`.agents/plans/live-follow/live-follow.PIPELINE.md` in the forge): the deferred stamps (an
action's clock, the party frames' second sighting), the controller and step bookkeeping, the observation (camera,
mental map, party frames from the real group with you rotated to the first slot, goals), the model, then the goal
holds, the free look and the action. There is no target and no objective: the leader is found by sight, the minimap's
dot and memory.

**Episodes.** Training's episodes were 300 s; the realm plays a synthetic one. A summon starts it at 0 with every
follower reset; every 300 s (`Animus.Follow.EpisodeSeconds`) it rolls over with a full reset that keeps the mental
map (a second older); moving to another map or instance starts a new one with the mental map cleared. The model's
episode clock is `min(1, time since the episode began / 300 s)`.

**What differs from training** (tell the tester): the leader is a person, who fights, jumps and loots; a live dungeon
has creatures, doors (drawn without a normal) and other players; fewer than four followers leave party slots empty; a
dead follower stands up 10 s later (`Animus.Curriculum.Respawn.DelayMs`) **beside you** at full health, where training
rises at the episode's start point; a follower not on your map (a loading screen, an instance portal) is brought to
you, since the controller cannot take an area trigger.

The model's forward pass (`.amdl` format 10: image and map inputs, look and goal heads) is `MlpPolicy` behind
`src/Follow/FollowPolicy.h`, checked against the learner's network by `animus-lib/tools/golden`.

## The addon

`interface_addon/Animus` (copy it to the client's `Interface/AddOns/`) talks to the module over addon whispers and
does what the commands do, with buttons: a **Followers** tab (summon one to four, dismiss, who is with you), a
**Record** tab and recorder bar (`/animus rec`; notes by box, `/animus note <text>` or a key binding; Stop asks keep
or discard and a title), and a **Settings** tab (your dungeon party fill, a game master's realm switch, Prepare at a
level you type, god mode). The protocol is in `interface_addon/Animus/README.md`; the addon and the module must be
the same version (the window says which side is older).

## Configuration

`conf/mod_animus.conf.dist` documents every key; the defaults are what `move4_follow` trained with.

- `Animus.Enable`, `Animus.ModelDir`, `Animus.Curriculum.Stage` (`move4_follow`), `Animus.Curriculum.DecisionMs`.
- `Animus.DataDir`, `Animus.Vision.Maps` (`389,36,43,34`: Ragefire, Deadmines, Wailing Caverns, the Stockade).
- The camera: `Animus.Vision.RenderSize` (`64x32`, the one size every follower casts at, scaled up to the canonical
  image), `Width`/`Height` (128 x 64), `FovH`/`FovV` (120/60), `Range` (100), `Zoom`/`ZoomMin` (12/12), `Pitch` (-15).
- The mental map: `Animus.Map.MaxTiles` (4096), `CoarseTiles` (0), `ClusterMinCells`/`PocketMinCells` (2/6).
- The follow tuning: `Animus.Curriculum.Actions.*` (pacing), `Options.*`, `PartyFollow.MinimapYards` (60),
  `Respawn.DelayMs` (10000), and `Animus.Follow.EpisodeSeconds` (300); `Animus.Follow.Security` (0: every player
  may summon).
- `Animus.PartyFill.Enable`, `Animus.Prepare.Security` / `BagItem`, and the play capture's `Animus.Capture.*`
  (below).

The camera and mental-map settings take effect at startup only. For debug logging set
`Logger.module.animus=1,Console Server`.

## Updating animus-lib

The forge folded animus-lib into its core (`src/server/game/Animus`), so the bundle is refreshed from a forge
checkout: `tools/update-animus-lib.sh [forge checkout]` copies every file the bundle shares with the forge's runtime,
keeps the module's own (the model reader, the loader and `CoreHooks`, and the bot factory that places companions),
lists any include the bundle can no longer resolve, records the forge revision in `animus-lib/FORGE_REVISION`, and
checks the conf template documents every tuning key. A realm must build the manifests its models were trained with,
so refresh it from the forge revision of the models, together with them, then build against a stock core (a
forge-only call only fails at link). The script ends by comparing every shared file with the forge's byte for byte
and fails on any difference (`--check` does only that); `--only Movement` refreshes one directory, and
`animus-lib/FORGE_REVISION` then records it on a line of its own. `Movement/` is the player controller and the client
logic the forge and this module share; the module's own server link for it is `src/Client`.


## Play capture

The module records play (`Animus.Capture.Enable`, on by default) to files the forge trains on, under
`Animus.Capture.Dir` (empty: `<LogsDir>/animus-capture`, so under Docker `env/dist/logs/animus-capture` on the host): realistic movement first (a learned style reward and a realism score), then reference numbers, prices and
scenarios from real play, and whole dungeon runs to learn a dungeon from. The module only records; it scores and
trains nothing. Two tiers share one writer and one format (version 4):

- **The automatic tier** (`Animus.Capture.Auto`, on by default): every player from login, and what the companions
  decide, into hourly files at the realm's rates, pseudonymous, no opt-in.
- **Recordings**: one player's own play from the moment they press Record until they stop, into a folder of their
  own at a far higher rate and detail, identified, with notes. See "Recordings" below.

Followers are the "companions" of the format: their movement is captured as a player's and each decision goes to
the companion stream (there are no owner commands or ratings any more).

**What is recorded**, in eight streams (the format is [doc/capture-format.md](doc/capture-format.md), a copy of the
forge's `apps/forge/python/animus/human/FORMAT.md`; the last two carry recordings only):

| Stream | Content |
|---|---|
| session | login, logout, class, race, level, talent points, item level, map/zone/area and group changes, known spells, latency |
| move | every movement packet the client sends, unquantised, with the client's own time; speeds; mount, taxi, teleport, loading screen, death, resurrect, root/stun/fear, knockback, shapeshift, vehicle and transport events; each mover's state (class, race, level, map, zone, mount, form, combat, a companion's model and move revision); companions' packets, which their player controller sends through their session as a client does (source 2) |
| action | cast requests, refusals, casts going off and their ends; target selection; item use; attack start/stop; interactions (gossip, loot, quests, objects, vendors, flight masters, mail, auction house, trainers, banks, innkeepers) |
| snapshot | every 250 ms in combat or moving, 1 s otherwise: the player, the nearest 24 hostile and 10 friendly units within 40 yards, party members on the map and the pet, auras and cooldowns |
| outcome | damage and healing to or from a player, kills, deaths with cause, quests, boss encounters, PvP kills and duels, area changes |
| companion | each companion decision (model, observation hash, action, goals), the owner's commands, and ratings (`.animus rate`, the addon's Good and Bad buttons) |
| note | recordings: the player's typed notes (with where they stood, their target and the state of the moment), the recording's start and end, their group's chat |
| packet | recordings: every packet between the player's client and the server, both ways, as sent |

Recordings add to the session, snapshot and outcome streams too: the inventory, action bars and talents; every
nearby unit's full state every 100 ms with its auras and threat list; every visible object's state and each change;
the player's own numbers once a second; the map's whole spawn table and which creatures of the instance are alive;
and every unit entering, leaving or evading a fight.

**Privacy.** In the automatic tier everyone is captured with no opt-in, and told so at login
(`Animus.Capture.LoginNotice`). No chat, whispers, mail, names, account data or IPs are written there. Characters and
every other unit are 8-byte ids hashed with a realm-local salt (`<Dir>/salt`, made once; keep it, and keep it on the
realm). A recording is the player's own and identified: it holds their character's name, their group's chat (other
players' text redacted unless `Recording.OthersChat`), and the raw packets; they are told when it starts
(`Recording.Notice`).

**Files.** `<Dir>/<yyyy-mm-dd>/<hh>/<stream>-<map>.bin.gz` (UTC; session and companion files are `-all`), each a
run of gzip members (one per flush, so a crash loses at most the last), and an `index.json` per hour once it closes:
record and byte counts per file, players, sessions, records dropped by full buffers, and whether the snapshot stream
was paused for disk space. Nothing is ever deleted. `tools/capture-sample.bin` holds one record of every type with
the values in `tools/capture-sample.json` (made by `tools/capture-sample.cpp`), for testing a reader.

**Cost.** Hooks copy a record into their own thread's buffer (no lock, no allocation, no I/O) and one writer thread
compresses and writes. A full buffer drops records and counts them; it never waits. Expect roughly 3 to 5 MB per
player-hour compressed, most of it snapshots: lower `Animus.Capture.IdleSnapshotMs` or switch the snapshot stream
off (`Animus.Capture.Streams`) first if storage is short. Below `Animus.Capture.DiskReserveGB` free the snapshot
stream pauses by itself; movement is never thinned.

**Keys** (`Animus.Capture.*`, documented in `conf/mod_animus.conf.dist`): `Enable`, `Auto`, `Dir`, `SnapshotMs`,
`IdleSnapshotMs`, `Streams`, `FlushMs`, `BufferRecords`, `DiskWarnGB`, `DiskReserveGB`, `LoginNotice`, and for
recordings `Recording.SnapshotMs`, `Recording.UnitMs`, `Recording.UnitRange`, `Recording.MaxUnits`,
`Recording.Packets`, `Recording.PacketMaxBytes`, `Recording.PacketSkip`, `Recording.OthersChat`,
`Recording.MaxMinutes`, `Recording.Notice`, `Recording.GodSecurity`.

### Recordings

A recording starts only when the player says so: the addon's Record tab (or the recorder bar, `/animus rec`, a key
binding) or `.animus record on`. From then until they stop, their play and everything around them is written to
`<Dir>/recordings/<id>/` (`<yyyy-mm-dd>T<hhmmss>Z-<character>-<6 hex>`) at the `Recording.*` rates, and not to the
hourly files; unit events on their map still reach the automatic tier as before. Stopping asks for a verdict
(keep or discard: a discarded recording is marked in its `manifest.json`, never deleted) and a title. A logout, a
disconnect, a shutdown, a disk shortage or `Recording.MaxMinutes` ends it too, with the reason in the manifest.

Nothing but notes is typed. The Note box (or `/animus note <text>`, `.animus note`, the key binding) writes free
text with where the player stood, what they targeted and the state of the moment. Pulls, rests, doors, bosses and
skips are not tagged: the forge's `python -m animus.human dungeon-run` reads them off the recording (combat edges,
threat lists, object changes, the spawn table) and writes the **clear way** a PointNav stage follows
(`AnimusForge.Curriculum.PointNav.ClearWayFile`), so the bots learn the dungeon as a person ran it.

For a run recorded solo: switch your own party fill off (Settings tab), **Prepare** the character at the dungeon's
level (Settings tab: a level you type and a spec; everything you wore and carried is mailed back to you first,
`Animus.Prepare.Security`), and record. God mode (`Recording.GodSecurity`) is there for a character that would not
survive, but it is flagged everywhere it shows, since health and damage under it are not human-like; play as a
group would (pull by pull, rest, drink) so the movement and pacing stay real. Several runs beat one.

**Not recorded or approximate** (what the stock core's hooks do not carry): damage and heal records carry no spell,
school, absorb, crit or overheal; an encounter's `boss_entry` is the instance script's boss index; breath is
estimated from time under water; snapshot unit velocities are estimated from movement flags and speeds; group
members on other maps are written without a level; companions get no snapshots.

## Dungeon party fill

Entering a five-player dungeon alone, or as its group's leader, fills the group to five with followers
(`Animus.PartyFill.Enable`) two seconds after arriving, as `.animus follow` would summon them; they are never saved
and go when you leave the dungeon alive. Each player can switch their own fill off and on (the addon's Settings tab,
`.animus partyfill off`), saved per character in `animus_player_settings`; a game master can switch the realm's off
until the next `.reload config` (`.animus partyfill realm off`). Both are checked when the group would be filled, so a
running group is not sent away. Followers only follow: they do not fight (the follow models are movement models), so a
filled party is company and a test of the models, not a dungeon group.

## Prepare

`.animus prepare <level> <spec>` (the addon's Settings tab) sets your character to a level you choose with gear, talents
and every spell for it, built the way the forge builds a seat (`SeatCharacter::Configure` on the follow stage's
layout). Everything you wear and carry is mailed back to you first, bags included; keyring, bank, money and
professions stay. Gated by `Animus.Prepare.Security` (game master by default) and refused while recording. For
getting a recording character ready in a minute.

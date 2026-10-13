# mod-animus

**This build is the live follow test** (the forge's live-follow plan): a game master summons up to four bots into
their party, and each is played by its class's `move4_follow` model from
[Animus Forge](https://github.com/Moloch17/animus-forge) exactly as the forge trains it -- a ray-cast third-person
camera, a mental map, party frames that remember where each member was, goal and look heads -- moving only through the
shared player controller, as a client would. They follow you through dungeons, loading screens and anywhere else. The
module builds against a stock AzerothCore with no core changes, together with its bundled copy of the forge's runtime
(`animus-lib/`).

**Not in this build.** The v1 features -- the class companion saved with your character, its director, dungeon party
fill, life outside the fight (quests, auction, mail, taxi, crafting) and the Animus addon -- cannot run on the current
runtime. Their code is in `attic/` (outside `src/`, so nothing builds it), kept for reference; the worldserver says so
loudly at startup. `interface_addon/` is likewise unused.

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

Game master commands.

| Command | Effect |
|---|---|
| `.animus follow <n>` | `n` (1-4) followers of your level and faction join your party, which you lead (made when you have none; never a raid). Each is a class with a loaded `<class>_follow` model, a race of your faction and a random spec. Refused on a map with no camera scene; a level outside 13-25 (what the models trained at) is warned about. Followers are never saved |
| `.animus follow` | Your followers, their models and the episode clock |
| `.animus dismiss` | Your followers leave (also when you log out) |
| `.animus models` | Console too: every class's model for `Animus.Curriculum.Stage`, whether it loads (and if not why), and the camera scenes loaded |

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

The model's forward pass (`.amdl` format 10: image and map inputs, look and goal heads) is behind
`src/Follow/FollowPolicy.h`; until it is linked, `.animus follow` refuses with "follow model runtime not yet linked".

## Configuration

`conf/mod_animus.conf.dist` documents every key; the defaults are what `move4_follow` trained with.

- `Animus.Enable`, `Animus.ModelDir`, `Animus.Curriculum.Stage` (`move4_follow`), `Animus.Curriculum.DecisionMs`.
- `Animus.DataDir`, `Animus.Vision.Maps` (`389,36,43,34`: Ragefire, Deadmines, Wailing Caverns, the Stockade).
- The camera: `Animus.Vision.RenderSize` (`64x32`, the one size every follower casts at, scaled up to the canonical
  image), `Width`/`Height` (128 x 64), `FovH`/`FovV` (120/60), `Range` (100), `Zoom`/`ZoomMin` (12/12), `Pitch` (-15).
- The mental map: `Animus.Map.MaxTiles` (4096), `CoarseTiles` (0), `ClusterMinCells`/`PocketMinCells` (2/6).
- The follow tuning: `Animus.Curriculum.Actions.*` (pacing), `Options.*`, `PartyFollow.MinimapYards` (60),
  `Respawn.DelayMs` (10000), and `Animus.Follow.EpisodeSeconds` (300).

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

In this build the followers are the "companions" below: their movement is captured as a player's, and each decision
goes to the companion stream. There are no owner commands or ratings (`.animus rate` and the addon are not in it).

With `Animus.Capture.Enable = 1` and `Animus.Capture.Dir` set, the module records what every player on the realm
does, and what the companions decide, to files the forge trains on: realistic movement first (a learned style
reward and a realism score), then reference numbers, prices and scenarios from real play. The module only records;
it scores and trains nothing.

**What is recorded**, in six streams (the format is [doc/capture-format.md](doc/capture-format.md), a copy of the
forge's `apps/forge/python/animus/human/FORMAT.md`):

| Stream | Content |
|---|---|
| session | login, logout, class, race, level, talent points, item level, map/zone/area and group changes, known spells, latency |
| move | every movement packet the client sends, unquantised, with the client's own time; speeds; mount, taxi, teleport, loading screen, death, resurrect, root/stun/fear, knockback, shapeshift, vehicle and transport events; each mover's state (class, race, level, map, zone, mount, form, combat, a companion's model and move revision); companions' packets, which their player controller sends through their session as a client does (source 2) |
| action | cast requests, refusals, casts going off and their ends; target selection; item use; attack start/stop; interactions (gossip, loot, quests, objects, vendors, flight masters, mail, auction house, trainers, banks, innkeepers) |
| snapshot | every 250 ms in combat or moving, 1 s otherwise: the player, the nearest 24 hostile and 10 friendly units within 40 yards, party members on the map and the pet, auras and cooldowns |
| outcome | damage and healing to or from a player, kills, deaths with cause, quests, boss encounters, PvP kills and duels, area changes |
| companion | each companion decision (model, observation hash, action, goals), the owner's commands, and ratings (`.animus rate`, the addon's Good and Bad buttons) |

**Privacy.** Everyone is captured with no opt-in, and told so at login (`Animus.Capture.LoginNotice`). No chat,
whispers, mail, names, account data or IPs are written. Characters and every other unit are 8-byte ids hashed with
a realm-local salt (`<Dir>/salt`, made once; keep it, and keep it on the realm).

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

**Keys** (`Animus.Capture.*`, documented in `conf/mod_animus.conf.dist`): `Enable`, `Dir`, `SnapshotMs`,
`IdleSnapshotMs`, `Streams`, `FlushMs`, `BufferRecords`, `DiskWarnGB`, `DiskReserveGB`, `LoginNotice`.

**Not recorded or approximate** (what the stock core's hooks do not carry): damage and heal records carry no spell,
school, absorb, crit or overheal; an encounter's `boss_entry` is the instance script's boss index; breath is
estimated from time under water; snapshot unit velocities are estimated from movement flags and speeds; group
members on other maps are written without a level; companions get no snapshots.


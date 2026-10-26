# mod-animus

Characters for an ordinary AzerothCore realm whose combat decisions come from models trained in
[Animus Forge](https://github.com/Moloch17/animus-forge). The module builds against a stock AzerothCore with no core
changes, together with animus-lib (`animus-lib/`), the forge's curriculum runtime it plays the models with.

- **A class companion.** Every player character may have one: a character of the name, race and class they
  choose, on an account made for it, saved in the characters database between summons. It joins your party at your
  level and levels up with you, follows you through loading screens, into instances and onto transports, and plays
  its class model in every fight. It gets no mail and no achievements.
- **The Animus addon** (`interface_addon/Animus`): the window players create, summon, rename and
  manage it from.
They run the same encoding code as training, so a model sees and does exactly what it trained on.

**The detail is in [chapter 6 of the Animus manual][manual-6].** This page is the map.

[manual-6]: https://github.com/Moloch17/animus-forge/blob/master/docs/manual/06-animus.md

## Installing

1. Put the module in a stock AzerothCore's `modules/`. animus-lib comes with it (`animus-lib/`, the revision this
   module was tested with), so nothing is fetched at build time. Build it static (the default); a dynamic build needs
   the library as its own module (copy `animus-lib/` to `modules/mod-animus-lib`).
2. Rebuild and install the worldserver (`docker compose build` works offline).
3. Installing creates `mod_animus.conf` in the modules config directory from `conf/mod_animus.conf.dist` when there is
   none, and never overwrites one; under Docker the container copies it to your config volume on its first start.
   AzerothCore reads a module's settings from the `.conf` only: without it every `Animus.*` key logs "Missing
   property" at startup.
4. Models: installing copies the ones in `models/` (exported from the forge, each `.amdl` with its `.json` manifest;
   the repository ships none) to `<config dir>/modules/animus`, which reaches a Docker runtime image (only `bin/` and
   `etc/` do). Companions play the stage `Animus.Curriculum.Stage` names (default `stage9_deadmines`). Models you
   place by hand go in `<DataDir>/animus`, which is searched first; the startup log names the directory used.

Don't build it into the forge core; a forge build disables it. Where the models come from, and how `DataDir` and the
install step line up, is in
[manual 6.3 and 6.4](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/06-animus.md#63-installing).

## Ground probe fields

A companion senses the ground around it (how far it can walk along sixteen bearings, steps, shores, burning ground,
the room around it, and in the air how far it can fly) from the forge's layered height fields, as the forge trains
it to: one file a map grid, `<map>_<gridX>_<gridY>.field`, about 0.3 MB. Put them in `<DataDir>/fields`, beside
`maps/`, `vmaps/` and `mmaps/` (`Animus.Probe.Dir` moves it). The forge makes them with `forge fieldworld all`, which
bakes every grid of every map's navmesh into its `apps/forge/probes/world/`; ship the whole directory, or the maps
your players go to. A companion on a grid with no field keeps its last reading until it reaches one, and the grid is
logged once. `Animus.Probe.Source` must be the probe the models were trained with (their manifest names it; the
forge's default is `geometry`).

The field files are zstd-compressed; the module carries zstd itself (`animus-lib/deps/zstd`, compiled in with its
symbols hidden), so a realm needs nothing installed and no change to its core or its Docker image.

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

## The addon

Players use the module through the Animus addon in `interface_addon/Animus`: copy that folder into
the client's `Interface/AddOns/` and type `/animus` (or click the minimap button). Until they have a companion the
window creates one from a name, a race of their faction and a class that race can be; then it summons and
dismisses it, renames it, or gives it a new race and class (a new character of the same name), and the unit menu
of its party frame has "Dismiss companion". Inspecting the companion edits it: its Talents tab learns a rank on
left click and unlearns one on right click, a Pet tab does the same for a hunter pet's tree, and an item dragged
from your bags onto its character pane goes on it, with what it wore coming back to you. Everything is saved with
the character.
The addon talks to the module over addon whispers the player sends to themselves (prefix `Animus`), which need
`AddonChannel = 1` in `worldserver.conf` (the default); no security is required. The messages are documented in
[its README](interface_addon/Animus/README.md), for anyone writing another client.

## Companions in the database

A companion is a character of its own: `animus_companion` (characters database, created by the module itself when the
first companion account is, and dropped by `.animus purge`) maps an owner to it -- its `characters` guid, the account
it lives on and what the module remembers (spec, whether the owner edited it, the gear they gave it). The account is
`ANIMUS<owner guid>` in the auth database, made on the owner's first create with a random password nobody is told; it
stays when the character is replaced and goes when the owner's character is deleted. The character saves as any does
(the core's autosave interval, and at once on dismiss, on the owner's logout and at shutdown), and is loaded again on
summon as a login loads one, on the database thread. Its mail and achievements are deleted before every load and after
every save, players cannot mail it, level rewards skip it and its achievement criteria are never checked.

## Commands

Everything the addon does with buttons can be typed. The companion, recording and settings commands are every
player's; the realm's switches are a game master's. `.animus help` lists the commands you may use with their syntax,
`.help animus <command>` explains one, and a wrong call prints its usage line (the module writes its help into the
core's `command` table at startup, with the same securities as below, so `.help` and the syntax errors know it).

| Command | Who | Effect |
|---|---|---|
| `.animus help` | Any player | Every `.animus` command you may use, with its syntax |
| `.animus create <name> <race> <class>` | Any player | Your companion character (`.animus create Tarn orc warrior`) |
| `.animus summon` | Any player | It comes to you |
| `.animus dismiss` | Any player | Saved and sent away |
| `.animus rename <name>` | Any player | A new name, nothing else changed |
| `.animus reroll <race> <class>` | Any player | A new character of the same name |
| `.animus list` | Any player | Your companion and whether its model is loaded |
| `.animus talent <companion> learn\|unlearn <talent id>` | Any player | One rank of its talent (the addon's talent pane does this by clicking) |
| `.animus life [<feature> on\|off]` | Game master | What companions do outside the fight: switches for all, quests, auction, mail, taxi, crafting, until the next `.reload config` (the addon's Settings tab shows them to game masters) |
| `.animus stage list` | Game master | Every curriculum stage and its arenas: the names `Animus.Curriculum.Stage` accepts |
| `.animus models` | Game master | Console too: every class's model for `Animus.Curriculum.Stage` and whether it loads, and if not the first manifest field that differs. Run it after copying models in, before anyone summons |
| `.animus rate <+\|-> [movement\|combat\|healing\|tanking\|stuck\|other]` | Any player | Your verdict on your companion's play, recorded for training (play capture) |
| `.animus record [on\|off [keep\|discard] [title]]` | Any player | A recording of your own play started, stopped with a verdict and a title, or its status (play capture, "Recordings") |
| `.animus note <text>` | Any player | A note into your running recording |
| `.animus partyfill [on\|off]` | Any player | Whether your own dungeon parties are filled with bots (saved per character) |
| `.animus partyfill realm [on\|off]` | Game master | The realm's dungeon party fill until the next `.reload config` |
| `.animus prepare <level> <spec>` | `Animus.Prepare.Security` | This character set to that level, geared, talented and taught for the spec (its name or index), everything it wore and carried mailed to it first |
| `.animus god on\|off` | `Animus.Capture.Recording.GodSecurity` | God mode, flagged in the recording and switched off when the recording that switched it on stops |
| `.animus purge` | Administrator | Console too: every account the module made (`ANIMUS<guid>`) deleted with its characters, every companion sent away unsaved, the module's table dropped |

## Play capture

The module records play (`Animus.Capture.Enable`, on by default) to files the forge trains on, under
`Animus.Capture.Dir` (empty: `<LogsDir>/animus-capture`, so under Docker `env/dist/logs/animus-capture` on the host): realistic movement first (a learned style reward and a realism score), then reference numbers, prices and
scenarios from real play, and whole dungeon runs to learn a dungeon from. The module only records; it scores and
trains nothing. Two tiers share one writer and one format (version 4):

- **The automatic tier** (`Animus.Capture.Auto`, on by default): every player from login, and what the companions
  decide, into hourly files at the realm's rates, pseudonymous, no opt-in.
- **Recordings**: one player's own play from the moment they press Record until they stop, into a folder of their
  own at a far higher rate and detail, identified, with notes. See "Recordings" below.

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

Entering a five-player dungeon alone, or as its group's leader, fills the group to five
(`Animus.PartyFill.Enable`): a tank if nobody present can hold a pull, a healer if nobody can heal,
then damage dealers. Each member's role is read off its talents, not its class, and your companion
counts as a member. The new members are of your level and faction, are never saved, and stay behind
when you leave the dungeon alive. They play with the configured stage's models, so they only tank and
heal well with a stage trained for party combat.

Each player can switch their own fill off and on (the addon's Settings tab, `.animus partyfill off`), saved per
character in `animus_player_settings`; a game master can switch the realm's off until the next `.reload config`
(`.animus partyfill realm off`). Both are checked when the group would be filled, so a running group is not sent
away.

## Testing a model from the forge

1. Export it on the forge: `forge export <scenario>` on its console, or by hand
   `python -m animus.export --checkpoint runs/<scenario>/best.pt --out <dir> --layouts-dir <OutputDir>/layouts` (the
   `layouts` directory itself, not the scenario's inside it: that gives the wrong names and no manifests).
2. Copy every `<class>_<stage>.amdl` **with its `.json`** into `Animus.ModelDir`, and set `Animus.Curriculum.Stage` to
   the stage they came from.
3. The layered fields (`forge fieldworld all` → `apps/forge/probes/world/`) go in `Animus.Probe.Dir` (default
   `<DataDir>/fields`; under Docker the data volume is read-only, so point the key at a mounted directory). Startup
   logs `Animus ground probe: geometry from <dir>`.
4. `.animus models` (console) must say every class loads.

A movement stage's model (stages 1-7) travels to an objective. A companion's is its owner, so it walks, jumps, swims
and flies to you with what it learned; the forge also tells a seat how much longer the walking route is than the
straight line, which the module does not measure and gives as the straight line. Following, fighting beside you and
the rest are the companion stage's (`stage9_deadmines`).

A build outside Docker reads module configs from its compile-time config directory (`<install>/etc/modules`), not
from beside `worldserver.conf`: a key that logs "Missing property" is a `mod_animus.conf` in the wrong place.

## Models

A companion plays `<class><stage suffix>.amdl` for `Animus.Curriculum.Stage` (`hunter_companion.amdl` for the default
`stage9_deadmines`): one model per class, not per class and role -- the curriculum has no roles, and a model plays
every build its class can have. A model loads only if its manifest is exactly the one this server builds for that
layout, so the realm needs the curriculum revision the forge trained with, and the same world database and DBC data. A
refused model is logged once and shown by `.animus list`, and its companion only follows you. Models load on first use
and again after `.reload config`.

`conf/mod_animus.conf.dist` documents every `Animus.*` key. For debug logging set
`Logger.module.animus=1,Console Server`.

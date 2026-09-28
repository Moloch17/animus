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
4. Models: installing copies the ones in `models/` (`stage16_companion`'s, one per class, each `.amdl` with its
   `.json` manifest) to `<config dir>/modules/animus`, which reaches a Docker runtime image (only `bin/` and `etc/`
   do). Companions play them by default (`Animus.Curriculum.Stage = stage16_companion`). Models you place by hand go
   in `<DataDir>/animus`, which is searched first; the startup log names the directory used.

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
forge-only call only fails at link).

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

Game master commands, not available from the console; the addon does the same for every player.

| Command | Effect |
|---|---|
| `.animus create <name> <race> <class>` | Your companion character (`.animus create Tarn orc warrior`) |
| `.animus summon` | It comes to you |
| `.animus dismiss` | Saved and sent away |
| `.animus rename <name>` | A new name, nothing else changed |
| `.animus reroll <race> <class>` | A new character of the same name |
| `.animus list` | Your companion and whether its model is loaded |
| `.animus stage list` | Every curriculum stage and its arenas: the names `Animus.Curriculum.Stage` accepts |
| `.animus models` | Console too: every class's model for `Animus.Curriculum.Stage` and whether it loads, and if not the first manifest field that differs. Run it after copying models in, before anyone summons |
| `.animus purge` | Administrator, console too: every account the module made (`ANIMUS<guid>`) deleted with its characters, every companion sent away unsaved, the module's table dropped |

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
the rest are the companion stage's (`stage16_companion`).

A build outside Docker reads module configs from its compile-time config directory (`<install>/etc/modules`), not
from beside `worldserver.conf`: a key that logs "Missing property" is a `mod_animus.conf` in the wrong place.

## Models

A companion plays `<class><stage suffix>.amdl` for `Animus.Curriculum.Stage` (`hunter_companion.amdl` for the default
`stage16_companion`): one model per class, not per class and role -- the curriculum has no roles, and a model plays
every build its class can have. A model loads only if its manifest is exactly the one this server builds for that
layout, so the realm needs the curriculum revision the forge trained with, and the same world database and DBC data. A
refused model is logged once and shown by `.animus list`, and its companion only follows you. Models load on first use
and again after `.reload config`.

`conf/mod_animus.conf.dist` documents every `Animus.*` key. For debug logging set
`Logger.module.animus=1,Console Server`.

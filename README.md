# mod-animus

Characters for an ordinary AzerothCore realm whose combat decisions come from models trained in
[Animus Forge](https://github.com/Moloch17/animus-forge). The module builds against a stock AzerothCore with no core
changes, together with [animus-lib](https://github.com/Moloch17/animus-lib), the code it shares with the forge.

- **Class/role companions.** Up to four characters of the race, class and role you choose join your party at your
  level and level up with you. They follow you through loading screens, into instances and onto transports, and play
  their class model in every fight.
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
4. Models: installing copies the ones in `models/` (`stage1_duel`'s, one per class, each `.amdl` with its `.json`
   manifest; confirmed at 60M steps, trained with animus-lib 99cbe94) to `<config dir>/modules/animus`, which reaches
   a Docker runtime image (only `bin/` and `etc/` do). Companions play them by default
   (`Animus.Curriculum.Stage = stage1_duel`). Models you place by hand go in `<DataDir>/animus`, which is searched
   first; the startup log names the directory used.

Don't build it into the forge core; a forge build disables it. Where the models come from, and how `DataDir` and the
install step line up, is in
[manual 6.3 and 6.4](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/06-animus.md#63-installing).

## Updating animus-lib

`tools/update-animus-lib.sh [ref]` pulls a revision of [animus-lib](https://github.com/Moloch17/animus-lib) into
`animus-lib/` (a git subtree, default `master`) and commits it. A realm must build the manifests its models were
trained with, so update it together with the models.

## Commands

Game master commands, not available from the console.

| Command | Effect |
|---|---|
| `.animus summon <race> <class> <role>` | A companion of that race, class and role joins your party (`human priest heal`, `orc warrior tank`) |
| `.animus list` | Your companions and whether their models are loaded |
| `.animus dismiss` | Remove all your companions |
| `.animus stage list` | Every curriculum stage and its arenas: the names `Animus.Curriculum.Stage` accepts |

Running a curriculum stage in the world is Animus Forge's job, not this module's: building an episode needs
animus-lib's training half, which needs a core patch this module deliberately does without.

## Models

A companion plays `<class>_<role><stage suffix>.amdl` for `Animus.Curriculum.Stage` (`hunter_dps_duel.amdl` for
the default `stage1_duel`). A model loads only if its manifest is exactly the one this server builds for that layout,
so the realm needs the animus-lib revision the forge trained with, and the same world database and DBC data. A refused
model is logged once and shown by `.animus list`, and its companion only follows you. Models load on first use and
again after `.reload config`.

`conf/mod_animus.conf.dist` documents every `Animus.*` key. For debug logging set
`Logger.module.animus=1,Console Server`.

# mod-animus

Characters for an ordinary AzerothCore realm whose combat decisions come from models trained in
[Animus Forge](https://github.com/Moloch17/animus-forge). The module builds against a stock AzerothCore with no core
changes, together with [animus-lib](https://github.com/Moloch17/animus-lib), the code it shares with the forge.

- **A class companion.** Every player character may have one: a character of the name, race and class they
  choose, on an account made for it, saved in the characters database between summons. It joins your party at your
  level and levels up with you, follows you through loading screens, into instances and onto transports, and plays
  its class model in every fight. It gets no mail and no achievements.
- **The Animus addon** (`interface_addon/animus_addon/Animus`): the window players create, summon, rename and
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

## The addon

Players use the module through the Animus addon in `interface_addon/animus_addon/Animus`: copy that folder into
the client's `Interface/AddOns/` and type `/animus` (or click the minimap button). Until they have a companion the
window creates one from a name, a race of their faction and a class that race can be; then it summons and
dismisses it, renames it, or gives it a new race and class (a new character of the same name), and the unit menu
of its party frame has "Dismiss companion". Inspecting the companion edits it: its Talents tab learns a rank on
left click and unlearns one on right click, a Pet tab does the same for a hunter pet's tree, and an item dragged
from your bags onto its character pane goes on it, with what it wore coming back to you. Everything is saved with
the character.
The addon talks to the module over addon whispers the player sends to themselves (prefix `Animus`), which need
`AddonChannel = 1` in `worldserver.conf` (the default); no security is required. The messages are documented in
[its README](interface_addon/animus_addon/Animus/README.md), for anyone writing another client.

## Companions in the database

A companion is a character of its own: `animus_companion` (characters database, created by the module's
`data/sql/db-characters/` on startup) maps an owner to it -- its `characters` guid, the account it lives on and
what the module remembers (spec, whether the owner edited it, the gear they gave it). The account is
`ANIMUS<owner guid>` in the auth database, made on the owner's first create with a random password nobody is told;
it stays when the character is replaced and goes when the owner's character is deleted. The character saves as
any does (the core's autosave interval, and at once on dismiss, on the owner's logout and at shutdown), and is
loaded again on summon as a login loads one, on the database thread. Its mail and achievements are deleted before
every load and after every save, players cannot mail it, level rewards skip it and its achievement criteria are
never checked.

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

## Models

A companion plays `<class><stage suffix>.amdl` for `Animus.Curriculum.Stage` (`hunter_duel.amdl` for the default
`stage1_duel`): one model per class, not per class and role -- the curriculum has no roles, and a model plays every
build its class can have. A model loads only if its manifest is exactly the one this server builds for that layout,
so the realm needs the curriculum revision the forge trained with, and the same world database and DBC data. A refused
model is logged once and shown by `.animus list`, and its companion only follows you. Models load on first use and
again after `.reload config`.

`conf/mod_animus.conf.dist` documents every `Animus.*` key. For debug logging set
`Logger.module.animus=1,Console Server`.

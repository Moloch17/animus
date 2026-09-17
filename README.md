# mod-animus

Characters for an ordinary AzerothCore realm whose combat decisions come from models trained in
[Animus Forge](https://github.com/Moloch17/animus-forge). The module builds against a stock AzerothCore with no core
changes, together with [animus-lib](https://github.com/Moloch17/animus-lib), the code it shares with the forge.

- **Class/role companions.** Up to four characters of the race, class and role you choose join your party at your
  level and level up with you. They follow you through loading screens, into instances and onto transports, and play
  their class/role model in every fight.
- **The stage viewer.** A game master runs any curriculum stage exactly as the forge trains it, in their own instance,
  and watches the seats play their models, a scripted baseline or random actions.

Both run the same scenario and encoding code as training, so a model sees and does exactly what it trained on.

**The detail is in [chapter 6 of the Animus manual][manual-6].** This page is the map.

[manual-6]: https://github.com/Moloch17/animus-forge/blob/master/docs/manual/06-animus.md

## Installing

1. Put the module in a stock AzerothCore's `modules/`. Configuring clones animus-lib into `modules/mod-animus-lib` if
   it is missing. Build both the same way: static (the default) or both dynamic.
2. Rebuild and install the worldserver.
3. Copy `conf/mod_animus.conf.dist` to your config directory as `mod_animus.conf`.
4. Put the exported models, each `.amdl` with its `.json` manifest beside it, in `Animus.ModelDir` (`animus`, relative
   to `DataDir`). Installing copies the ones in `models/` there: `stage1_duel`'s, one per class/role (confirmed at
   60M steps, trained with animus-lib 99cbe94). Companions play them with `Animus.Curriculum.Stage = stage1_duel`.

Don't build it into the forge core; a forge build disables it. Where the models come from, and how `DataDir` and the
install step line up, is in
[manual 6.3 and 6.4](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/06-animus.md#63-installing).

## Commands

Game master commands, not available from the console.

| Command | Effect |
|---|---|
| `.animus summon <race> <class> <role>` | A companion of that race, class and role joins your party (`human priest heal`, `orc warrior tank`) |
| `.animus list` | Your companions and whether their models are loaded |
| `.animus dismiss` | Remove all your companions |
| `.animus stage list` | Every curriculum stage and its arenas |
| `.animus stage open <stage> [policy] [arena]` | Build a stage in your own instance and spawn its first episode, frozen. `policy` is `model` (the default), `random`, `greedy` or `fight` |
| `.animus stage spawn [tier] [class_role] [level]` | Replace the episode with a new one, frozen: a difficulty tier (stages that fight one creature), what the first seat plays (`warlock_dps`), every character's level. Each is `any` or left out for the curriculum's own, and holds for later episodes |
| `.animus stage start` | Let it play: episodes follow one another until `stop` |
| `.animus stage stop` | Freeze everything where it is |
| `.animus stage status` | The open stage: frozen or playing, episode, arena, spawn choices, seats and their models |
| `.animus stage close` | Remove the stage |

A stage duel at the top tier, frozen, then played: `.animus stage open stage1_duel`, `.animus stage spawn 6 warlock_dps
70`, `.animus stage start`.

## Models

A companion plays `<class>_<role><stage suffix>.amdl` for `Animus.Curriculum.Stage` (`warrior_tank_party.amdl` for
the default `stage5_party`). A model loads only if its manifest is exactly the one this server builds for that layout,
so the realm needs the animus-lib revision the forge trained with, and the same world database and DBC data. A refused
model is logged once and shown by `.animus list`, and its companion only follows you. Models load on first use and
again after `.reload config`.

`conf/mod_animus.conf.dist` documents every `Animus.*` key. For debug logging set
`Logger.module.animus=1,Console Server`.

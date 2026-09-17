# animus-lib

The code that training and play share. [mod-animus-forge](https://github.com/Moloch17/animus-forge) trains models with
it on the forge core; [mod-animus](https://github.com/Moloch17/animus) plays them with it on a stock AzerothCore. Both
run the same scenarios and encodings, so a stage a game master watches is exactly the stage the forge trained, and a
model's observations and actions mean the same thing in training and in play.

It is an AzerothCore module (`modules/mod-animus-lib`), but a passive one: no settings, no commands, no world update.
It registers only the combat hooks env pools need, uses only public core APIs, and builds on both cores.

**The detail is in the Animus manual:**
[chapter 3](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/03-animus-lib.md) for the machinery and
[chapter 4](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/04-curriculum.md) for the curriculum.
This page is the map.

## What is in it

| Directory | Contents | Manual |
|---|---|---|
| `src/Scenario/Curriculum/` | The curriculum: stage definitions, blocks (observation features and actions), layouts and manifests, encounters, character building, rewards, scripted baselines, tuning | [4](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/04-curriculum.md) |
| `src/Scenario/` | `Scenario`, the interface a host drives, and `StageSettings`, what a host tells it | [3.3](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/03-animus-lib.md#33-the-scenario-interface) |
| `src/Env/` | `EnvPool`: every env of a scenario and the flat buffers a host exchanges | [3.4](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/03-animus-lib.md#34-envs-and-the-env-pool) |
| `src/Bot/` | Sessionless bots with no character row, rebuilt every episode | [3.5](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/03-animus-lib.md#35-bots) |
| `src/Core/` | `CoreHooks`, the seams for what only the forge core can do | [3.6](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/03-animus-lib.md#36-core-seams-corehooks) |
| `src/Model/` | The `.amdl` reader and forward pass, and the library that checks a model against its manifest | [3.8](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/03-animus-lib.md#38-models) |
| `src/Hooks/` | Damage, heal, cast and creature level hooks feeding every registered pool | [3.4](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/03-animus-lib.md#hooks-and-threading) |
| `tools/spec_builds/` | The standard talent builds and glyphs, and the generator for `SpecBuilds.cpp` | [4.3](https://github.com/Moloch17/animus-forge/blob/master/docs/manual/04-curriculum.md#43-characters) |

## Getting it

mod-animus and mod-animus-forge each bundle this library's source as a git subtree in `animus-lib/`, at the revision
they were tested with, so a module folder builds offline with nothing else to fetch. When `modules/mod-animus-lib` is
present (a development checkout of this repository) it is built instead and the bundled copies are ignored:

```
git clone https://github.com/Moloch17/animus-lib.git modules/mod-animus-lib
```

Build it the way you build the modules that need it: static (the default) or all dynamic (a dynamic build needs this
module; copy a bundle to `modules/mod-animus-lib`). `cmake/AnimusLibDependency.cmake` holds the rules the dependents
apply. A dependent picks up a new revision with its `tools/update-animus-lib.sh`.

## Changing it

A layout's manifest records everything its model depends on: the stage's blocks, each block's features and actions,
the action catalog and the talents. Change any of them and every model exported before the change is refused, and the
affected stages must be retrained from the first one that has the change. All three Animus modules share one include
path, so header names must stay unique across them, and nothing here may call a forge-only core API directly: add a
`CoreHooks` seam instead. Recipes for tuning values, reward terms, features, blocks, encounters and stages are in
[manual 7.9][manual-7-9].

[manual-7-9]: https://github.com/Moloch17/animus-forge/blob/master/docs/manual/07-operations.md#79-extending-the-curriculum

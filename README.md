# mod-animus

Companions for a normal AzerothCore realm whose combat decisions come from a policy trained in
[mod-animus-forge](../mod-animus-forge). The module needs no core changes and adds no dependencies.

First slice: a level 1 human warrior that fights a training dummy using the `warrior_dummy`
policy.

## Commands (GM)

| Command | Effect |
|---|---|
| `.animus spawn` | A level 1 human warrior (`Animus<N>`) appears beside you and follows you |
| `.animus attack` | Your companion attacks the training dummy you have targeted |
| `.animus dismiss` | Removes your companion |

Each player can have one companion. It can only be summoned in the open world, not in instances,
on transports or in flight. It is removed when its owner logs out, changes map or dismisses it.

## How a companion decides

```
every world update                      every Animus.DecisionMs while it has a target
 └─ MovementTree (decision tree)         └─ WarriorDummy::Observe  → 9 floats + action mask
     has target? in melee? facing?           MlpPolicy::Decide     → greedy allowed action
     → follow / chase / face /               WarriorDummy::Apply   → queue or cancel Heroic Strike
       start auto-attack / hold
```

- **Movement and engagement are scripted.** `src/Companion/MovementTree.cpp` decides whether the
  bot follows its owner, closes to melee, turns to face the target or starts auto-attack. That
  matches training, where the scenario starts white swings itself.
- **Every ability choice goes through the model.** `src/Model/MlpPolicy.cpp` runs the exported
  actor: dense layers with tanh between them. The input is the observation plus a one-hot agent id,
  and the output is the argmax over allowed actions.
- **Observations mirror the training scenario.** `src/Companion/WarriorDummyPolicyIO.*` must stay in
  step with `mod-animus-forge/src/Scenario/WarriorDummyScenario.*`. The model file records its
  scenario and shapes, and a mismatched model is refused at load.
- **Bots are sessionless players.** They have no socket and no character row, and are never saved
  (`src/Bot/BotFactory.cpp`). Other players see them normally. They get an empty social list and a
  character cache entry, so whispers, invites and name queries work.

## Installing

1. Put the module in `modules/`, then rebuild and install the worldserver.
2. Copy `conf/mod_animus.conf.dist` to your config directory as `mod_animus.conf`.
3. Check the startup log for the model:

   ```
   Animus loaded warrior_dummy model from /azerothcore/env/dist/data/animus/warrior_dummy.amdl (9+1 -> 128 -> 128 -> 3)
   ```

Without a model, companions can still be summoned and follow you, but `.animus attack` is refused.
The error line gives the full path that was tried.

### Where the model goes

The install step copies `models/*.amdl` into the data directory. The rule is in `mod-animus.cmake`.
At startup the worldserver reads `<DataDir>/<Animus.ModelDir>/warrior_dummy.amdl`.

| Setting | Default | Notes |
|---|---|---|
| CMake `ANIMUS_MODELS_INSTALL_DIR` | `<install prefix>/data/animus` | Where the install copies the models |
| `Animus.ModelDir` | `animus` | Relative paths resolve against `DataDir`; absolute paths are used as-is |

- **Docker:** the defaults line up. `AC_DATA_DIR` is `/azerothcore/env/dist/data`, and installing in
  `ac-dev-server` writes into the shared client-data volume that `ac-worldserver` also mounts.
- **Other setups:** the stock `worldserver.conf` sets `DataDir = "."`, which is the worldserver's
  working directory. Either set `DataDir` to `<install prefix>/data`, configure with
  `-DANIMUS_MODELS_INSTALL_DIR=<DataDir>/animus`, or give `Animus.ModelDir` an absolute path.

After exporting a new model, run the install step again, then restart the worldserver or run
`.reload config`.

## Producing the model

Train in mod-animus-forge, then export the checkpoint:

```
cd ../mod-animus-forge/python
python -m animus.train  --config configs/warrior_dummy.yaml
python -m animus.export --checkpoint runs/warrior_dummy/latest.pt --out ../../mod-animus/models/warrior_dummy.amdl
```

The `.amdl` format is documented in `mod-animus-forge/python/animus/export.py`.

## Debugging

Set `Logger.module.animus=1,Console Server` to log at debug level. It logs each movement-tree
transition and every non-noop model action, together with the observation that produced it.

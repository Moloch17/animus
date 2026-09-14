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

1. Put the module in `modules/` and rebuild the worldserver.
2. Copy `conf/mod_animus.conf.dist` to your config directory as `mod_animus.conf`.
3. Place the model at `<Animus.ModelDir>/warrior_dummy.amdl`. The startup log confirms it:

   ```
   Animus loaded warrior_dummy model from modules/mod-animus/models/warrior_dummy.amdl (9+1 -> 128 -> 128 -> 3)
   ```

Without a model, companions can still be summoned and follow you, but `.animus attack` is refused.

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

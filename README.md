# mod-animus

Companions for a normal AzerothCore realm whose combat decisions come from trained policies. The module needs no
core changes and adds no dependencies: it builds against a stock AzerothCore on its own.

- **Class/role companions:** up to four characters of any class and role at your level join your party and play
  their class/role models.
- **`warrior_dummy`:** the first slice, a level 1 human warrior that fights a training dummy.

## Commands (GM)

| Command | Effect |
|---|---|
| `.animus summon <class_role>` | A class/role companion (`priest_heal`, `warrior_tank`, `mage_dps`, ...) at your level joins your party |
| `.animus list` | Your class/role companions and whether their models are loaded |
| `.animus spawn` | A level 1 human warrior (`Animus<N>`) appears beside you and follows you |
| `.animus attack` | Your warrior companion attacks the training dummy you have targeted |
| `.animus dismiss` | Removes all your companions |

Companions can only be summoned in the open world, not in instances, on transports or in flight, and are removed
when their owner logs out.

## Class/role companions

`.animus summon <class_role>` builds a character of that class and role (the 18 class/roles: `warrior_dps`,
`warrior_tank`, `paladin_heal`, ... `druid_heal`) at your level: a race of your faction, one of the role's specs
with a random talent build, the class trainers' spells, level-appropriate gear, food and drink, and for hunters a
stable of beasts to call. It joins your group (one is created if you have none; only the leader can add
companions, and the group must have room). Up to four companions per player.

- **Models:** each companion plays `<class>_<role><stage suffix>.amdl` from `Animus.ModelDir` for
  `Animus.Curriculum.Stage` (`warrior_tank_party.amdl` for the default `party` stage). Every model must have its layout
  manifest beside it (`warrior_tank_party.json`, exported with the model), and the manifest must be exactly the one
  this server builds for the layout: same stage, class/role, sizes, block offsets, actions and talents. A model
  without a manifest or with a different one is refused (logged once, and shown by `.animus list`); its companion
  only follows you.
- **Decisions:** every `Animus.Curriculum.DecisionMs` (100) each companion observes itself, its target, the enemies, you and the
  other companions (`src/Curriculum/Layout/SeatEncoder.*`), its model picks an allowed action, and the action is applied as
  a client would: casts, item uses, movement, target selection, pet commands, heals on party members.
- **Enemies:** everything attacking you, a companion or their pets, and what any of you attack, fills up to four
  enemy slots for the current fight; the fight is over when none of them is still alive and fighting.
- **Upkeep:** out of combat, companions more than 30 yards away run back to you and more than 100 yards away (or
  on another open-world map) are teleported to you. While you are in an instance they wait where they are. A dead
  companion stands up 10 seconds after the fight.
- **Nothing is saved:** companions and their pets have no character rows. Their group membership is written like
  any group member's and removed when they are dismissed; rows left behind by a crash are cleaned up by the core
  at startup (group members without a character).

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

1. Put the module in a stock AzerothCore's `modules/`, then rebuild and install the worldserver.
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

After adding a model, run the install step again (or copy it to `Animus.ModelDir` yourself), then restart the
worldserver or run `.reload config`. Class/role models are loaded the first time a companion needs them, and again
after `.reload config`.

## Models

Models are exported by the trainer (`.amdl` files, with a `.json` layout manifest beside each class/role model) and
copied into `models/` or `Animus.ModelDir` by hand; nothing copies them automatically. The `.amdl` format: dense
layers with tanh between them, fed the observation followed by a one-hot agent id, the greedy allowed action out
(`src/Model/MlpPolicy.cpp` reads it).

## Debugging

Set `Logger.module.animus=1,Console Server` to log at debug level. It logs each movement-tree
transition and every non-noop model action, together with the observation that produced it.

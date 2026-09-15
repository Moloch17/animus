# mod-animus

Characters for a normal AzerothCore realm whose combat decisions come from policies trained in
[mod-animus-forge](https://github.com/Moloch17/animus-forge). The module needs no core changes: it builds against a
stock AzerothCore, together with [animus-lib](https://github.com/Moloch17/animus-lib), the code it shares with the
forge.

- **Stage viewer:** a game master runs any curriculum stage exactly as the forge trains it, without the learner, in
  their own instance, and watches the seats play their models.
- **Class/role companions:** up to four characters of the race, class and role you choose, at your level, join your
  party, follow you through loading screens and play their class/role models.

## Commands (GM)

| Command | Effect |
|---|---|
| `.animus stage list` | Every curriculum stage, its arenas and how to start one |
| `.animus stage start <stage> [policy] [arena]` | Teleports you to where the stage happens and runs it there (see below) |
| `.animus stage status` | The stage you watch: its episode, arena and seats, and their models |
| `.animus stage reset` | Ends the current episode and starts a new one |
| `.animus stage stop` | Removes the stage you watch |
| `.animus summon <race> <class> <role>` | A companion of that race, class and role (`human priest heal`, `orc warrior tank`, ...) at your level joins your party |
| `.animus list` | Your class/role companions and whether their models are loaded |
| `.animus dismiss` | Removes all your companions |

## Stage viewer

`.animus stage start stage5_party` runs the forge's `stage5_party` with the same scenario code the forge trains
with (animus-lib's `StageScenario` and `EnvPool`), one env instead of many:

1. **You go where the stage happens.** The command teleports you (GM teleport) to `Animus.Stage.SpawnPoint.*` --
   by default the forge's own spawn point, the Old Hillsbrad Foothills entrance -- in an instance of that map.
2. **The stage is built in your instance** once you arrive: the seats' characters (race, level, spec, standard
   talents and glyphs, trainer spells, gear, potions), and whatever the episode's arena has -- a creature, pulls,
   a scripted owner and party group, a scripted or mirror enemy player, ambushers.
3. **Episodes follow one another** as in training: every `Animus.Stage.DecisionMs` each seat observes, its policy
   picks an allowed action and the action is applied as a client would. When an episode ends (its goal or its time
   limit) the env is rebuilt for the next one, and chat reports how it went: the arena, its length, and per seat its
   class/role and level, damage and dps, damage taken, whether it killed and died, health left and its total reward.
4. **It stops** with `.animus stage stop`, when you leave the instance or log out, or when you start another stage.

`policy` is what the seats play (`Animus.Stage.Policy` when omitted):

| Policy | Seats play |
|---|---|
| `model` | Each seat's exported class/role model for the stage (`<class>_<role><stage suffix>.amdl` in `Animus.ModelDir`, checked against its manifest). A seat whose model is missing or refused does nothing, and you are told once per model. |
| `random` | Random allowed actions |
| `greedy` | The first usable spell or trinket (the forge's scripted baseline) |
| `fight` | `greedy`, plus attacking, moving, eating and drinking, and healing (stages with the duel block) |

`arena` plays only that arena of a stage that mixes several (`.animus stage start stage8_crossroads model ambush`);
without it every episode draws its arena by weight, as in training.

Things to know:

- **GM mode goes on for you.** `.animus stage start` turns it on as `.gm on` would (and says so), so the stage's
  creatures and enemy players leave you alone; it stays on when the stage stops. Your presence changes nothing the
  seats observe.
- **What makes it match training:** the stage definitions, blocks and encounters are the forge's own code. The
  settings are this module's: set `Animus.Stage.DecisionMs`, `EpisodeSeconds`, `ClassRoles`, `Level` and
  `SpawnPoint.*` like the forge's `AnimusForge.*` keys, and the `Animus.Curriculum.*` tuning like the run's
  `AnimusForge.Curriculum.*` (its `stage.json` `"tuning"` holds the values it trained with).
- **The first start of a stage** builds every class/role's assets it needs (trainer data and item pools): the world
  stalls for some seconds. Later starts reuse them.
- **Several game masters** can each watch a stage (`Animus.Stage.MaxViewers`); each gets its own instance.
- **Nothing is saved:** the seats, owners and enemy players have no character rows; the rows a stock core writes for
  their instance binds and groups are removed when they go.

## Class/role companions

`.animus summon <race> <class> <role>` builds a character of that race, class and role at your level, as the forge
builds a stage's seats (animus-lib's `SeatCharacter`): one of the role's specs with its standard talent build and
glyphs, the class trainers' spells, level-appropriate gear, potions, bandages and stones, food and drink for layouts
that use them, and for hunters a stable of beasts to call. It joins your group (one is created if you have none).

| Argument | Accepted |
|---|---|
| `race` | `human`, `dwarf`, `nightelf`, `gnome`, `draenei`, `orc`, `undead` (or `forsaken`), `tauren`, `troll`, `bloodelf` |
| `class` | `warrior`, `paladin`, `hunter`, `rogue`, `priest`, `deathknight` (or `dk`), `shaman`, `mage`, `warlock`, `druid` |
| `role` | `dps` (or `damage`), `tank`, `heal` (or `healer`) |

Names ignore case, underscores and hyphens (`night_elf`, `NightElf`). The class/roles are the forge's 18:
warriors, paladins, death knights and druids tank; paladins, priests, shamans and druids heal; every class deals
damage. The summon is refused only when the arguments cannot make a character of your side:

- a name that is not a race, class or role;
- a role the class does not have (`mage tank`);
- a race the class does not allow (`orc paladin`, `human shaman`);
- a race of the other faction.

A death knight you summon below level 55 is made at level 55, the level death knights start at.

Beyond the arguments, only where you are or what no character could do stops a companion: you are on a flight path or
a vehicle, you already have four, you are in a group you do not lead or that is full, you are in a battleground or
arena (they only take queued players), or you are in the middle of a loading screen.

**Where companions go with you.** You can summon in the open world, in dungeon and raid instances, and on a boat,
zeppelin or elevator (the companion boards it with you). Companions then follow you through every loading screen:

- **Another map or an instance:** as soon as you arrive, each companion is brought to you, into the same instance
  (it joins with your group's instance and difficulty). Entry requirements (level, attunement, keys) do not apply to
  it. Only an instance that refuses anyone at that moment -- full, or a raid encounter in progress -- keeps it out; it
  comes as soon as the instance takes it.
- **Transports:** when you board a transport your companions board it too, and when you step off they step off beside
  you. A transport that crosses to another map carries them through its loading screen with you.
- **Flight paths and vehicles:** when you take a flight path (a taxi, or a scripted quest flight) or get on a vehicle
  (a bombing run, a siege engine, a seat on someone else's mount), your companions disappear. When you are off it they
  come back beside you, wherever you landed -- another zone, another map or an instance. `.animus list` shows them as
  waiting for you to land.
- **Anything else that teleports a companion** (a summoning spell, a script) completes as a client would acknowledge it.
- **Battlegrounds and arenas:** companions wait where you left them and rejoin you when you come back.

Companions are removed when their owner logs out.

- **Models:** each companion plays `<class>_<role><stage suffix>.amdl` from `Animus.ModelDir` for
  `Animus.Curriculum.Stage` (`warrior_tank_party.amdl` for the default `stage5_party`). Every model must have its
  layout manifest beside it (`warrior_tank_party.json`, exported with the model), and the manifest must be exactly the
  one this server builds for the layout: same stage, class/role, sizes, block offsets, actions and talents. A model
  without a manifest or with a different one is refused (logged once, and shown by `.animus list`); its companion
  only follows you.
- **Decisions:** every `Animus.Curriculum.DecisionMs` (100) each companion observes itself, its target, the enemies,
  you and the other companions (animus-lib's `SeatEncoder`), its model picks an allowed action, and the action is
  applied as a client would: casts, item uses, movement, target selection, pet commands, heals and resurrections on
  party members.
- **Enemies:** everything attacking you, a companion or their pets, and what any of you attack, fills up to four
  enemy slots for the current fight; the fight is over when none of them is still alive and fighting.
- **Upkeep:** out of combat, companions more than 30 yards away run back to you and more than 100 yards away are
  teleported to you. On another map, in another instance, or on or off a different
  transport, they are brought to you at once, in or out of combat. A dead companion accepts a resurrection, or stands up
  10 seconds after the fight.
- **Nothing is saved:** companions and their pets have no character rows. Their group membership is written like
  any group member's and removed when they are dismissed; rows left behind by a crash are cleaned up by the core
  at startup (group members without a character). A companion never answers an instance's lock warning, so it is only
  ever bound to an instance temporarily, and the bind goes with it.

## Installing

1. Put the module in a stock AzerothCore's `modules/`. Configuring clones animus-lib into `modules/mod-animus-lib`
   when it is missing (`mod-animus.cmake`; `-DANIMUS_LIB_GIT_URL` and `-DANIMUS_LIB_GIT_REF` choose another
   repository or branch). Build both the same way (static, the default, or both dynamic).
2. Rebuild and install the worldserver.
3. Copy `conf/mod_animus.conf.dist` to your config directory as `mod_animus.conf`.

### Where the models go

The install step copies `models/*.amdl` into the data directory. The rule is in `mod-animus.cmake`. Copy each
model's `.json` manifest beside it too.

| Setting | Default | Notes |
|---|---|---|
| CMake `ANIMUS_MODELS_INSTALL_DIR` | `<install prefix>/data/animus` | Where the install copies the models |
| `Animus.ModelDir` | `animus` | Relative paths resolve against `DataDir`; absolute paths are used as-is |

- **Docker:** the defaults line up. `AC_DATA_DIR` is `/azerothcore/env/dist/data`, and installing in
  `ac-dev-server` writes into the shared client-data volume that `ac-worldserver` also mounts.
- **Other setups:** the stock `worldserver.conf` sets `DataDir = "."`, which is the worldserver's
  working directory. Either set `DataDir` to `<install prefix>/data`, configure with
  `-DANIMUS_MODELS_INSTALL_DIR=<DataDir>/animus`, or give `Animus.ModelDir` an absolute path.

Models are loaded the first time a companion or a stage seat needs them, and again after `.reload config`.

## Models

Models are exported by the forge (`forge export`: an `.amdl` file and a `.json` layout manifest per class/role) and
copied into `models/` or `Animus.ModelDir` by hand; nothing copies them automatically. The `.amdl` format: dense
layers with tanh between them, fed the observation followed by a one-hot agent id, the greedy allowed action out
(animus-lib's `src/Model/MlpPolicy.cpp` reads it).

## Debugging

Set `Logger.module.animus=1,Console Server` to log at debug level.

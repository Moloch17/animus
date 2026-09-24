# Animus (addon)

The player's side of [mod-animus](../../README.md): a window to summon and dismiss class companions, for the
3.3.5a client. Every player can use it; the `.animus` commands stay game master ones.

## Installing

1. Copy this `Animus` folder into the client's `Interface/AddOns/`, so `Interface/AddOns/Animus/Animus.toc` exists.
2. The realm runs mod-animus, and `AddonChannel = 1` in its `worldserver.conf` (the default): the addon talks to the
   module over addon messages.
3. `/animus` opens the window; the minimap button does the same. That is the addon's one command.

## What it does

**The window** shows the stage whose models the realm's companions play, your companions (name, level, class and
spec, whether the model loaded, and whether the companion is parked while you fly), and a summon panel: a race of
your faction, a class that race can be, and what to ask of the build (tank, healer, or nothing in particular). A
summon the module refuses (a class no build of which can do what was asked, a full group, a flight path) shows the
module's reason under the button and in the chat frame. "Dismiss all" removes every companion.

**The unit menu.** Right click a companion's party frame (or its target frame) and the menu has "Dismiss
companion". 3.3.5 nameplates have no menu; this is the unit frame's.

**The inspect window** edits a companion. Inspect it (within 28 yards, `TalentsInspecting = 1` on the realm, the
default) and:

- **Talents.** The Talents tab learns a rank on left click and unlearns one on right click, under the game's own
  rules (points in the rows above, prerequisites), refunding the point. The companion spends its points itself when
  it levels up, along its spec's standard build; once you have edited it, a level-up keeps your choices and spends
  only the new points, leaving what the build cannot place for you.
- **Pet.** A fourth tab shows a hunter companion's pet tree (ferocity, tenacity or cunning), edited the same way. A
  pet that dies and is replaced from the stable comes back with the standard build.
- **Character.** Drag an item from your bags onto a slot of its character pane: the companion equips it and what it
  wore goes into your bags (the slot you dragged from, if it fits). The game's own rules apply (class, level,
  proficiency), and the client's red error tells you why when they refuse. Gear you gave a companion comes back to
  your bags when you dismiss it; it is lost if you log out or the server stops with the companion still out, and a
  two-hander you give one puts its off-hand into its own bags, which are emptied at its next level-up.

## Protocol

The addon whispers itself with prefix `Animus`; the module swallows those whispers and answers with addon whispers
from the player to themselves, prefix `Animus`. Words are tab-separated, one message per line below, each at most
255 bytes (the client's limit); a reply puts its one free-text field last, so it is what a cut takes.

Requests (lower case):

| Request | Answer |
|---|---|
| `hello` | `HELLO`, one `RACE` per race, `WANTS`, then the party |
| `list` | the party |
| `summon <race> <class> <wants>` | `OK` or `ERR`, then the party |
| `dismiss [name]` | `OK` or `ERR`, then the party; one companion by name, or all |
| `talent <name> learn\|unlearn <talent id>` | `OK` or `ERR`; the addon inspects again to see the change |
| `pettalent <name> learn\|unlearn <talent id>` | `ERR`, or the pet (below) |
| `pet <name>` | `PET` and one `PETTALENT` per talent of its tree, or `ERR` for a companion without a hunter pet out |
| `equip <name> <bag> <slot> <inventory slot>` | `OK` or `ERR`; bag 0 is the backpack, slots from 1, inventory slots 1 to 19 as the client numbers them |

Replies (upper case, so the addon can tell an answer from its own request echoed back by a realm without the
module):

| Reply | Fields |
|---|---|
| `HELLO` | protocol version (`1`), `1` when Animus is enabled else `0`, the stage name |
| `RACE` | race word, comma-separated class words that race can be; races of the player's faction only |
| `WANTS` | comma-separated words the summon's third argument takes (`tank,heal,dps`) |
| `PARTY` | companion count, the most a party holds; `MEMBER` lines follow |
| `MEMBER` | name, class, spec, level, `1` when parked else `0`, model name, `loaded` or why the model is not |
| `PET` | companion name, pet name, level, unspent points, talent count |
| `PETTALENT` | companion name, talent id, row, column, max rank, rank, the ranks' spell ids comma-separated, prerequisite talent id (0 for none), the rank it needs |
| `OK` / `ERR` | the module's message |

Talent ids are Talent.dbc's, the ones in the client's talent hyperlinks (`|Htalent:1234:...`). The race, class and
wants words are the ones `.animus summon` accepts (`nightelf`, `deathknight`, `heal`), first
name of each. Which builds a class can be asked for is not in the catalog (a class's assets are built on first use,
a few seconds each); a summon that asks too much is refused with the wants that class can be asked for.

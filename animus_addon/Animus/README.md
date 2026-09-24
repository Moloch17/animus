# Animus (addon)

The player's side of [mod-animus](../../README.md): the window your companion is created, summoned and managed
from, for the 3.3.5a client. Every player can use it; the `.animus` commands stay game master ones.

## Installing

1. Copy this `Animus` folder into the client's `Interface/AddOns/`, so `Interface/AddOns/Animus/Animus.toc` exists.
2. The realm runs mod-animus, and `AddonChannel = 1` in its `worldserver.conf` (the default): the addon talks to the
   module over addon messages.
3. `/animus` opens the window; the minimap button does the same. That is the addon's one command.

## What it does

**One companion per character.** Until you have one, the window is a create panel: a name, a race of your faction
and a class that race can be, and "Create bot". The realm makes an account for the companion, creates the
character on it at your level and saves it; it joins your party. From then on the button is gone (a character has
one companion) and the window shows it:

- **Summon** brings it back to you from the database, **Dismiss** saves it and sends it away. It also leaves when
  you log out, and comes back when you summon it. Only you can summon it; the account it lives on has a password
  nobody is told.
- **Rename** gives the character a new name and changes nothing else (a summoned companion goes and comes back
  under it).
- **Change race and class** resets it: the character is deleted and a new one of the same name, race and class
  you picked, at your level with default talents and gear, takes its place. A confirmation says so.

**The unit menu.** Right click the companion's party frame (or its target frame) and the menu has "Dismiss
companion". 3.3.5 nameplates have no menu; this is the unit frame's.

**The inspect window** edits a summoned companion. Inspect it (within 28 yards, `TalentsInspecting = 1` on the
realm, the default) and:

- **Talents.** The Talents tab learns a rank on left click and unlearns one on right click, under the game's own
  rules (points in the rows above, prerequisites), refunding the point. The companion spends its points itself when
  it levels up, along its spec's standard build; once you have edited it, a level-up keeps your choices and spends
  only the new points, leaving what the build cannot place for you.
- **Pet.** A fourth tab shows a hunter companion's pet tree (ferocity, tenacity or cunning), edited the same way. A
  pet that dies and is replaced from the stable comes back with the standard build, and a pet's talents do not
  survive a dismiss.
- **Character.** Drag an item from your bags onto a slot of its character pane: the companion equips it and what it
  wore goes into your bags (the slot you dragged from, if it fits). The game's own rules apply (class, level,
  proficiency), and the client's red error tells you why when they refuse. The companion keeps that gear, saved
  with it, until you change its race and class. A two-hander you give one puts its off-hand into its own bags,
  which are emptied at its next level-up.

The companion gets no mail (players cannot send it any; the server's level rewards skip it) and no achievements.

## Protocol

The addon whispers itself with prefix `Animus`; the module swallows those whispers and answers with addon whispers
from the player to themselves, prefix `Animus`. Words are tab-separated, one message per line below, each at most
255 bytes (the client's limit); a reply puts its one free-text field last, so it is what a cut takes.

Requests (lower case):

| Request | Answer |
|---|---|
| `hello` | `HELLO`, one `RACE` per race, then `COMPANION` |
| `list` | `COMPANION` |
| `create <name> <race> <class>` | `OK` or `ERR`, then `COMPANION` |
| `summon` | `OK` (the character loads on the database thread; `OK` and `COMPANION` again when it stands there) or `ERR`, then `COMPANION` |
| `dismiss` | `OK` or `ERR`, then `COMPANION` |
| `rename <name>` | `OK` or `ERR`, then `COMPANION` |
| `reroll <race> <class>` | `OK` or `ERR`, then `COMPANION` |
| `talent <name> learn\|unlearn <talent id>` | `OK` or `ERR`; the addon inspects again to see the change |
| `pettalent <name> learn\|unlearn <talent id>` | `ERR`, or the pet (below) |
| `pet <name>` | `PET` and one `PETTALENT` per talent of its tree, or `ERR` for a companion without a hunter pet out |
| `equip <name> <bag> <slot> <inventory slot>` | `OK` or `ERR`; bag 0 is the backpack, slots from 1, inventory slots 1 to 19 as the client numbers them |

Replies (upper case, so the addon can tell an answer from its own request echoed back by a realm without the
module):

| Reply | Fields |
|---|---|
| `HELLO` | protocol version (`2`), `1` when Animus is enabled else `0`, the stage name |
| `RACE` | race word, comma-separated class words that race can be; races of the player's faction only |
| `COMPANION` | `0` for none; else `1`, name, race word, class word, level, spec, `1` when out, `1` while loading, `1` when parked, model name, `loaded` or why the model is not (the last two when out) |
| `PET` | companion name, pet name, level, unspent points, talent count |
| `PETTALENT` | companion name, talent id, row, column, max rank, rank, the ranks' spell ids comma-separated, prerequisite talent id (0 for none), the rank it needs |
| `OK` / `ERR` | the module's message |

Talent ids are Talent.dbc's, the ones in the client's talent hyperlinks (`|Htalent:1234:...`). The race and class
words are the ones `.animus create` accepts (`nightelf`, `deathknight`), first name of each.

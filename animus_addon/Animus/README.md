# Animus (addon)

The player's side of [mod-animus](../../README.md): a window to summon and dismiss class companions, for the
3.3.5a client. Every player can use it; the `.animus` commands stay game master ones.

## Installing

1. Copy this `Animus` folder into the client's `Interface/AddOns/`, so `Interface/AddOns/Animus/Animus.toc` exists.
2. The realm runs mod-animus, and `AddonChannel = 1` in its `worldserver.conf` (the default): the addon talks to the
   module over addon messages.
3. `/animus` opens the window; the minimap button (left click) does the same. Right click on the button dismisses
   every companion.

| Command | Effect |
|---|---|
| `/animus` | Open or close the window |
| `/animus summon <race> <class> [tank\|heal\|dps]` | A companion joins your party (`human priest heal`) |
| `/animus list` | Refresh the companion list and open the window |
| `/animus dismiss` | Remove all your companions |
| `/animus reconnect` | Ask the realm again (after a `.reload config`, say) |
| `/animus minimap` | Hide or show the minimap button |

The window shows the stage whose models the realm's companions play, your companions (name, level, class and
spec, whether the model loaded, and whether the companion is parked while you fly), and a summon panel: a race of
your faction, a class that race can be, and what to ask of the build (tank, healer, or nothing in particular). A
summon the module refuses (a class no build of which can do what was asked, a full group, a flight path) shows the
module's reason under the button and in the chat frame.

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
| `dismiss` | `OK` or `ERR`, then the party |

Replies (upper case, so the addon can tell an answer from its own request echoed back by a realm without the
module):

| Reply | Fields |
|---|---|
| `HELLO` | protocol version (`1`), `1` when Animus is enabled else `0`, the stage name |
| `RACE` | race word, comma-separated class words that race can be; races of the player's faction only |
| `WANTS` | comma-separated words the summon's third argument takes (`tank,heal,dps`) |
| `PARTY` | companion count, the most a party holds; `MEMBER` lines follow |
| `MEMBER` | name, class, spec, level, `1` when parked else `0`, model name, `loaded` or why the model is not |
| `OK` / `ERR` | the module's message |

The race, class and wants words are the ones `.animus summon` accepts (`nightelf`, `deathknight`, `heal`), first
name of each. Which builds a class can be asked for is not in the catalog (a class's assets are built on first use,
a few seconds each); a summon that asks too much is refused with the wants that class can be asked for.

# Animus (addon)

The player's side of [mod-animus](../../../README.md): the window your companion is created, summoned and managed
from, for the 3.3.5a client. Every player can use it; the `.animus` commands stay game master ones.

## Installing

1. Copy this `Animus` folder into the client's `Interface/AddOns/`, so `Interface/AddOns/Animus/Animus.toc` exists.
2. The realm runs mod-animus, and `AddonChannel = 1` in its `worldserver.conf` (the default): the addon talks to the
   module over addon messages.
3. `/animus` opens the window; the minimap button does the same. That is the addon's one command.

## What it does

A **Followers** tab: summon one to four bots of your level and faction into your party, played by their class's
follow model as the forge trained it (they see through a camera, keep a map in mind, and keep up with you through
dungeons and loading screens); dismiss them; see who is with you. They are never saved, and only come on a map the
realm baked a camera scene for. The realm's `Animus.Follow.Security` says who may summon (every player by default).

## Recording, settings, Prepare

Two more tabs beside Followers. **Record**: a recording of your own play on the realm (movement, actions, combat,
your group's chat and everything around you) from the moment you press Record until you stop; it is identified
as yours. The recorder bar (`/animus rec`, movable, remembers its place) shows the clock and the note count and
holds the Note box; `/animus note <text>` and a key binding (Esc > Key Bindings > Animus) note too. Stop asks for a
title and whether to keep or discard it. **Settings**: whether your dungeon parties are filled with bots (saved
per character; a game master also sees the realm's switch), and, when the realm allows it for your account,
Prepare -- a level you type and a spec, then your character set to it with gear, talents and spells, everything
you wore and carried mailed back to you first -- and god mode, flagged in the recording. Everything the window does
can also be typed: `.animus help` lists the chat commands.

The addon and the module must be the same version: the window says which side is older.

## Protocol

The addon whispers itself with prefix `Animus`; the module swallows those whispers and answers with addon whispers
from the player to themselves, prefix `Animus`. Words are tab-separated, one message per line below, each at most
255 bytes (the client's limit); a reply puts its one free-text field last, so it is what a cut takes. Protocol 4.

Requests (lower case):

| Request | Answer |
|---|---|
| `hello` | `HELLO`, then `FOLLOW` (with its `FOLLOWER` lines), `RECORD`, `SETTINGS` |
| `followers` | `FOLLOW` and one `FOLLOWER` per follower |
| `follow <n>` | `OK` or `ERR` (warnings as `OK` lines first), then `FOLLOW`: `n` (1-4) followers join your party (Animus.Follow.Security) |
| `dismiss` | `OK` or `ERR`, then `FOLLOW` |
| `record` | `RECORD` |
| `record on` | `OK` or `ERR`, then `RECORD`: a recording of your own play starts |
| `record off <keep\|discard> [title]` | `OK` or `ERR`, then `RECORD`: the recording ends with your verdict and an optional title (free text, last) |
| `note <client ms> <text>` | `NOTE` or `ERR`: free text (at most 200 bytes, last) into the running recording |
| `settings` | `SETTINGS` |
| `partyfill on\|off` | `OK` or `ERR`, then `SETTINGS`: whether your own dungeon parties are filled with followers (saved per character) |
| `partyfill realm on\|off` | `OK` or `ERR`, then `SETTINGS`: the realm's dungeon party fill until its config is reloaded (game master) |
| `specs` | one `SPEC` per spec of your class, then `LEVELS` (when Prepare is for your account) |
| `prepare <level> <spec index>` | `OK` or `ERR`: your character set to that level, geared, talented and taught for the spec; everything worn and carried mailed to you first (Animus.Prepare.Security) |
| `god on\|off` | `OK` or `ERR`, then `RECORD` (Animus.Capture.Recording.GodSecurity) |

Replies (upper case, so the addon can tell an answer from its own request echoed back by a realm without the
module):

| Reply | Fields |
|---|---|
| `HELLO` | protocol version (`4`), `1` when Animus is enabled else `0`, the stage name, `1` when the realm can record play, `1` for a game master, `1` when Prepare is for this account, `1` when this account may summon followers |
| `FOLLOW` | followers with you, the most allowed; then one `FOLLOWER` line per follower: its name, level, class and spec, model and episode clock as free text |
| `RECORD` | `1` while recording else `0`, the recording id, seconds so far, notes so far, `1` under god mode, `1` when the realm can record |
| `NOTE` | the note's sequence number in the recording, notes so far |
| `SETTINGS` | your own dungeon party fill (`1`/`0`), the realm's, `1` while a game master's override is in force, the config's value |
| `SPEC` | spec index, spec name (one line per spec of your class) |
| `LEVELS` | the lowest and the highest level Prepare takes for your class |
| `OK` / `ERR` | the module's message |

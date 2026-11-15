# Wi-Fi sync

`engine/sync.cpp` is what makes a saved file appear on the phone a second later. It exists only in the loader build. A standalone game contains none of this code, because the App Store does not allow an app to download code and because a released game has nothing to sync with.

The other half of the conversation is the server in `tools/dev.py`. See [dev.py and its server](../reference/dev-tool.md) for every address it answers.

## What it offers the rest of the engine

::: file engine/sync.h

`main.cpp` starts it once, asks for commands each frame, and shows its status line on the game list. Everything else happens on a background thread.

## What happens after you save

```
 You save games/coin-dash/main.lua
        │
        ▼
 dev.py's watcher notices, prints [live], and the "games revision" number changes
        │
        ▼
 The phone asks  GET /api/state  once a second and sees the new revision
        │
        ▼
 The phone asks  GET /api/manifest  : every game file with a hash of its contents
        │
        ▼
 For each file whose hash differs from what the phone has:  GET /files/coin-dash/main.lua
        │
        ▼
 The file is written into the app's Documents/games/coin-dash/
        │
        ▼
 main.cpp's hot-reload check sees the folder changed and restarts the game
```

Sync only moves files. It never tells the game to reload: the ordinary hot-reload check does that, the same one the PC uses.

## Finding the PC

The phone is not told the PC's address. It works it out:

1. If `Documents/server.txt` holds an address that still answers, use it.
2. Otherwise try every address on the phone's own network, asking each `GET /api/hello` and looking for the reply `gamelab`.
3. Remember the one that answered.

`server.txt` is visible in the Files app, so it can be edited by hand if discovery picks the wrong machine.

## Deciding what to download

::: source engine/sync.cpp sync_games

For each game, the phone keeps a small record (`.sync`) of the hash of every file it downloaded. A file is fetched when its hash in the manifest differs from the record, or when the file itself is missing. Files and whole games that have disappeared from the PC are removed from the phone, using the record, so only files that sync itself downloaded are ever deleted.

Synced games take priority over the copies bundled inside the app. That is how you can change a game without rebuilding the app that contains it.

## Never trusting the file list

The phone writes whatever paths the manifest names. A wrong or malicious list must not be able to write outside the games folder, so every name is checked first:

::: source engine/sync.cpp valid_name safe_path

A game name is lowercase letters, digits and dashes. A path may not be absolute, contain a backslash, or contain `.` or `..` as a part. Anything else is ignored.

## Commands from the PC

Typing `play coin-dash`, `menu`, `reload` or `lua <code>` at the `dev.py` prompt sets a command with a new id. The phone sees the id change in its next `/api/state` poll and acts once.

`lua <code>` runs a line of Lua inside the running game and sends back what it returns. It is how you change a variable or call a function on the phone while the game is running.

## Logs travel the other way

`print()` in a game, and Lua errors, are posted to `POST /api/log` and appear in the `dev.py` window. That is the only data the phone sends, apart from its build code.

## The build code

Every poll includes the app's build code. `dev.py` compares it with the fingerprint of the C++ on disk and tells you when the phone's app is older. If a newer build is waiting on the PC, the game list shows **DOWNLOAD UPDATE**. See [How work reaches the phone](../onboarding/shipping.md).

## Security

!!! warning "This link is for development on a network you trust"
    The connection between phone and PC is plain HTTP with no login. Anyone on the same Wi-Fi could, in principle, serve the phone their own Lua. That is acceptable at home or in the office and nowhere else. Do not run the loader on public Wi-Fi. It is one more reason the code is left out of release builds entirely.

What limits the damage if it did happen: games run in a Lua state with no file or system access, they can only call web hosts their `game.cfg` lists, and sync can only write inside the games folder.

## Working on this file

- **It cannot be compiled on Windows.** It uses POSIX sockets, so the PC build leaves it out. Every GitHub build first compiles it with `tests/sync_test.cpp` and runs it against a real `dev.py` on the Mac; the app is only built if that passes. Add a check there for anything you change here.
- The namespace is `devsync` because `sync` is already a POSIX function.
- On Linux, `gamelab --phone <folder>` makes a desktop build behave like the loader, which is the only way to run this code off the phone.
- While it searches for the PC, the phone opens and drops many connections. Python on Windows would print an error for each; `QuietServer` in `dev.py` hides those harmless messages.

## Exercises

??? example "1. You rename `main.lua` to `game.lua` on the PC. What does the phone do?"
    Downloads `game.lua` (new in the manifest) and deletes `main.lua` (in its record, gone from the manifest). The game then shows "This game has no main.lua", because the engine always starts from that name.

??? example "2. Why hash file contents instead of comparing modified times?"
    The PC's and the phone's clocks differ, and git changes modified times when switching branches. Contents are the truth.

??? example "3. Could a manifest line with the path `../../Library/x` write outside the games folder?"
    No. `safe_path` rejects any path with `..` as a part, and the line is ignored.

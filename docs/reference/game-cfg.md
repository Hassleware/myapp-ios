# game.cfg

An optional file in a game's folder with settings the engine needs before the game's Lua runs. Lines are `key = value`. A line starting with `#` is a comment.

```ini
# Stop the phone turning the game.
orientation = landscape

# The only web hosts this game may call.
hosts = api.example.com scores.example.org
```

## Keys

| Key | Values | Default | Effect |
|---|---|---|---|
| `orientation` | `landscape`, `portrait` | Either way up | Which ways the phone may be held. In a standalone build this is also written into the app's own settings |
| `hosts` | Host names separated by spaces or commas | None: every web request is refused | The hosts `net.request` may call. A listed host also allows its subdomains |
| `backend` | An `https://` address | None | Where `net.backend` goes in a **standalone release**. While developing it is ignored and calls go to `backends/<game>/server.py` on your PC. See [Web calls](../engine/networking.md#the-games-own-backend) |

Unknown keys are ignored.

## How it is read

::: source engine/main.cpp config_value

It is read each time a game starts, so a change takes effect on the next reload, on the PC and on the phone. It is an ordinary game file and is synced like any other.

## What a new game starts with

`python tools/dev.py new <name>` copies this into the game, with every line commented out:

::: file tools/template/game.cfg

## Adding a key

Read it in `start_game` in `engine/main.cpp`, next to the existing ones, and pass the value to the subsystem that needs it:

::: source engine/main.cpp start_game

Then add a row to the table above. If a standalone build needs the value at build time, as `orientation` does, `CMakeLists.txt` must read it too; search that file for `game.cfg`.

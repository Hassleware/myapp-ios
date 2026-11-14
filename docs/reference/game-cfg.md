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

Unknown keys are ignored.

## How it is read

::: source engine/main.cpp config_value

It is read each time a game starts, so a change takes effect on the next reload, on the PC and on the phone. It is an ordinary game file and is synced like any other.

## The test game's file

::: file games/net-demo/game.cfg

## Adding a key

Read it in `start_game` in `engine/main.cpp`, next to the existing two, and pass the value to the subsystem that needs it:

::: source engine/main.cpp start_game

Then add a row to the table above. If a standalone build needs the value at build time, as `orientation` does, `CMakeLists.txt` must read it too; search that file for `game.cfg`.

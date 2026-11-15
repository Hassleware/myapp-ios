# dev.py and its server

`tools/dev.py` is the one tool. This page lists its commands and the small HTTP protocol the phone uses to talk to it. The tutorial for changing the tool is [Python](../languages/python.md).

## Commands

Run as `python tools/dev.py <command>`.

| Command | What it does |
|---|---|
| *(none)* | Watch the repository, serve the phone, and show a prompt |
| `run [game]` | Build the PC version and play a game. With no game, shows the game list |
| `new <name>` | Create a game from the template. Names are lowercase letters, digits and dashes |
| `backend <game>` | Give a game its own server code in `backends/<game>/server.py`. See [Web calls](../engine/networking.md#the-games-own-backend) |
| `ship [message]` | Commit, push, wait for the iPhone build, fetch it for the phone |
| `status` | What is out of date, and the command that fixes each thing |
| `check <game>` | Play a game inside every iPhone shape and list what each would hide |
| `release <game>` | Build one game as its own standalone app |
| `doctor` | Check that the tools this repository needs are installed |
| `docs` | Serve this handbook |
| `fingerprint` | Print the build code of the native files on disk |
| `shaders` | Compile every shader |

## At the prompt

While the tool is watching, these can be typed at its `>` prompt:

::: source tools/dev.py HELP

## The server

The tool serves plain text over HTTP on port **8765**. The loader app is the only client.

| Request | Reply |
|---|---|
| `GET /api/hello` | `gamelab`. How the phone recognises the PC |
| `GET /api/state?build=<code>` | Five lines: games revision, command id, action, game, and the build code of the app held on the PC |
| `GET /api/manifest` | One line per game file: `game<TAB>path<TAB>sha1` |
| `GET /files/<game>/<path>` | The file's contents |
| `GET /app/GameLab.ipa` | The newest app build, saved by Safari as `GameLab-<build code>.ipa` |
| `POST /api/log` | Body `level<TAB>game<TAB>message`. Shown in the tool's window |
| Any method on `/backend/<game>/<path>` | Passed to `handle()` in `backends/<game>/server.py`, and its answer returned |

The handler:

::: source tools/dev.py do_GET do_POST

Only files inside a game's folder can be served. This is the function that decides:

::: source tools/dev.py resolve

!!! warning "A development server"
    It has no login and no encryption, and listens on your whole local network. Run it on networks you trust. See [Wi-Fi sync](../engine/sync.md#security).

## Live or native

How the tool labels each change you save:

::: source tools/dev.py describe_changes

## State

The tool remembers a few facts between runs in `tools/.state.json`, which is not committed: the build code last shipped, the one last built for the PC, and the one held in `tools/.ipa/`. `status` reads it to tell you what is behind.

## The handbook

::: source tools/dev.py cmd_docs

The handbook is served on port **8766**, one above the dev server.

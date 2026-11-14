# How work reaches the phone

There are two kinds of change in this project, and they travel to the phone in completely different ways. Knowing which kind you are making tells you whether you wait one second or ten minutes.

## The rule that shapes everything

**iOS will not run compiled code that arrives after an app is installed.** That is a technical limit of the operating system, not a policy we chose.

So the app is split in two:

| | Compiled into the app | Sent as data |
|---|---|---|
| What | The C++ engine, Objective-C++ files | Lua, compiled shaders, models, textures, sounds |
| Folders | `engine/`, `ios/`, `CMakeLists.txt` | `games/` |
| `dev.py` calls it | **native** | **live** |
| Reaches the PC window | `run <game>` rebuilds | On save |
| Reaches the phone | Build on GitHub, install by hand | Over Wi-Fi on save |
| Time | About 10 minutes | About 1 second |

This is why gameplay is written in Lua, and why the project says **prefer data over code**. Anything you can express in Lua or a data file, you can try on the phone immediately.

## Live changes

While `python tools/dev.py` runs, it watches `games/`. When you save a file:

1. `dev.py` prints a `[live]` line.
2. If the file is a `.slang` shader, it is compiled to SPIR-V first.
3. The app on the phone asks the PC once a second whether anything changed, downloads the files that did, and restarts the game.

The details are in [Wi-Fi sync](../engine/sync.md).

## Native changes

When you save a C++ file, `dev.py` prints a `[native]` line and tells you what to do:

- **For the PC:** type `run <game>`. The engine is recompiled, usually in seconds.
- **For the phone:** type `ship "what changed"`.

`ship` does all of this:

1. Compiles every shader, so the app is not built with a stale one.
2. Commits and pushes your work.
3. Waits for GitHub to build the iPhone app on a Mac it provides.
4. Downloads the finished app to `tools/.ipa/` on your PC.

Here is the function, read from the tool:

::: source tools/dev.py cmd_ship

Then on the phone:

1. Open Game Lab and tap **DOWNLOAD UPDATE** on the game list. Safari saves the file.
2. Open LiveContainer, tap **+**, and pick `GameLab-<build code>.ipa`.

!!! warning "The app cannot install itself"
    Earlier attempts to install from inside the app failed, because LiveContainer will not install an app while one is running inside it. The two manual steps above are the tested route. Do not spend time on in-app install links.

## The build code

How does the tool know the phone's app is older than your C++? Every build carries a **fingerprint**: a short code computed from the contents of every native file.

::: source tools/dev.py native_files fingerprint

- The GitHub build bakes the code into the app.
- The app shows it on the game list and reports it to `dev.py` every second.
- `dev.py` compares it with the fingerprint of the files on your disk, and warns when they differ.

The downloaded file is named after the same code, so `GameLab-4ca59e25.ipa` holds build `4ca59e25`.

??? question "I changed only a comment in a C++ file. Do I need a new build?"
    The fingerprint will change, so `dev.py` will say the phone is out of date. The app would behave the same, but the tool cannot know that. Either ship, or ignore the notice until your next real change.

## Check yourself

??? question "You changed the speed of the player in `main.lua`. What do you run?"
    Nothing. Save the file. It is a live change.

??? question "You added a new function to `engine/script.cpp`. What do you run?"
    `run <game>` to test on the PC, then `ship "message"` and the two install steps for the phone.

??? question "You edited a shader in `games/my-game/shaders/`. Live or native?"
    Live. `dev.py` compiles it and the phone downloads the compiled file.

??? question "You edited `engine/shaders/common.slang`. Live or native?"
    This one is special. Every shader includes it, so `dev.py` recompiles all of them. Game shaders reach the phone live. The engine's own compiled shaders are bundled inside the app, so a change to the standard look needs `ship`.

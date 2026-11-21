# A frame, start to finish

Everything the engine does is driven from `engine/main.cpp`. If you understand this one file's flow, you can find anything else.

## How the app is structured

SDL3 asks an app to provide four functions and then calls them itself. There is no `main` loop of our own.

| SDL calls | When | We use it to |
|---|---|---|
| `SDL_AppInit` | Once at start | Open the window, start the renderer, audio and input, find the games |
| `SDL_AppEvent` | For each touch, key press or system event | Handle pause and resume and the PC keys; pass the rest to `input` |
| `SDL_AppIterate` | Once a frame | Run one frame |
| `SDL_AppQuit` | Once at exit | Shut everything down in order |

::: source engine/main.cpp SDL_AppIterate

When the app is in the background it does nothing but sleep, which is what iOS expects.

## One frame

::: source engine/main.cpp frame

In order:

1. **Measure time.** `dt` is the seconds since the last frame, capped at 0.05 so a long stall does not make everything jump.
2. **Loader only: act on commands from the PC** such as `play`, `reload` and `lua`.
3. **Hot reload.** Every 400 ms, see whether the running game's folder changed.
4. **Update the game** through `script::update`.
5. **Draw**: `gfx::begin_frame`, the game's `draw()`, the touch controls, the HUD, then `gfx::end_frame`, which is where drawing really happens.
6. **`input::end_frame`** clears the "pressed this frame" flags.

## Inside the game's update

::: source engine/script.cpp update

Why `fixed_update` exists: frame rates vary. If movement ran once per frame, a jump would be a different height at 30 and at 120 frames a second. So time is saved up and spent in steps of exactly one sixtieth of a second. A slow frame runs two or three steps; a fast one sometimes runs none. At most five run in one frame, so a long stall cannot snowball.

## Starting a game

::: source engine/main.cpp start_game release_assets

Starting a game:

1. Unloads the previous Lua state and lets go of what it loaded.
2. Reads the game's `game.cfg` for its orientation and the web hosts it may call.
3. Makes a new Lua state, runs `main.lua`, and calls `init()`.

`release_assets` is careful on a reload of the **same** game: files that have not changed on disk are kept, so a hot reload does not re-read every texture and model. When the game changes, everything is freed.

## Hot reload

The whole mechanism is a number that changes when anything in the game's folder does:

::: source engine/main.cpp game_folder_stamp

It hashes each file's name, size and modified time. The frame loop compares it with the last value and restarts the game on a difference. Because the loader's sync writes downloaded files into a folder, the same code reloads the game on the phone. There is no separate "reload" message for file changes.

## When a game has an error

Lua errors never crash the app. `script.cpp` runs every call into the game in protected mode:

::: source engine/script.cpp call

After an error the game stops being updated and drawn, the message with its traceback is drawn on screen and sent to `dev.py`, and the engine waits. Saving a fixed file triggers a reload and the game comes back.

## Pause and resume

When the player switches away, iOS tells the app. The engine calls the game's `on_pause()`, silences audio and stops running frames. `on_pause` is the moment to save: iOS may end a background app at any time without further notice.

## The crash marker

At start the app writes a small marker file, and removes it when it goes to the background or exits normally. If the marker is already there at the next start, the last run ended unexpectedly, and the tail of the previous log is sent to `dev.py` as an error. It is a crude crash reporter that needs no extra library.

## Exercises

??? example "1. What is the longest `dt` a game's `update` can ever be given?"
    0.05 seconds (times the time scale). See `SDL_min(..., 0.05f)` at the top of `frame`.

??? example "2. A game sets `sys.time_scale(0)`. Does `draw()` still run?"
    Yes. Only the time passed to the update functions is scaled. Drawing carries on, which is what lets a pause menu animate using `sys.real_time()`.

??? example "3. Why does the hot-reload check skip while `syncing` is true?"
    Files are still being written. Reloading halfway would run a game with a mix of old and new files.

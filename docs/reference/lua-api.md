# Lua API

Everything the engine gives to a game. Each entry shows the call, what it does, and a link to the C++ function behind it.

::: lua-api-status

This page is built from `docs/api/gamelab.lua`. The same file gives VS Code its autocomplete. To add or change an entry, see [Writing these docs](../guides/docs.md#the-lua-api-description).

## Conventions

- **`<transform>`** is up to ten numbers: position `x, y, z`, rotation as a quaternion `qx, qy, qz, qw`, then scale `sx, sy, sz`. Leave the end off for no rotation and a scale of 1.
- **Colours** are `r, g, b [, a]`, each from 0 to 1.
- **`[brackets]`** mark arguments you may leave out.
- **Handles.** Anything loaded or made comes back as a number to pass to other functions. A missing file gives a pink stand-in and one warning in the `dev.py` window, never a crash.
- **2D units.** The screen's shorter side is always 720.

## Callbacks

Functions *you* write and the engine calls.

::: lua-api callbacks

## gfx: drawing

::: lua-api gfx

## model: Blender files and animation

Clips and nodes can be given by name or by number. More in [Models and animation](../engine/models.md).

::: lua-api model

## phys: collision

`layer` and `mask` are bit sets (1, 2, 4, 8...). A question only sees bodies that share a bit with its mask. More in [Collision](../engine/collision.md).

::: lua-api phys

## audio: sound

More in [Audio](../engine/audio.md).

::: lua-api audio

## input: touch, sensors, keyboard

More in [Input and keyboard](../engine/input.md).

::: lua-api input

## net: web calls

More in [Web calls](../engine/networking.md), including the safety rules.

::: lua-api net

## save: values that survive

::: lua-api save

## sys: time and the app

::: lua-api sys

## Globals

::: lua-api globals

## What is not available

Games get Lua's `table`, `string`, `math`, `utf8` and `coroutine` libraries. There is no `io`, `os`, `debug`, `require`, `dofile` or `loadfile`. See [Lua](../languages/lua.md#what-you-can-and-cannot-use) for what to use instead.

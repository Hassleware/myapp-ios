# lib.lua helpers

`lib.lua` is a library written in Lua on top of the engine's API. Every new game gets its own copy from `tools/template/lib.lua`, and it is the game's to change. Because it is Lua, it travels to the phone like any other game file and needs no app build.

```lua
local lib = include("lib.lua")

function update(dt)
  lib.update(dt)        -- needed for timers, scripts and anything that waits
end
```

The whole file is below, read from the template. Its comments are the documentation: each helper has a line or two above it saying how it is used.

| Section | What is in it |
|---|---|
| Numbers and vectors | `clamp`, `lerp`, `approach`, `length`, `normalize`, `cross`, `dot` |
| Rotations | Quaternions from an axis or from yaw, pitch and roll; multiply, rotate, slerp, look-at; `attach` |
| Paths | A smooth curve through points |
| Time | `run`, `wait`, `wait_until`, `after`, `cancel` |
| Events | `on`, `emit`, `emit_later` |
| State machines | `machine` |
| Animation player | `animation`: cross-fades, loops, timed events |
| Sound | `cue` (shuffled takes), `duck` |
| Words | Text in more than one language |
| Particles | `emitter` |
| Loading | `loader`: one thing per frame, with progress |
| Debugging | `wire_box`, `label`, `strict` |

!!! tip "Improving it for everyone"
    A change to a game's own `lib.lua` affects only that game. If a helper would be useful to every game, add it to `tools/template/lib.lua` as well, so new games start with it.

::: file tools/template/lib.lua

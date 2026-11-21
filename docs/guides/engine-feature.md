# Add an engine feature

This is the route every new capability takes from an idea to a function a game can call. It is worth following exactly, because the last step costs a ten-minute build and a manual install.

## First: does it need C++ at all?

| If the feature... | Then |
|---|---|
| Can be written with existing Lua functions | Write it in Lua. Put it in the game, or in `tools/template/lib.lua` if every game would want it |
| Is a different look for a surface | Write a shader |
| Is a pure-Lua library someone else wrote | Drop the file in the game's folder and `include` it |
| Needs the operating system, the graphics chip, a file format, or real speed | It needs C++. Read on |

Search the [Lua API](../reference/lua-api.md) before starting. The engine may already do it.

## The six steps

We will follow a real feature: the phone's tap motor, `input.haptic`.

### 1. Write it in C++

Put it in the subsystem it belongs to. Declare it in the header with a comment that says what a caller needs to know:

::: source engine/input.h haptic

Implement it in the `.cpp`. If it only exists on one platform, fence that with `#if` and do something harmless elsewhere:

::: source engine/input.cpp haptic

If the feature is a new subsystem, make a new pair of files and add the `.cpp` to `ENGINE_SOURCES` in `CMakeLists.txt`.

### 2. Give it to Lua

In `engine/script.cpp`, write a function named `l_<name>` that reads its arguments from Lua and calls yours:

::: source engine/script.cpp l_haptic

Then add it to the right table, in `script::load`. This one is in `inputFunctions`:

```cpp
const luaL_Reg inputFunctions[] = { ..., {"haptic", l_haptic}, ... };
```

The helpers that keep these functions short:

| Helper | Reads |
|---|---|
| `num(l, i, fallback)` | Argument `i` as a number, or the fallback if absent |
| `flag(l, i, fallback)` | A boolean |
| `handle(l, i)` | A handle |
| `luaL_checkstring(l, i)` | A string; raises a Lua error if it is not one |
| `luaL_optstring(l, i, "x")` | A string, or a default |

Return the number of values you pushed.

### 3. Describe it

Add an entry to `docs/api/gamelab.lua`: the call in backticks, one or two lines on what it does, and an empty function.

```lua
--- `input.haptic("light" | "medium" | "heavy" | "success" | "warning" | "error")`
--- A tap from the phone's motor.
function input.haptic(kind) end
```

This one entry puts the function in the [Lua API](../reference/lua-api.md) reference, in VS Code's autocomplete, and in the check that fails if the C++ and the description disagree. If you skip this step, the handbook's build warns you by name.

### 4. Use it from a game, on the PC

```
run net-demo
```

Call your function from a game's `main.lua`. Lua changes reload instantly, so you can iterate on *how the function is used* without rebuilding. Change the C++ and you run `run` again.

If you touched the renderer, run with the validation layers. See [Renderer](../engine/renderer.md#working-on-the-renderer).

### 5. Ship it

```
ship "Add input.haptic"
```

Then install on the phone: **DOWNLOAD UPDATE**, then **+** in LiveContainer.

### 6. Test on the phone and write down the result

`CLAUDE.md` has a table of what has and has not been tested on a real device. Add your feature, and be honest: "tested on the PC, untested on the phone" is useful to the next person. "Done" when it has only run on Windows is not.

## Designing the Lua side

The Lua function is the part people live with. Some conventions that keep the API consistent:

- **Numbers, not tables, for positions and colours.** `gfx.cube(x, y, z, ...)`, not `gfx.cube({x, y, z})`. A table per call is garbage for Lua to collect, thousands of times a frame.
- **Optional arguments go at the end**, each with a sensible default.
- **Things that are loaded return a handle**, a number. Never give Lua a pointer.
- **Files are named, not located.** `load("hero.glb")`, never a path. The engine knows where the game's folder is on each platform.
- **A failure should be visible, not fatal.** Return nothing, or a stand-in, and report a warning. A game must not crash because a file is missing.
- **Anything slow goes on a thread and answers later**, the way `net.request` does. Never block a frame.
- **If it only works on the phone, it must do nothing gracefully on the PC.**

## Before you ship: a checklist

- [ ] Builds and runs on the PC with `run <game>`
- [ ] New `.cpp` or `.mm` files are in `CMakeLists.txt`
- [ ] The function is described in `docs/api/gamelab.lua`, and the handbook builds without warnings
- [ ] Renderer changes are clean under the validation layers
- [ ] `net.cpp` changes pass `GAMELAB_NET_CHECK=1`
- [ ] Code that cannot be compiled on Windows (`devsync.cpp`, `ios/`) has been read twice
- [ ] `FrameData` / `PushData` changes were made on both the C++ and the Slang side, and every shader recompiled
- [ ] Any shortcut taken is marked with a `ponytail:` comment saying when to revisit it

## A second example to study

`net.request` is the largest recent addition and touches every layer: a new subsystem (`net.h`, `net.cpp`), a platform file (`ios/http.mm`), a Lua function with a callback (`l_net_request`), a setting in `game.cfg`, and delivery of answers in `script::update`.

::: source engine/script.cpp l_net_request

Notice how it keeps the game's function: `luaL_ref` stores it in Lua's registry and gives back a number, which is kept against the request's id until the answer arrives. The whole flow is drawn on the [Web calls](../engine/networking.md#how-a-request-travels) page.

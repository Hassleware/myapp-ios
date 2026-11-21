# C++: the language of the engine

The engine is about 7,000 lines of C++17 in `engine/`. This page is for someone who knows some C++ (or C, Java or C#) and needs to become productive in *this* code. It covers how the code is organised, the handful of idioms it uses everywhere, and how to read a file. To add something, follow [Add an engine feature](../guides/engine-feature.md) afterwards.

## The style in one paragraph

Plain functions in namespaces. No class hierarchies, no templates beyond one small pool, no exceptions, no smart-pointer webs. Each subsystem is one `.cpp` file with a short header listing what other files may call. State that belongs to a subsystem lives in file-level variables inside an unnamed namespace, where nothing outside the file can touch it. Comments say *why*, in plain words.

If you come from a codebase of deep class trees, this will look almost too simple. That is intended: the whole engine can be read in an afternoon.

## Anatomy of a subsystem

Take web requests. The header is the contract:

::: file engine/net.h

Everything a caller needs is there, and the comment at the top states the rules. Now the layout of the matching `.cpp`:

```cpp
#include "net.h"            // its own header first
#include <mutex>            // then the standard library

namespace net {
namespace {                 // unnamed: private to this file
    const size_t MAX_SEND = 1 << 20;
    std::vector<std::string> hosts;        // the subsystem's state
    bool host_allowed(...) { ... }         // helpers
}  // namespace

void set_hosts(...) { ... }                // the functions the header promised
int send(...) { ... }
}  // namespace net
```

Every engine file follows this pattern. A private helper from the real file:

::: source engine/net.cpp host_allowed

## Idioms you will meet

### Handles, not pointers

Lua must never hold a C++ pointer: a script could keep one after the thing it points to is gone. Instead the engine hands out numbers, and each number carries a generation that is checked on use.

::: file engine/handles.h

A texture, mesh, font, sound and collision body are all handles from a pool like this. A stale handle simply finds nothing.

### Platform differences with `#if`

One source builds for Windows, Linux and the iPhone. Differences are fenced with the preprocessor, as close to the difference as possible:

::: source engine/input.cpp haptic

The names you will see:

| Name | Meaning |
|---|---|
| `SDL_PLATFORM_IOS` | Building for the iPhone |
| `_WIN32` | Building for Windows |
| `LOADER_SYNC` | The loader build, with Wi-Fi sync. Never set in a standalone game |
| `SINGLE_GAME` | A standalone build of one game |
| `USE_VOLK` | Desktop: Vulkan functions are loaded at run time. Not set on the iPhone |
| `GFX_SCREENSHOT` | A PC build that can save a frame to a file, for tests |

### `ponytail:` comments

A comment starting with `ponytail:` marks a place where the simplest thing that works was chosen on purpose, and says what to do when it stops being enough.

```cpp
// ponytail: no HTTPS on Linux. Add libcurl here if the Linux build ever needs to call APIs.
```

Do not "fix" these in passing. They are decisions. When you make one yourself, leave the same kind of note.

### Errors are values or log lines

There are no exceptions. A function that can fail returns `false`, `0` or an empty string, and the caller decides. Problems a game developer should see are reported so they reach the `dev.py` window. A missing file gives a visible stand-in rather than a crash, because a crash on the phone tells you far less than a pink texture does.

### Matrices

Matrices are sixteen floats in column-major order, the same as GLSL and Vulkan expect. Shaders are compiled to match, so Slang code writes `mul(matrix, vector)`.

## How Lua calls C++

Every function a game can call is a small C++ function in `engine/lua_<table>.cpp` that reads its arguments from Lua's stack, calls the engine, and pushes results back.

::: source engine/lua_gfx.cpp l_sphere l_project

The pattern:

- The argument is always `lua_State* l`.
- `num(l, 4, 1)` reads argument 4 as a number, or gives `1` if it was left out. Helpers like it keep these functions short.
- The return value is **how many results** were pushed. `return 0` means none.

The function is then listed in a table, and the table is given a name in Lua:

::: source engine/script.cpp register_table

And this is how the engine calls back into Lua, safely:

::: source engine/script.cpp call

Note what it guards against: a game that never defined the function, and a game whose function raises an error. Either way the engine carries on and the error is shown.

## Reading the code: a suggested order

1. `engine/main.cpp`: the function `frame`. Everything that happens each frame starts there. See [A frame, start to finish](../engine/lifecycle.md).
2. `engine/lua_<table>.cpp`: skim the `l_` functions for an area you care about.
3. The header of that area: `gfx.h`, `phys.h`, `audio.h`. All are on the [C++ headers](../reference/headers.md) page.
4. Only then the `.cpp` behind it.

`gfx.cpp` is the largest file by far. Read [Renderer](../engine/renderer.md) before opening it.

## Building and testing

| To do this | Run |
|---|---|
| Build and run on the PC | `python tools/dev.py run <game>` |
| Check every Vulkan call you make | Set `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` before running (needs the Vulkan SDK) |
| Build for the phone | `python tools/dev.py ship "message"` |
| See why a phone build failed | `gh run view <id> --log-failed` |

Two things cannot be compiled on Windows: `engine/devsync.cpp` (it uses POSIX sockets) and the files in `ios/`. The GitHub build is their first test, so read those changes twice before shipping.

## Exercises

??? example "1. Find where the 60-times-a-second step is defined"
    Search `engine/script.cpp` for `FIXED_STEP`. Then find where it is used in `update`, shown on the [Lua page](lua.md#a-game-is-a-folder-with-a-mainlua).

??? example "2. What does `gfx.sphere` do if you leave out the colour?"
    Read `l_sphere` above. Each colour argument is read with a default of `1`, so the sphere is white.

??? example "3. Add a Lua function `sys.build()` that returns the build code"
    Follow [Add an engine feature](../guides/engine-feature.md). The value is the `BUILD_FINGERPRINT` definition; search `engine/main.cpp` for how it is used.

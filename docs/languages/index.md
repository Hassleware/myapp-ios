# Which language goes where

Six languages are used in this repository. Each has one job, and the jobs do not overlap. You do not need all six on day one: most people start with Lua and add others as their work calls for them.

| Language | Its job here | Files | Changes take | Tutorial |
|---|---|---|---|---|
| **Lua 5.4** | Everything a game does: rules, movement, menus, drawing calls | `games/**/*.lua` | 1 second | [Lua](lua.md) |
| **C++17** | The engine: drawing, sound, collision, input, the Lua host | `engine/*.cpp`, `engine/*.h` | Seconds on the PC, 10 minutes for the phone | [C++](cpp.md) |
| **Slang** | Shaders: the colour of every pixel | `**/shaders/*.slang` | 1 second | [Slang](slang.md) |
| **Python 3** | The dev tool and the handbook's build | `tools/dev.py`, `docs/hooks.py` | Immediately | [Python](python.md) |
| **Objective-C++** | The few things only Apple's own frameworks can do | `ios/*.mm` | 10 minutes | [Objective-C++](objc.md) |
| **CMake, YAML** | Describing how the engine is compiled, on your PC and on GitHub | `CMakeLists.txt`, `.github/workflows/build.yml` | 10 minutes | [CMake and YAML](build-files.md) |

## How to choose

Start from what you want to change, and take the first row that fits.

| You want to... | Write it in | Why |
|---|---|---|
| Change how a game plays, looks or sounds | Lua | It reloads on the phone in a second |
| Change how a surface is shaded | Slang | Also live |
| Do something Lua has no function for | C++, then expose it to Lua | The engine is the only place that can reach the system |
| Reach an Apple framework (purchases, the Keychain, notifications) | Objective-C++, called from C++ | Apple's frameworks speak Objective-C |
| Make a repetitive job a single command | Python, in `dev.py` | One tool, no extra installs |
| Add a source file or a library | CMake | So both the PC and the iPhone builds know about it |

!!! tip "The rule of thumb"
    Go as far left in that list as you can. A feature written in Lua is one you can tune on the phone all afternoon. The same feature in C++ costs a build and a manual install for every attempt.

## Languages we chose not to use

- **JavaScript and web views.** The project is native on purpose. Do not add a web view to the app.
- **Swift.** It would work for the `ios/` files, and may be used there in future. Objective-C++ was chosen because it can be called directly from C++ with no bridging layer.
- **A physics library.** Physics simulation is written in Lua on top of the engine's collision questions. Do not add Jolt or similar.

## How the languages meet

```
   Lua game  ──calls──▶  C++ engine  ──calls──▶  Vulkan (drawing)
  main.lua               script.cpp               SDL3 (window, touch, sound)
                         gfx.cpp ...  ──calls──▶  Objective-C++ (ios/*.mm) ──▶ Apple frameworks
                              ▲
   Slang shader ─compiled by dev.py (Python)─▶ .spv file loaded by gfx.cpp
```

- Lua reaches C++ through the tables `gfx`, `model`, `phys`, `audio`, `input`, `net`, `save` and `sys`. Each function in them is a small C++ function in `engine/script.cpp`.
- C++ reaches Objective-C++ through plain C functions, declared with `extern "C"`.
- Slang never talks to Lua directly. Lua sets numbers with `gfx.param`, C++ passes them to the graphics chip, and the shader reads them.

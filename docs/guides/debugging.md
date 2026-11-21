# Debugging

What to reach for when something is wrong, roughly in the order to try it.

## First questions

| Symptom | Look at |
|---|---|
| The game shows an error on screen | Read it. It has the file, the line and a traceback. Fix and save |
| Nothing I change shows up on the phone | Is `dev.py` running? Is the phone on the same Wi-Fi? Does `dev.py` say a phone is connected? |
| A new Lua function "does not exist" on the phone | The phone's app is older than your C++. `dev.py` says so. `ship` |
| Something is pink | A file is missing. The warning in `dev.py` names it |
| It works on the PC and not on the phone | See [Only on the phone](#only-on-the-phone) below |

```
python tools/dev.py status
```

prints what is out of date and the command that fixes each thing.

```
python tools/dev.py doctor
```

checks that your tools are installed.

## Printing

`print(...)` in a game shows in the `dev.py` window, from the PC and from the phone alike. It is the fastest tool there is.

```lua
print("player", x, y, z, state)
```

To watch a value live, draw it instead:

```lua
gfx.text(string.format("speed %.2f", speed), 20, 20, 20, 1, 1, 0)
```

## The HUD

++f3++ on the PC, or `sys.hud(true)` from a game, shows:

| Line | Meaning | Worry when |
|---|---|---|
| FPS, MS | Frames a second and milliseconds per frame | Over 16.7 ms: you are below 60 |
| DRAWS | Draw calls this frame | Many hundreds on a phone |
| VERTS | Vertices drawn | Hundreds of thousands |
| TEX, MESH | Textures and meshes alive | Climbing every frame: something is leaking |
| LUA KB | Memory Lua is holding | Climbing steadily: tables made every frame |
| SOUNDS | Voices playing | Near 32 |

`sys.stats()` returns the main numbers to Lua if you want to log them.

## Poking a running game

At the `dev.py` prompt, while a game runs on the phone:

```
lua player.speed = 9
lua sys.time_scale(0.2)
lua score
```

Each line runs inside the game and prints what it returns. Changing a number this way is faster than editing and saving, and it does not restart the game. It only reaches globals: a `local` at the top of `main.lua` is not visible. During development, keep the state you want to poke in a global table.

Stopping time is especially useful:

| Command | Effect |
|---|---|
| `lua sys.time_scale(0)` | Freeze the game. Drawing continues |
| `lua sys.step()` | Advance exactly one tick |
| `lua sys.time_scale(0.2)` | Slow motion |
| `lua sys.time_scale(1)` | Back to normal |

## Seeing what is not drawn

- `phys.debug()` inside `draw()` draws every collision shape.
- `gfx.line(...)` draws a line in the world: a ray you cast, a velocity, a path.
- `lib.wire_box` and `lib.label` in `lib.lua` draw a box in lines and text floating at a point.

## Catching typos

A misspelt variable in Lua is silently `nil`. One line at the top of a game turns that into an error naming the variable:

```lua
local lib = include("lib.lua")
lib.strict()
```

::: source tools/template/lib.lua lib.strict

## The PC window as a phone

| Key | Effect |
|---|---|
| ++f1++ | Step through iPhone shapes, with notch, island and home bar drawn |
| ++f2++ | Turn it sideways |
| ++f5++ | Restart the game |

Environment variables, set before running:

| Variable | Effect |
|---|---|
| `GAMELAB_DEVICE=1u` | Start inside phone frame 1, upright (`1` for sideways). Frame 1 is the iPhone 11 shape, the team's test phone |
| `GAMELAB_WINDOW=414x896` | Open the window at that size |
| `GAMELAB_NO_MSAA=1` | Turn off edge smoothing |
| `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` | Check every Vulkan call (needs the Vulkan SDK) |

In PowerShell:

```powershell
$env:GAMELAB_DEVICE = "1u"
python tools/dev.py run net-demo
```

## Screenshots without a person

For automated checks, the PC build can save one frame and quit. Configure once with screenshots on, then:

```powershell
cmake -S . -B build -DGFX_SCREENSHOT=ON
cmake --build build --config RelWithDebInfo
$env:GAMELAB_SHOT = "out.bmp"; $env:GAMELAB_SHOT_FRAME = "60"
.\build\RelWithDebInfo\gamelab.exe net-demo
```

## Only on the phone

The phone differs from the PC in a few specific ways. When something works on one and not the other, it is nearly always one of these.

| Difference | Consequence |
|---|---|
| Vulkan runs through MoltenVK | A rarely used Vulkan feature may be missing. Shaders are translated to Metal on first use |
| The graphics chip is slower | Something smooth on the PC can drop frames |
| File names are case-sensitive | `Hero.glb` and `hero.glb` are different files on the phone and the same on Windows. **Match case exactly** |
| The app is older than your C++ | A function exists on the PC and not on the phone until you `ship` |
| Touch, not mouse | Several fingers at once; no hover |
| The safe area is real | Notch and home bar cover what a plain window showed. Run `check` |

### The log and the crash report

The app writes a log file. If it stops unexpectedly, the next time it starts and finds `dev.py` it sends the last lines of that log, and they appear in the `dev.py` window as an error. So after a crash on the phone: reopen the app with `dev.py` running, and read.

## When a phone build fails

```powershell
gh run view <id> --log-failed
```

`ship` prints the id. See [CMake and YAML](../languages/build-files.md#when-a-phone-build-fails) for the usual causes.

## Asking for help

Bring three things: what you did, what you expected, and what happened instead, with the exact text from the screen or the `dev.py` window. Add the build code shown on the phone's game list. That is nearly always enough for someone to answer without a second question.

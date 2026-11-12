# Game Lab

One repo for many games. A C++ engine (Vulkan, running on Metal through MoltenVK on the iPhone) runs games written in Lua, with shaders written in Slang.

```
engine/     C++: renderer, input, Lua host, Wi-Fi sync for the loader
games/      one folder per game: main.lua, shaders/, anything else it needs
ios/        iPhone app settings
tools/      dev.py, the one tool you run
```

## Daily use

```
python tools/dev.py doctor        check your tools (first time only)
python tools/dev.py               watch the repo and serve the phone; type commands at its prompt
python tools/dev.py run runner    build the PC version and play a game
python tools/dev.py ship "msg"    commit, push, build the phone app, fetch it to this PC
python tools/dev.py status        what is out of date and what to do
```

## Two kinds of change

| Change | Reaches the PC | Reaches the phone |
|---|---|---|
| Lua, shaders, levels, other game files | reloads on save | over Wi-Fi on save, while `dev.py` is running |
| C++ in `engine/`, `CMakeLists.txt`, `ios/` | `run <game>` rebuilds | `ship` builds a new app on GitHub |

`dev.py` tells you which kind each change was. The app shows the fingerprint of the C++ it was built from, and `dev.py` warns when the phone's app is older than the repo.

## Writing a game

`python tools/dev.py new my-game` creates `games/my-game/main.lua`. The engine calls `init()`, `update(dt)` and `draw()`.

```
gfx.clear(r, g, b)                         gfx.camera(ex, ey, ez, tx, ty, tz [, fov])
gfx.light(x, y, z)                         gfx.shader([name])
gfx.cube(x, y, z, sx, sy, sz, r, g, b [, yaw [, alpha]])
gfx.sphere(x, y, z, diameter, r, g, b)     gfx.plane(x, y, z, sx, sz, r, g, b)
gfx.rect(x, y, w, h, r, g, b [, a])        gfx.circle(x, y, radius, r, g, b [, a])
gfx.text(s, x, y, size, r, g, b [, a])     gfx.text_width(s, size)     gfx.size() -> w, h
input.controls{ {type="stick"|"button", id=, x=, y=, size=, label=, key=} }
input.stick(id) -> x, y     input.down(id)     input.pressed(id)
save.get(key, default)      save.set(key, value)
include("other.lua")        print(...)   -- prints show in the dev.py window, from the phone too
```

2D drawing uses a screen whose shorter side is always 720 units, so layouts hold up in both orientations; `gfx.size()` gives the current width and height. On the PC, WASD or the arrow keys drive the first stick, and each button's `key` presses it.

## Shaders

Put `name.slang` in a game's `shaders/` folder and select it with `gfx.shader("name")`. Start from `games/shader-demo/shaders/pulse.slang`. `dev.py` compiles it to SPIR-V when you save, and MoltenVK converts that to Metal Shading Language on the iPhone. The compiled `.spv` files are committed, so the GitHub build needs no shader compiler.

## Updating the app on the phone

Builds are never published. GitHub keeps each one as a private build file, `ship` fetches it to `tools/.ipa/` with your own login, and `dev.py` offers it to the phone over your Wi-Fi. When the PC holds a newer build than the app, the game list shows **INSTALL UPDATE** (hands the file to SideStore) and **DOWNLOAD FILE** (saves it through Safari, to install by hand).

## Releasing one game

`python tools/dev.py release runner` builds `runner` as its own app and saves it to `tools/.ipa/runner.ipa`: only that game inside, no game list, no Wi-Fi syncing. The result is unsigned, which suits SideStore. An App Store upload also needs signing with an Apple Developer Program account.

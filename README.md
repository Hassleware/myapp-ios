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
python tools/dev.py check runner  play a game inside every iPhone shape and list what each phone would hide
```

## Two kinds of change

| Change | Reaches the PC | Reaches the phone |
|---|---|---|
| Lua, shaders, levels, other game files | reloads on save | over Wi-Fi on save, while `dev.py` is running |
| C++ in `engine/`, `CMakeLists.txt`, `ios/` | `run <game>` rebuilds | `ship` builds a new app on GitHub |

`dev.py` tells you which kind each change was. The app shows the fingerprint of the C++ it was built from, and `dev.py` warns when the phone's app is older than the repo.

## Writing a game

`python tools/dev.py new my-game` creates `games/my-game/` with `main.lua` and `lib.lua`. `games/features/main.lua` is a working example of nearly everything below.

The engine calls these if the game defines them: `init()`, `fixed_update(dt)` (a steady 60 times a second: movement and collision), `update(dt)`, `late_update(dt)` (cameras and things attached to other things), `draw()`, `on_pause()`, `on_resume()`.

A transform is ten numbers: position `x, y, z`, rotation as a quaternion `qx, qy, qz, qw`, scale `sx, sy, sz`. Leave the end off for no rotation and scale 1. Colours are `r, g, b [, a]` from 0 to 1. Anything loaded gives back a number (a handle) to use later; a missing file gives a pink stand-in and one warning in the `dev.py` window, never a crash.

```
-- the scene (set every frame, inside draw)
gfx.clear(r, g, b)
gfx.camera(ex, ey, ez, tx, ty, tz [, fov [, near, far [, upx, upy, upz]]])   -- fov below 0: a flat camera that many units tall
gfx.ambient(r, g, b)                       gfx.light(x, y, z [, r, g, b])    -- the sun: direction it shines, and colour
gfx.point_light(x, y, z, radius, r, g, b)  gfx.spot_light(x, y, z, dx, dy, dz, radius, angle, r, g, b)   -- up to 8 in all
gfx.fog(r, g, b, start, end)               gfx.shadows(cx, cy, cz, radius [, strength])   -- sun shadows inside that ball
gfx.sky(texture [, r, g, b])               -- a panorama picture wrapped round the camera
gfx.target(t) ... gfx.target()             -- draw into a texture made by gfx.new_target, then back to the screen

-- how the next things are drawn (stays until changed; gfx.reset() puts it all back)
gfx.shader([name])            gfx.texture(slot 1-4, texture | nil)     gfx.blend("opaque" | "alpha" | "add")
gfx.cull(bool)                gfx.unlit(bool)        gfx.depth(test, write)      gfx.cutoff(alpha)
gfx.cast_shadows(bool)        gfx.param(1-2, x, y, z, w)               gfx.frame_param(1-4, x, y, z, w)   -- numbers for your shader
gfx.font(font | nil)

-- loading and making things
gfx.load_texture(file [, "nearest clamp nomip data"])     gfx.new_texture(w, h, pixel_string [, options])
gfx.new_target(w, h)          gfx.texture_size(t) -> w, h          gfx.free_texture(t)
gfx.new_mesh(numbers [, indices [, "pnuc"]])   -- p position, n normal, u texture coordinate, c colour; indices count from 1
gfx.free_mesh(m)              gfx.load_font(file.ttf [, pixel_height])
gfx.CUBE  gfx.SPHERE  gfx.PLANE                -- the built-in meshes

-- drawing in 3D
gfx.draw(mesh, <transform>, r, g, b, a)
gfx.cube(x, y, z, sx, sy, sz, r, g, b [, yaw [, alpha]])
gfx.sphere(x, y, z, diameter, r, g, b [, a])     gfx.plane(x, y, z, sx, sz, r, g, b [, a])
gfx.particles(numbers [, count])               -- squares facing the camera, eight numbers each: x, y, z, size, r, g, b, a
gfx.line(x1, y1, z1, x2, y2, z2, r, g, b, a [, width])
gfx.project(x, y, z) -> sx, sy                 gfx.unproject(sx, sy) -> ox, oy, oz, dx, dy, dz   -- world <-> screen

-- drawing in 2D
gfx.rect(x, y, w, h, r, g, b [, a])        gfx.circle(x, y, radius, r, g, b [, a])
gfx.image(texture, x, y, w, h [, r, g, b, a [, u0, v0, u1, v1]])
gfx.text(s, x, y, size, r, g, b [, a])     gfx.text_width(s, size)     gfx.size() -> w, h
gfx.safe() -> left, top, right, bottom     -- how far to stay in from each edge to clear the notch, island and home bar

-- models: .glb files from Blender (File > Export > glTF 2.0, format "glTF Binary")
model.load(file) -> m
model.nodes(m) -> { {name, parent, x, y, z, qx, qy, qz, qw, sx, sy, sz, mesh, extras = {...}, light, r, g, b, range, angle, fov}, ... }
model.clips(m) -> { {name, duration}, ... }
model.draw(m, <transform>, r, g, b, a [, animator [, node]])     -- node: draw just that part, placed at the transform
model.animator(m) -> a                         model.pose(m, a, clip, time, weight [, clip, time, weight ...])
model.joint(m, a, node) -> <transform>         -- where a bone is now, inside the model
model.local(m, a, node [, <transform>])        -- read or set a bone by hand, after posing
model.sample(m, clip, node, time) -> <transform>
-- clips and nodes are given by name or by number. extras are the object's Custom Properties in Blender.

-- collision (no physics solver: you move things, these tell you what they touch)
phys.sphere(x, y, z, radius [, layer])         phys.capsule(x, y, z, radius, height [, layer])
phys.box(x, y, z, sx, sy, sz [, qx, qy, qz, qw [, layer]])
phys.mesh(mesh, <transform> [, layer])         phys.model(m, <transform> [, layer [, node]]) -> { bodies }
phys.place(body, x, y, z [, qx, qy, qz, qw])   phys.position(body) -> x, y, z      phys.remove(body)      phys.clear()
phys.raycast(ox, oy, oz, dx, dy, dz, distance [, mask [, ignore]]) -> distance, x, y, z, nx, ny, nz, body   (nothing on a miss)
phys.spherecast(ox, oy, oz, dx, dy, dz, distance, radius [, mask [, ignore]]) -> the same
phys.overlap(x, y, z, radius [, mask]) -> { bodies }
phys.move(body, dx, dy, dz [, mask [, step, slope]]) -> x, y, z, grounded, blocked, nx, ny, nz   -- a character that slides along walls
phys.debug()                                   -- draws every shape, inside draw
-- layer and mask are bit sets (1, 2, 4, 8...). A question only sees bodies sharing a bit with its mask.

-- sound (.wav and .ogg). Volumes are decibels: 0 full, -6 about half, -80 silent.
audio.load(file [, stream]) -> sound           -- stream = true for long music
audio.play(sound [, volume, pitch, loop, group]) -> voice
audio.stop(voice [, fade_seconds])             audio.playing(voice)
audio.volume(voice, db)       audio.pitch(voice, p)       audio.pan(voice, -1..1)       audio.lowpass(voice, hz)
audio.position(voice, x, y, z [, min_radius, max_radius])   -- a sound in the world, heard from the camera
audio.listener(x, y, z, rightx, righty, rightz)             -- only if the ears are not the camera
audio.group(group 0-7, db [, fade_seconds])    audio.group_pause(group, bool)

-- input
input.controls{ {type="stick"|"button", id=, x=, y=, size=, label=, key=} }
input.stick(id) -> x, y     input.down(id)     input.pressed(id)
input.touches() -> { {id, x, y, dx, dy, phase = "down"|"move"|"up", claimed}, ... }   -- every finger; the mouse on the PC
input.tilt() -> x, y, z     input.gyro() -> x, y, z     input.haptic("light"|"medium"|"heavy"|"success"|"warning"|"error")
input.pad(name) -> number   -- a game controller: "leftx", "lefty", "rightx", "a", "b", "start"...
input.key(name) -> bool     -- the PC keyboard, for testing

-- everything else
save.get(key, default)      save.set(key, value)
sys.time()                  sys.real_time()          sys.time_scale([scale])   -- 0 stops the game, 0.3 is slow motion
sys.step()                  sys.hud(bool)            sys.stats() -> fps, draws, vertices, sounds      sys.platform() -> "ios" | "pc"
include("other.lua")        print(...)   -- prints show in the dev.py window, from the phone too
```

`lib.lua` is plain Lua on top of all that, and yours to edit: quaternion and vector helpers, `lib.run`/`lib.wait` scripts, timers, events, state machines, an animation player with cross-fades and timed events, shuffled sound cues, a particle emitter, splines, a table of translated text, and `lib.strict()` to turn misspelt globals into errors. Its comments say how each is used.

A game may have a `game.cfg` with a line `orientation = landscape` (or `portrait`) to stop the phone turning it.

2D drawing uses a screen whose shorter side is always 720 units, so layouts hold up in both orientations; `gfx.size()` gives the current width and height. On the PC, WASD or the arrow keys drive the first stick, and each button's `key` presses it.

**On the PC:** **F3** shows frame time, draw calls and memory. **F5** restarts the game. Press **F1** to step through iPhone shapes from the 11 to the 17 Pro Max, and **F2** to turn the phone. The window takes that phone's shape and the rounded corners, notch or Dynamic Island and home bar are drawn over the game, so you can see what a real phone would hide. `$env:GAMELAB_DEVICE = '9'` before `run` starts with the ninth phone in the list (the list is at the top of the phone-frame code in `engine/main.cpp`).

**On the phone:** type `lua <code>` at the `dev.py` prompt to run a line of Lua in the running game, for example `lua sys.time_scale(0.2)` or `lua player.speed = 9`. The app also keeps a log file; if it stops unexpectedly, the last lines are sent to `dev.py` the next time it connects.

## Shaders

Put `name.slang` in a game's `shaders/` folder and select it with `gfx.shader("name")`. Start from `games/shader-demo/shaders/pulse.slang`. Every shader begins with `#include "common.slang"`, which lives in `engine/shaders/` and lists everything a shader can read (camera, lights, the four texture slots, your `gfx.param` numbers) along with the standard lighting as functions to call. `dev.py` compiles it to SPIR-V when you save, and MoltenVK converts that to Metal Shading Language on the iPhone. The compiled `.spv` files are committed, so the GitHub build needs no shader compiler.

## Updating the app on the phone

Builds are never published. GitHub keeps each one as a private build file, `ship` fetches it to `tools/.ipa/` with your own login, and `dev.py` offers it to the phone over your Wi-Fi. When the PC holds a newer build than the app, the game list shows **DOWNLOAD UPDATE**, which saves the file through Safari. Then open LiveContainer, tap **+** and pick `GameLab.ipa`. (LiveContainer can't install an app while one is running inside it, so the app can't do this step for you.)

## Releasing one game

`python tools/dev.py release runner` builds `runner` as its own app and saves it to `tools/.ipa/runner.ipa`: only that game inside, no game list, no Wi-Fi syncing. The result is unsigned, which suits SideStore. An App Store upload also needs signing with an Apple Developer Program account.

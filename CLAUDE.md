# Game Lab: handoff notes

This file briefs an assistant (or a future you) picking up this repo. It records what exists, what has and hasn't been tested, the constraints that shaped the design, and what to build next. `README.md` has the day-to-day commands and the Lua API.

## The person and the goal

- Solo developer on **Windows** with VS Code and PowerShell. **No Mac.** Has `git`, `gh` (logged in, with the `workflow` scope) and Python 3.14.
- Wants to make several **3D games for iPhone** in one monorepo, test them on a PC and on a real phone, and release one to the **App Store in about a month** (from mid-November 2026).
- Firm preferences: **native C++**, **Vulkan with MoltenVK**, **Lua** for gameplay, **Slang** for shaders. **No JavaScript or web technology.** An earlier web-view scaffold was thrown out for this reason; don't reintroduce it.
- Comfortable running commands but not an experienced build engineer. Give exact commands for PowerShell, say what each one does, and explain errors plainly. Has some Vulkan and Slang knowledge.

## The phone setup

- iPhone on **iOS 18.7.8**. Its screen is the **iPhone 11 / XR shape**: the app reported 414 x 896 points, safe area 48 points at the top and 34 at the bottom when upright. That is frame 1 in the PC build (`GAMELAB_DEVICE=1u` upright, `1` sideways), so that frame is the one to judge layouts in. Apps are sideloaded with **SideStore running inside LiveContainer** (the "LiveContainer + SideStore" nightly bundle, installed with iloader). **LocalDevVPN** must be connected to install or refresh.
- Uses a **spare Apple ID** on a **free account**: 3 sideloaded apps, 7-day expiry (refresh in SideStore), 10 new app IDs per week. Apps installed *inside* LiveContainer don't use a slot.
- SideStore sign-in error -80045 was fixed by using the nightly build, turning off on-device anisette and picking another anisette server. If sign-in breaks again, start there.
- GitHub repo: `Hassleware/myapp-ios`, currently **public**. Local folder: `...\iphone\myapp-starter`.

## Hard constraints (don't design against these)

1. **iOS will not run native code that arrives after install.** C++ changes always need a rebuilt app. Only data can be sent over Wi-Fi: Lua, compiled shaders, levels, assets. This is why gameplay is in Lua.
2. **There is no iPhone simulator on Windows.** The PC build of the same engine is the preview. The phone is for touch, performance and feel.
3. **iPhone builds happen on GitHub's macOS runners.** A build takes roughly 5 to 15 minutes, most of it SDL3's configure step under the Xcode generator.
4. **An app can't replace itself.** The in-app update button can only hand the IPA to SideStore or Safari.
5. **App Store rules:** a released app must not download code. Release builds contain one game's files and no sync code. Lua shipped inside the app is fine.

## Architecture

```
engine/main.cpp     SDL3 app callbacks, game list, game lifecycle, hot reload, log file, HUD, update buttons
engine/gfx.cpp      Vulkan 1.2 renderer (the only renderer; MoltenVK runs it on Metal): meshes, textures, fonts, lights, shadows, targets
engine/model.cpp    glTF loading (cgltf), node lists, skeletons, animation sampling and blending, CPU skinning
engine/collide.cpp  collision shapes, ray and sphere casts, overlaps, the character mover. No physics solver, by choice
engine/audio.cpp    mixer on an SDL audio stream: clips, streamed .ogg (stb_vorbis), 3D, groups
engine/script.cpp   Lua 5.4 host and the gfx / model / phys / audio / input / save / sys API given to games
engine/input.cpp    touch sticks and buttons, raw touches, tilt, gamepad, with keyboard and mouse stand-ins
engine/pool.h       numbered handles with a generation check; everything Lua holds is one of these
engine/sync.cpp     loader only: finds the PC, downloads changed game files (POSIX sockets)
engine/shaders/     common.slang (shared by every shader), mesh.slang, shadow.slang and their compiled .spv files
games/<name>/       main.lua, lib.lua, optional game.cfg and shaders/*.slang, models, textures, sounds
games/features/     a tour of every engine feature; its binary files come from tools/make_demo_assets.py
ios/Info.plist.in   iPhone app settings
ios/haptics.mm      the iPhone's tap motor (UIKit), the only Objective-C in the project
tools/dev.py        the one dev tool: watch, serve, run, ship, status, release, doctor, check
tools/template/     what `new <name>` copies: main.lua and lib.lua (the Lua-side helper library)
.github/workflows/build.yml   iPhone build
```

Three builds come from the same sources:

| Build | Made by | Contains |
|---|---|---|
| PC | `python tools/dev.py run <game>` | reads `games/` straight from the repo, hot reloads on save |
| Loader ("Game Lab") | `ship`, on push to `main` when native code changes | every game bundled, plus Wi-Fi sync (`LOADER_SYNC`) |
| Standalone | `release <game>` (workflow_dispatch with `game=`) | one game (`SINGLE_GAME`), no game list, no sync |

Dependencies are fetched by CMake `FetchContent`: SDL3 `release-3.4.20`, Lua `v5.4.8` (built from `onelua.c`), Vulkan-Headers and volk `vulkan-sdk-1.4.363.0`, stb (image, truetype, vorbis; pinned commit) and cgltf `v1.15`. MoltenVK `v1.4.2` is downloaded by the workflow as a static library.

### How the pieces talk

- `dev.py` serves plain text over HTTP on port **8765**:
  - `GET /api/hello` returns `gamelab`
  - `GET /api/state?build=<fingerprint>` returns five lines: games revision, command id, action, game, fingerprint of the app build held on the PC
  - `GET /api/manifest` returns `game<TAB>path<TAB>sha1` per line
  - `GET /files/<game>/<path>` and `GET /app/GameLab.ipa`
  - `POST /api/log` with `level<TAB>game<TAB>message`
- The loader finds the PC by trying every address on its own /24 network, then remembers it in `Documents/server.txt` (visible in the Files app, editable by hand). It polls state once a second, syncs changed files into `Documents/games/`, and synced games override bundled ones.
- **Native fingerprint:** `dev.py fingerprint` hashes `engine/`, `ios/` and `CMakeLists.txt` (line endings normalised, `.slang` excluded). The workflow bakes it into the app as `BUILD_FINGERPRINT`. The app reports it on every poll, so `dev.py` can warn when the phone runs older C++ than the repo.
- **Hot reload:** `main.cpp` hashes the running game's folder (names, sizes, modified times) every 400 ms and restarts the game's Lua state when it changes. This one mechanism serves both the PC and the phone.

### Rendering details worth knowing

- One vertex format everywhere: position, normal, colour, texture coordinate, tangent (64 bytes). Descriptor set 0 holds the per-frame uniform block and the shadow map; set 1 holds a material's four textures. The push constant block is 128 bytes (`model`, `color`, `flags`, two `params`). All of it is declared once in `engine/shaders/common.slang`, which **every shader includes**; `FrameData` and `PushData` in `gfx.cpp` must match it.
- Drawing is recorded during `draw()` and replayed in `end_frame`: the shadow pass, then each texture target, then the screen (sky, solid things, see-through things sorted far to near, 2D). Pipelines are made on first use per shader and state combination.
- Colours given by games are sRGB and are converted to linear on the CPU; textures and the swapchain are sRGB formats, so lighting is done in linear light.
- The screen is 4x MSAA where available (`GAMELAB_NO_MSAA=1` turns it off on the PC). Texture targets and the 2048 shadow map are not multisampled.
- Skinned meshes are moved on the CPU into the per-frame vertex buffer, so any shader works on characters. That is fine for a handful; crowds would need GPU skinning.
- Triangles wind counter-clockwise seen from outside (as glTF does) and back faces are culled; `gfx.cull(false)` for two-sided things.
- Files loaded from a game's folder are kept across a hot reload unless their modified time changed; everything is freed when the game changes.
- Matrices are column-major in C++ and shaders are compiled with `-matrix-layout-column-major`, so Slang code writes `mul(matrix, vector)`.
- Slang emits **SPIR-V 1.5**, which is why the instance asks for **Vulkan 1.2**.
- Shaders are compiled to `<name>.vert.spv` and `<name>.frag.spv` (entry points `vsMain`, `fsMain`). The `.spv` files are **committed**, so the GitHub build needs no shader compiler. MoltenVK turns SPIR-V into Metal Shading Language at run time.
- 2D drawing uses a virtual screen whose **shorter side is 720 units**. Text uses a built-in 8x8 bitmap font drawn as rectangles, or a `.ttf` chosen with `gfx.font`, whose letters are drawn into a 1024 atlas the first time each is used (so any language the font covers works).
- Desktop loads Vulkan through **volk**. The iPhone build links MoltenVK statically and must **not** use volk, because volk's function-pointer names collide with MoltenVK's real functions. `gfx.cpp` switches on `USE_VOLK`.
- Simplifications to revisit (each is marked `ponytail:` in the code): one frame in flight; frees wait for the GPU; one draw call per mesh (no instancing); textures are uncompressed RGBA with CPU-made mipmaps (no ASTC); assets load on the main thread (`lib.loader` spreads them over frames); capsules collide as a column of spheres and never tilt; sphere casts step along the line rather than solving exactly; the swapchain is rebuilt whenever the window's pixel size changes.

### Small traps already hit

- The sync namespace is `devsync`, because `sync` collides with a POSIX function.
- Lua on iOS needs `LUA_USE_IOS` (there is no `system()`). Games get only the base, table, string, math, utf8 and coroutine libraries; files are read with `include("file.lua")`.
- Windows prints a `ConnectionResetError` traceback when the phone's network scan hangs up. `QuietServer` in `dev.py` suppresses that.
- The workflow only runs on pushes that touch `engine/`, `ios/`, `CMakeLists.txt` or the workflow itself. Game-only commits don't build an app, by design.
- Bundle IDs: loader is `com.starter.gamelab`; standalone builds default to `com.starter.<game>`. **Never change a game's ID after release.** Pick real ones (for example `com.<yourname>.<game>`) before the first App Store upload.

## What has been tested

| Area | State |
|---|---|
| Engine on Linux with a software Vulkan driver: both demo games, custom shader, controls, game list, portrait and landscape | Tested |
| Loader sync against `dev.py` on Linux: discovery, download, live edits, remote `play`, Lua errors sent to the PC, update notice | Tested |
| **iPhone build of the first version**: compiled on GitHub first try, ran on the phone, rendered through MoltenVK, touch worked, found the PC by itself | Tested on device |
| Windows PC build (`run <game>`) | Tested: builds with MSVC and runs every game |
| The big engine build of November 2026 (textures, glTF, skinning, lights, shadows, targets, fonts, collision, audio, raw touches) | Tested on the PC with Vulkan validation clean, by screenshots and by a Lua self-test of collision, animation and audio numbers. **Untested on the phone** when this was written: the first iPhone build, MoltenVK's handling of the new passes, sound output, tilt, haptics (`ios/haptics.mm` has never been compiled on the PC), gamepad, the log file, `lua` from `dev.py`, `game.cfg` orientation |
| Private build flow: artifact upload, `gh run download`, `/app/GameLab.ipa` served to the phone | Tested |
| Updating the app on the phone | Game Lab is installed **inside LiveContainer**. `sidestore://install` failed (bundle mismatch) and `livecontainer://install` failed ("LiveContainer must refresh to install apps": it won't install while an app runs inside it). The working route, **tested**: DOWNLOAD UPDATE (Safari saves the IPA), then **+** in LiveContainer. Don't retry in-app install links |
| Slang auto-download in `dev.py` (guessed release file name) | **Untested.** The Vulkan SDK's `slangc` is the reliable source |
| `release <game>` standalone build | **Untested** |
| App Store signing and upload | **Not built** |

When something from the untested rows fails, that is expected; fix it rather than working around it.

## Debugging aids

- `python tools/dev.py status` and `doctor`.
- **F3** (or `sys.hud(true)`) shows frame time, draw calls, vertices, textures, meshes, Lua memory and sounds playing. `phys.debug()` draws collision shapes.
- `lua <code>` at the `dev.py` prompt runs Lua in the game on the phone (loader only; it travels as a command, like `play`).
- The app writes `log.txt` in its preferences folder and a `running` marker; if the marker is still there at the next start, the old log's tail is sent to `dev.py` as an error.
- Phone-side `print()` output and Lua errors appear in the `dev.py` window. Errors also show on the phone's screen.
- `GAMELAB_WINDOW=414x896` opens the PC build in a phone shape.
- **F1** in the PC window steps through iPhone frames (11 to 17 Pro Max): the window takes the phone's shape and its rounded corners, notch or Dynamic Island and home bar are drawn on top. The phone's black body is drawn round the screen too (`gfx::set_bezel`), so the window has the whole phone's proportions. **F2** turns it. `GAMELAB_DEVICE=9` starts with the ninth. PC only (`draw_phone_frame` in `main.cpp`); sizes are approximate. Games and the game list keep clear with `gfx.safe()` (left, top, right, bottom insets in 2D units): real values from `SDL_GetWindowSafeArea` on the phone (**untested on device**), simulated from the frame on the PC, zero with no frame.
- **`python tools/dev.py check <game>`** (or `gamelab --check <game>`) runs the game in all 12 frames, sideways and upright, and prints every text, rect, circle or touch control that a corner, notch, island or home bar covers. **Run it after any change to a game's 2D drawing or controls.** It works by logging the boxes of 2D draws (`gfx::ui_log`), ignores anything over 60% of the screen as background, and only sees what the game draws in its first moments, so later screens (menus, game over) need a way to be reached before they can be checked.
- `gamelab --phone <folder>` makes a desktop build behave like the loader: it ignores the repo and syncs into that folder (non-Windows only, since sync uses POSIX sockets).
- Configure with `-DGFX_SCREENSHOT=ON`, then `GAMELAB_SHOT=out.bmp` saves a frame and quits (`GAMELAB_SHOT_FRAME=n` picks the frame). Useful for headless checks.
- Failed iPhone build: `gh run view <id> --log-failed`.
- Vulkan validation on the PC: install the Vulkan SDK and set `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`.

## How to develop

**Daily loop:** keep `python tools/dev.py` running. Edit Lua or `.slang` files and save; both the PC window and the phone reload. The tool prints `[live]` or `[native]` for each change and says what to do.

**Adding an engine feature** (the usual shape of C++ work):

1. Implement it in C++ (`gfx.cpp` or a new file added to `ENGINE_SOURCES` in `CMakeLists.txt`).
2. Expose it to Lua in `script.cpp`: write an `l_name` function and add it to the right `luaL_Reg` table.
3. Use it from a game's `main.lua` and test with `run <game>` on the PC.
4. Add it to the API list in `README.md`.
5. `ship "what changed"`, then update the phone app.

Keep the rule that **games ask the engine for things by name and never know where files live**. That is what lets the same game run from the repo, from synced files and from inside a release app. Keep sync code confined to `sync.cpp` and `LOADER_SYNC` blocks.

**Prefer data over code.** Anything expressed in Lua or data files iterates in a second on the phone. Anything in C++ costs a GitHub build.

## What to build next, in order

Toward a shippable game within the month:

1. **Confirm the Windows build** with `run runner` and fix any MSVC errors.
2. **Confirm the private update flow** end to end on the phone; adjust the install link if SideStore inside LiveContainer doesn't accept it.
3. **Speed up iPhone builds:** cache `build-ios/_deps` with `actions/cache`, or try the Ninja generator for iOS.
4. **Done in the November 2026 engine build:** textures, glTF models and skinned animation, audio, collision, lights and shadows, fonts, raw touches, tilt, haptics, gamepad, orientation lock, culling. The user chose to write any physics simulation themselves in Lua on top of the collision queries, so **do not add Jolt or another physics library**. The aim of that build was that games need no further C++ for a long time; check `README.md` and `games/features/` before adding to the engine.
5. **Get that build onto the phone and fix what MoltenVK or iOS reject** (see the untested row above).
6. **Make the game.** Blender for levels and characters (export `.glb`), Lua for everything else.
7. **Phone polish left over:** scrolling on the game list.
8. **App Store path:** join the Apple Developer Program early (about $99 a year, enrolment can take days). Then add app icons and a launch screen, real bundle IDs, a signing and upload workflow using an App Store Connect API key, and the store listing (screenshots, privacy answers, age rating). A paid account also removes the 3-app and 7-day sideload limits.
9. Consider making the repo private before release: `gh repo edit --visibility private --accept-visibility-change-consequences`. Private repos have a monthly Actions allowance and macOS minutes count heavily against it, so check GitHub's billing page first.

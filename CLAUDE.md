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
engine/input.cpp    touch sticks and buttons, raw touches, tilt, gamepad, the on-screen keyboard and clipboard
engine/net.cpp      HTTPS requests for games (WinHTTP on the PC, ios/http.mm on the phone) and every safety check on them
engine/pool.h       numbered handles with a generation check; everything Lua holds is one of these
engine/sync.cpp     loader only: finds the PC, downloads changed game files (POSIX sockets)
engine/shaders/     common.slang (shared by every shader), mesh.slang, shadow.slang and their compiled .spv files
games/<name>/       main.lua, lib.lua, optional game.cfg and shaders/*.slang, models, textures, sounds
games/net-demo/     the only game left in the folder: keyboard, paste and one web call. The older games (features,
                    render-demo, shader-demo, runner) were deleted on request; they are in git history up to 261eb48
ios/Info.plist.in   iPhone app settings
ios/haptics.mm      the iPhone's tap motor (UIKit)
ios/http.mm         HTTPS through NSURLSession
ios/bodytrack.mm    ARKit body tracking for motion capture (declared for C++ in engine/mocap.h, stubbed off-iPhone)
tools/dev.py        the one dev tool: watch, serve, run, ship, status, release, doctor, check, docs
docs/               the handbook (MkDocs + Material): onboarding, a tutorial per language, engine tour, reference
docs/hooks.py       pulls real code into pages by name and checks the Lua API description against script.cpp
docs/api/gamelab.lua  every Lua function described once: feeds the handbook, the drift check and VS Code (.luarc.json)
mkdocs.yml          the handbook's settings and menu
tools/template/     what `new <name>` copies: an empty main.lua with every callback, lib.lua, a commented game.cfg
tools/backend_template.py   what `backend <game>` copies to backends/<game>/server.py: accounts and emailed-code sign-in
tools/backend_client.lua    what `backend <game>` copies to games/<game>/auth.lua: the game's side of sign-in
backends/<game>/    each game's own server code (Python). Runs inside dev.py on the PC; never synced or bundled
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

### Keyboard and web calls (added 14 November 2026)

- `input.keyboard(show, kind)` uses SDL's text input. Typed text is queued in `input.cpp` and handed to the game's `on_text` / `on_key` at the start of `script::update`. Kind `"code"` maps to SDL's number-password type, which SDL marks as a one-time code on iOS so the SMS code is offered above the keyboard.
- `net.request` runs each request on its own thread (`net.cpp`); answers are collected in `script::update` and passed to the Lua callback. Answers for a game that has been reloaded are dropped.
- **Security rules, all in `net.cpp`, do not loosen them:** https only; the host must be listed in the game's `game.cfg` (`hosts = ...`); URLs with a user name, backslash, space or non-ASCII character are refused, so the engine and the platform can never disagree about the host; header line breaks refused; redirects not followed; no cookies, cache or stored credentials; size and count caps. Certificate checking is the platform default and is never turned off. `GAMELAB_NET_CHECK=1` runs a self-check of these rules at the first game start and prints the result.
- Left out on purpose, add when needed: response headers, a Keychain-backed `secret` store for login tokens (`save` is a plain file), certificate pinning, uploads and downloads to files, WebSockets, HTTPS on the Linux build, moving the view up when the keyboard covers a field.
- The Wi-Fi sync between `dev.py` and the loader is plain HTTP with no login. That is acceptable on a home network for development only, and is one more reason release builds contain no sync code.

### Game backends (added 15 November 2026)

- `net.backend({path=...}, cb)` goes to the game's own backend; the game never names a host. In dev builds `net.cpp` points it at `dev.py` (`/backend/<game>/<path>`): on the PC over WinHTTP to 127.0.0.1:8765, in the loader through `devsync::backend`, which uses sync's own POSIX socket client, so **iOS needs no ATS plain-http exception**. In `SINGLE_GAME` builds it is `game.cfg`'s `backend = https://...` plus the path, through the normal HTTPS rules (that host is allowed automatically).
- `dev.py` loads `backends/<game>/server.py` with importlib, reloads it when its modified time changes, and calls `handle(method, path, query, body, headers)` under one lock. Errors print with a traceback and return 500. The file also runs on its own (`python server.py 8080`) for a real deployment, which is **untested**.
- The backend template is **accounts and sign-in only** (the user's choice: the one thing every game and app needs), **passwordless like Supercell ID**: `POST /code` emails a 6-digit code, `POST /verify` swaps it for a session token. Codes: `secrets.randbelow`, single use, 10 minutes, 5 tries, one per email per minute, stored hashed. Sessions: 256-bit tokens stored as SHA-256, a year, pushed forward on each use. `DELETE /me` for the App Store's account-deletion rule. With no `GAMELAB_SMTP` set the code is printed in the `dev.py` window; with it, sent by SMTP with STARTTLS (**untested**). A 15-check test of the flow passed on the PC. Missing on purpose: Sign in with Apple, a daily email cap, refresh tokens, Keychain storage of the token on the phone (`auth.lua` uses `save`).
- `net.request` still refuses all plain `http://`. Only the engine-built backend address may be plain http, and only in dev builds.
- **Untested on the phone**: the loader path through `devsync::backend` has only been compiled by the GitHub build. Tested on the PC: `dev.py` routing (store, list, 404, a backend error) and the engine reaching `127.0.0.1:8765` over plain http.

### Motion capture (added 15 November 2026)

- `games/mocap` records a take with ARKit body tracking (`ios/bodytrack.mm`), sends the raw take with `net.backend` (backend uploads may be up to 64 MB; `net.request` stays at 1 MB), and `backends/mocap/server.py` writes `captures/<name>.bvh` (git-ignored). The take format: a header of joint name, parent and local offset (measured at the first recorded frame), then per frame the time, the body's room position and turn, and every joint's local rotation quaternion. BVH channels are ZXY Euler; the converter resamples to 60 fps by nearest frame. `NSCameraUsageDescription` is in the loader's Info.plist keys.
- The screen shows the camera picture at 20% behind the skeleton: `bodytrack.mm` turns each ARKit frame upright and shrinks it to 270 x 360 RGBA on the CPU, and projects each joint onto it with `ARCamera projectPoint` (portrait). `mocap.camera()` keeps one texture and refreshes it with `gfx::update_texture` (new: re-uploads pixels into a no-mip texture after `vkQueueWaitIdle`). `FLIP_X` / `FLIP_Y` in the game fix a mirrored or upside-down picture without a build. `GAMELAB_FAKE_CAMERA=1` gives the PC a moving test picture.
- **Untested on the phone.** Tested on the PC: the screen, recording flow and sending with a stand-in skeleton, a real `.bvh` written through `dev.py`, and the quaternion-to-Euler round trip. Biggest unknown: camera permission inside LiveContainer.

### GVHMR on the PC (added 15 November 2026)

- The mocap game also records an H.264 video (`AVAssetWriter` in `bodytrack.mm`, 30 fps, `mocap.video()`), uploads it with `net.backend` (backend uploads up to 256 MB), and `backends/mocap/server.py` runs `tools/gvhmr/run_take.py` in **WSL Ubuntu** (`~/GVHMR`, conda env `gvhmr`, PyTorch 2.10 + PyTorch3D cuda130 build: the only recipe that runs on the RTX 5080, from GVHMR issue #88). Jobs are tracked in `captures/<name>.job.json` so a backend reload does not lose them. The phone polls `/job` and fetches `/result` (22 SMPL-X joint positions per frame) to replay.
- `dev.py` now passes a backend upload as `bytes` when it isn't UTF-8 text (it used to decode everything, which corrupted video).
- Weights: GVHMR's Google Drive hit its quota, so gvhmr/hmr2/vitpose came from the Hugging Face mirror `ryanrudes/gvhmr` after `tools/gvhmr/scan_checkpoint.py` (static pickle scan; verified to refuse an `os.system` trap) passed them; YOLO is Ultralytics' official v8.3.0 release.
- **Status:** tested on the PC up to GVHMR's model: ffmpeg, WSL launch, YOLO, ViTPose-H and features all ran on the RTX 5080 with GVHMR's sample video. It stops at `SMPLX_NEUTRAL.npz`, which the user must download (licence-gated) into `tools/.bin/body_models/smplx/`. Untested: the BVH export, the phone's video recording, upload, and replay. Non-commercial licences (GVHMR weights, SMPL-X).

### SAM 3D Body on the PC (added 15 November 2026, replaces GVHMR in the mocap backend)

- The user didn't want the SMPL-X registration GVHMR needs, so the mocap backend runs **SAM 3D Body** (Meta, arXiv 2602.15989, MHR body rig, 127 joints with hands and feet) instead: `tools/sam3d/install.sh` (WSL, conda env `sam3d`, `~/sam-3d-body`, PyTorch 2.10), `tools/sam3d/run_take.py` (YOLO box per frame, iPhone 11 26 mm lens as intrinsics, 5-frame moving average, writes `captures/<name>-sam3d.bvh` and a half-rate `-sam3d.txt` for the phone). Detectron2 and MoGe are skipped on purpose.
- Weights: Meta's Hugging Face repos are gated, so they came from `jetjodh/sam-3d-body-dinov3`, after checking `model.ckpt` and `mhr_model.pt` are byte-identical (Hugging Face LFS SHA-256) across 5 independent re-uploads and scanning `model.ckpt` with `tools/gvhmr/scan_checkpoint.py`. The SAM License allows redistribution with the licence attached; it is kept with the files. `mhr_model.pt` is TorchScript.
- **Coordinate trap:** `pred_joint_coords` (+ `pred_cam_t`) are in camera space (y down) but `pred_global_rots` are in the body model's y-up frame. Flip the positions, leave the rotations. `tools/sam3d/check_bvh.py` rebuilds positions from a BVH: 11.5 mm mean error on the sample clip (worst 82 mm at fingertips: MHR bone stretch).
- Tested on the PC with GVHMR's sample video through `server.run_job`. Untested from the phone (needs app build 057b3b19, already built). About 40 s of model loading per take, then roughly 3 frames a second.
- GVHMR stays installed (`tools/gvhmr`); it only lacks `SMPLX_NEUTRAL.npz`.
- Speed on the RTX 5080: about 10 s to load the model, YOLO 15 ms a frame, SAM 3D Body 166 ms a frame body-only (the default) or 664 ms with the hand pass (the phone's HANDS button sends `&hands=1`). bfloat16 autocast fails (a sparse op), so it runs in float32.
- `FOOT_LOCK` in `run_take.py` shifts the root while a foot (`l_ball` / `r_ball`) is planted: sliding 140 to 93 mm/s on the sample clip, joint rotations untouched.
- The backend keeps one **worker** (`run_take.py serve`) with the model loaded: jobs go one a line on its stdin, it answers with STAGE lines and `JOB DONE` / `JOB FAILED`, and quits after 10 idle minutes or when `dev.py` closes. Kept on `sys` so a reload of `server.py` reuses it. Second take of the sample clip: 17 s against 43 s cold.
- Defaults in `run_take.py`: `SIMPLE` (81 bones: drops 40 MHR twist helpers, tongue, teeth), `FOOT_LOCK`, `SMOOTH = 5` moving average. A One Euro filter was tried and measured worse at every setting (more shake at equal fidelity), so it was removed; each take prints its jitter numbers for tuning.
- **Wooden mannequin** (`tools/blender_mannequin.py`, 16 November 2026): builds a generic artist's mannequin (every part turned on a lathe from a profile: egg head, ribcage chest, pelvis, tapered limbs, paddle hands with thumbs, ball joints, domed slotted screws; every vertex weighted 1.0 to one bone; beech grain and normal map generated with numpy as tileable 1024 images and UV-mapped at real size, TILE = 0.12 m per copy, so grain density is the same on every part) on a 19-bone humanoid rig in a T-pose, retargets a take's `-sam3d.npz` (world rotations saved by `run_take.py`) onto it and exports `games/mannequin/mannequin.glb` with the clip named after the take. Retargeting: each bone follows one take joint's world rotation times a fixed correction found on frame 1 by swinging the bone to the take's direction, so neither skeleton's rest pose matters; hips travel scaled to the mannequin's 1.0 m hip height. Tested: rendered in Blender 4.4 and played in the engine (`games/mannequin`) on the PC.
- **Foot IK in Lua** (`games/mannequin/main.lua`): after `anim:update`, rays from above each ankle find the ground (`phys`), a foot the animation has low and still is planted (eased down and held in place), the hips drop when a foot can't reach, and two-bone IK turns thigh and shin through `model.joint` / `model["local"]`. Measured over the take: planted ankles 36.6 mm off the ground to 0.2 mm, sliding 160 to 55 mm/s, samples inside the floor or step 724 to 2. Planted feet also tilt flat to the ground normal from the raycast (sole up found from the T-pose): toes 44 mm off the ground to 1.6 mm. **`model.local` is not valid Lua** (`local` is a keyword): it is `model["local"]`, fixed in the docs too.
- The mannequin game now also has **suspension** (the body rides on the legs like a car on springs, `SPRING_HZ` 4, `SPRING_DAMP` 0.6, toward the height where its lowest sole corner rests on the ground and every foot can reach; hips jolts max 133 to 23 m/s²; planted feet slide after the animation at `SLIDE`; the IK foot correction is capped at 45° and foot turn and knee direction ease at `JOINT_EASE`, so feet never spin: max foot turn per frame 52° to 12°) and **rolling feet**: heel and toes are separate contact points, each planted and held when low and still; the toes may not leave until the heel has risen `HEEL_UP` (heel first, toes last), a heel on the ground always has its toes down too (toes down with the heel up is allowed), and the sole is levelled side to side when both are down. Before gravity, every bone follows the animation on a bouncy spring (`BOUNCE_HZ` 2, `BOUNCE_DAMP` 0.45) for cartoon smoothing: hand jitter 0.70 to 0.48 mm/frame². Measured over take-1: lift-offs 10 of 10 heel first, landings 11 of 12 heel first, planted points within 1 mm of the ground.
- **Taped bats** (`tools/blender_bats.py` -> `games/mannequin/bat.glb`): measures the left hand of `assets/mannequin/mannequin.blend`, builds a table tennis bat (handle along the palm, blade past the fingertips, red and black rubber) and 5 wraps of 48 mm duct tape with a generated woven-silver texture: 3 shrink-wrapped onto hand and handle (Shrinkwrap, Smooth, Shrinkwrap, Solidify modifiers, applied by the exporter), 2 in the middle 2.5 mm proud and crumpled. The handle is on the palm side (+y in the T-pose); bat.glb holds bat.L and a mirrored bat.R, each drawn at its hand bone with its turn relative to the bone found once from the T-pose. The mannequin file is untouched.
- **Shots takes for Dream Table**: while recording, a voice (Windows SAPI words in `games/mocap/*.wav`) says "three, two, one, go", then "ready" 1.2 s before each "hit!" every 4 s (first at 4 s; `FIRST_HIT`, `PERIOD` in `games/mocap/main.lua` and `tools/split_shots.py` must match), the user plays a different shot on each "hit", and takes are named `shots-<n>`. Hands free: both hands held over the head (two tracked joints over 0.8 m above the hips, 0.25 m apart) for 1 s presses RECORD or STOP. The HANDS button is gone (the PC runs body-only). After SAM 3D Body, `server.py` runs `tools/split_shots.py`, which cuts `captures/shots-<n>-sam3d.npz` into one clip per beep (half a period either side, the unfinished last one dropped) named `shots-<n>-<k>-<guess>-sam3d.npz`, guessing the shot from the right wrist (side, height, rise, speed). The guess is rough; check it. **Untested with a real shots take.**
- `tools/blender_import_take.py` imports the newest take into Blender with the right axes, scale and frame rate. Tested headless in Blender 4.4: 127 bones, feet on the floor, positions matching the PC's to a centimetre.

### The handbook (added 14 November 2026)

- `python tools/dev.py docs` serves it on port 8766 with live rebuild; `docs` at the prompt does the same in the background. It needs `pip install mkdocs-material`, the one non-standard-library dependency in the repo, and only for the handbook.
- Pages never paste engine code. A line `::: source <file> <name>` is replaced at build time with that function, struct or constant as it is now, with real line numbers. A name that no longer exists **stops the build**, so renaming something in the engine means fixing the page that explains it.
- **When adding or removing a Lua function, edit `docs/api/gamelab.lua` too.** The build warns by name about any function registered in `script.cpp` but not described, and the reverse. `python -m mkdocs build --strict` turns that into a failure.
- Nothing under `docs/`, `mkdocs.yml` or `.luarc.json` is in the native fingerprint, so docs changes never ask for an app build.
- Material for MkDocs has announced end of life on 5 May 2027 (its makers' successor is Zensical, which reads the same `mkdocs.yml`). The hook uses only MkDocs' standard `hooks:` interface; check it still runs before migrating.

### Small traps already hit

- **`tests/sync_test.cpp` runs on every GitHub build, before the app is built**: it compiles `sync.cpp` on the Mac and checks it against a real `dev.py` (connect, download byte for byte, remove a deleted game, backend call, status pass-through, size limit). Added on 15 November 2026 after build c4c6d822 shipped a size check that overflowed and silently broke all sync (fixed in 6d901abd; that bug also meant the phone could not see DOWNLOAD UPDATE, so the fix had to be fetched in Safari from `http://<pc>:8765/app/GameLab.ipa`). The test then caught a second bug (part of a reply dropped), fixed in bd5c393c. Extend it whenever `sync.cpp` changes.
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
| Keyboard, clipboard and `net.request` (14 November 2026) | Tested on the PC: typing, backspace, a real HTTPS call, a 404, and refusals of plain http and unlisted hosts. **Untested on the phone**: `ios/http.mm` has never been compiled on the PC, the on-screen keyboard, the SMS code suggestion, the paste permission prompt |
| Game backends (15 November 2026) | Tested on the PC: `dev.py` routing and the engine's plain-http call to it. **Untested**: the loader's path to the backend on the phone, and a standalone release with `backend = https://...` |
| Motion capture (15-16 November 2026) | **Tested on device (16 November, build 057b3b19)**: camera permission inside LiveContainer, ARKit tracking, recording, the video upload, SAM 3D Body on the PC and the replay on the phone all worked end to end. Not yet done with a real take: importing it into Blender and retargeting it onto a character |
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

1. **Confirm the Windows build** (done: `run <game>` builds with MSVC).
2. **Confirm the private update flow** end to end on the phone; adjust the install link if SideStore inside LiveContainer doesn't accept it.
3. **Speed up iPhone builds:** cache `build-ios/_deps` with `actions/cache`, or try the Ninja generator for iOS.
4. **Done in the November 2026 engine build:** textures, glTF models and skinned animation, audio, collision, lights and shadows, fonts, raw touches, tilt, haptics, gamepad, orientation lock, culling. The user chose to write any physics simulation themselves in Lua on top of the collision queries, so **do not add Jolt or another physics library**. The aim of that build was that games need no further C++ for a long time; check `README.md` before adding to the engine.
5. **Get that build onto the phone and fix what MoltenVK or iOS reject** (see the untested row above).
6. **Make the game.** Blender for levels and characters (export `.glb`), Lua for everything else.
7. **Phone polish left over:** scrolling on the game list.
8. **App Store path:** join the Apple Developer Program early (about $99 a year, enrolment can take days). Then add app icons and a launch screen, real bundle IDs, a signing and upload workflow using an App Store Connect API key, and the store listing (screenshots, privacy answers, age rating). A paid account also removes the 3-app and 7-day sideload limits.
9. Consider making the repo private before release: `gh repo edit --visibility private --accept-visibility-change-consequences`. Private repos have a monthly Actions allowance and macOS minutes count heavily against it, so check GitHub's billing page first.

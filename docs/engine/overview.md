# The big picture

The engine is a small C++ program built on three libraries: **SDL3** for the window, input and sound output, **Vulkan** for drawing, and **Lua** for running games. It is designed around one constraint: compiled code cannot be updated on an iPhone after install, so the engine is kept general and games are data.

## The map

| File | Namespace | Responsible for |
|---|---|---|
| `engine/main.cpp` | (none) | The app itself: start-up, the frame loop, the game list, hot reload, the log file, the PC-only phone frames |
| `engine/gfx.cpp` | `gfx` | All drawing. The only file that talks to Vulkan |
| `engine/model.cpp` | `model` | Loading `.glb` files, skeletons, animation |
| `engine/phys.cpp` | `phys` | Collision shapes and the questions asked about them |
| `engine/audio.cpp` | `audio` | Mixing sound |
| `engine/input.cpp` | `input` | Touch, sticks and buttons, sensors, game pads, the keyboard |
| `engine/net.cpp` | `net` | HTTPS requests, calls to the game's own backend, and every safety rule on them |
| `engine/script.cpp` | `script` | Running a game's Lua: loading, callbacks, errors, hot reload |
| `engine/lua_<table>.cpp` | `script::api` | One file per Lua table (`lua_gfx.cpp`, `lua_phys.cpp`...): the functions games call |
| `engine/devsync.cpp` | `devsync` | Loader only: fetching changed game files from the PC |
| `engine/handles.h` | | The handle pool every subsystem uses |
| `engine/shaders/` | | `common.slang` and the engine's two shaders |
| `ios/*.mm` | | The two iPhone-only files |

Each has a page in this section. The headers, which are the fastest way to see what a subsystem offers, are all on [C++ headers](../reference/headers.md).

## Who calls whom

```
                         main.cpp
          (owns the loop; calls everything below once a frame)
               │
   ┌───────────┼───────────────┬────────────┬───────────┐
   ▼           ▼               ▼            ▼           ▼
 input      script          gfx          audio      devsync
            (Lua host)   (renderer)                (loader only)
               │
               │  a game's Lua calls these through script.cpp
   ┌─────┬─────┼─────┬───────┬───────┐
   ▼     ▼     ▼     ▼       ▼       ▼
  gfx  model phys    audio  input   net
```

Two rules keep this simple:

- **Subsystems do not call upward.** `gfx` knows nothing about Lua. `script` is the only file that includes Lua's headers.
- **Only `gfx.cpp` touches Vulkan, and only `devsync.cpp` touches the dev server.** If you find yourself needing a Vulkan type elsewhere, add a function to `gfx.h` instead.

## The principle: games ask by name

A game never knows where its files are. It says `gfx.load_texture("grass.png")` and the engine finds the file. That is what lets the same game run in three places without changing a line:

| Running as | Files come from |
|---|---|
| PC build | `games/<name>/` in the repository |
| Loader on the phone | Files synced into the app's Documents folder, falling back to the ones bundled inside the app |
| Standalone app | Bundled inside the app |

Keep to this when adding features. An API that takes a full path would work on your PC and break on the phone.

## Three builds, one source

| Build | Contains | Sync code |
|---|---|---|
| PC | The engine; reads games from the repository | No |
| Loader ("Game Lab") | The engine and every game | Yes (`LOADER_SYNC`) |
| Standalone | The engine and one game (`SINGLE_GAME`) | No |

The App Store does not allow an app to download code. A standalone build therefore must not contain the sync code at all, and it does not: `devsync.cpp` is only compiled into the loader. See [CMake and YAML](../languages/build-files.md).

## Dependencies

All are fetched at pinned versions by CMake. Nothing is installed system-wide.

| Library | Used for |
|---|---|
| SDL3 | Window, events, touch, sensors, game pads, audio output, the keyboard, file access |
| Lua 5.4 | The game scripting language |
| Vulkan-Headers, volk | Vulkan's declarations, and loading it at run time on desktop |
| MoltenVK | Vulkan on the iPhone (downloaded by the GitHub build) |
| stb_image, stb_truetype, stb_vorbis | Pictures, fonts, `.ogg` sound |
| cgltf | Reading `.glb` model files |

## Deliberate simplifications

The engine chooses the simple design wherever a small game does not need more. Each is marked `ponytail:` in the code with a note on what to do when it is outgrown.

| Simplification | Fine until |
|---|---|
| One draw call per mesh, no instancing | You draw thousands of the same thing |
| Characters are skinned on the processor | You have a crowd |
| Textures are uncompressed | Memory on older phones becomes tight |
| Assets load on the main thread | Loading hitches are noticeable (`lib.loader` spreads them across frames) |
| Capsules never tilt; sphere casts step along their path | You need precise fast-moving physics |
| No physics solver | Never: by decision, simulation is written in Lua |

## Read next

[A frame, start to finish](lifecycle.md) follows one frame through all of these files.

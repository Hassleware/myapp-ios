# CMake and YAML: how the engine gets built

Two files describe how source code becomes a program. `CMakeLists.txt` says *what* to build, for any platform. `.github/workflows/build.yml` tells GitHub *when* to build the iPhone app and what to do with it.

You will touch them rarely: when adding a source file, a library, or an iPhone setting. Both are native changes, so a mistake costs a ten-minute round trip. Read this page before editing either.

## Three builds from one description

| Build | Made by | What is special |
|---|---|---|
| PC | `python tools/dev.py run <game>` | Reads `games/` straight from the repository |
| Loader ("Game Lab") | `ship`, or any push that changes native code | Every game bundled, plus Wi-Fi sync |
| Standalone | `release <game>` | One game, no list, no sync |

## CMake in five ideas

CMake is a language for describing builds. It does not compile anything itself: it writes a Visual Studio project on Windows and an Xcode project on the Mac, and those do the compiling.

1. **Variables** are set with `set(NAME value)` and read with `${NAME}`.
2. **A target** is something to build. Ours is `gamelab`, made by `add_executable`.
3. **Properties are attached to targets**: `target_include_directories`, `target_compile_definitions`, `target_link_libraries`.
4. **`if()` blocks** choose per platform.
5. **`FetchContent`** downloads a library's source at a pinned version and builds it as part of ours.

## The file

::: file CMakeLists.txt

### Where to make common changes

**A new engine source file.** Add it to the list. Forgetting this is the most common build error: the linker reports an "unresolved external symbol".

::: source CMakeLists.txt ENGINE_SOURCES

**A new library.** Copy an existing `FetchContent_Declare` block and pin it to an exact tag or commit. Never track a branch: a build that worked yesterday must work tomorrow.

**A definition the C++ can test with `#if`.** These are how one source tree becomes three builds:

| Definition | Set when |
|---|---|
| `LOADER_SYNC` | Not a standalone game, and not Windows |
| `SINGLE_GAME` | A standalone build |
| `USE_VOLK` | Any desktop build |
| `BUILD_FINGERPRINT` | Always: the build code |
| `GFX_SCREENSHOT` | You configured with `-DGFX_SCREENSHOT=ON` |

**An iPhone setting.** Settings such as permissions text and allowed orientations live in `ios/Info.plist.in`. CMake fills in the `@NAME@` placeholders:

::: file ios/Info.plist.in

!!! danger "Never change a released game's bundle identifier"
    The identifier (`com.something.game`) is how Apple knows an update belongs to an existing app. Change it and players get a second, separate app with none of their data.

## The GitHub workflow

GitHub Actions runs a list of steps on a machine GitHub provides. Ours asks for a Mac, because only a Mac can build iPhone apps.

::: file .github/workflows/build.yml

### Reading it

- **`on:`** is when it runs. A push to `main` runs it **only if** the push touches `engine/`, `ios/`, `CMakeLists.txt` or the workflow itself. A commit that only changes games does not build an app, on purpose. `workflow_dispatch` lets `release <game>` start it by hand with inputs.
- **`jobs:`** has one job, `build`, on `macos-latest`.
- **`steps:`** run in order. Each has a name that shows up in `dev.py` while you wait.

| Step | Why it exists |
|---|---|
| Test the phone's link to dev.py | Compiles `engine/devsync.cpp` with `tests/devsync_test.cpp` and runs it against a real `dev.py`: connecting, downloading files, removing deleted games, backend calls, the size limit. A failure stops the build, so a broken sync never reaches the phone |
| Download MoltenVK | The iPhone has no Vulkan. MoltenVK is linked into the app |
| Work out what to build | Computes the build code by running `dev.py fingerprint`, and chooses loader or standalone |
| Reuse the last build folder | Most of a fresh build is setting up SDL3. Keeping the folder between runs skips that |
| Set up the Xcode project | Runs CMake with the iPhone settings |
| Build the app without signing | Compiles. Signing happens later, on the phone, through SideStore |
| Package the IPA | Zips the app in the layout iPhones expect |
| Keep the IPA as a private build file | Stores it where only someone logged in to the repository can fetch it |

### YAML in one minute

YAML is indentation-based. Two spaces per level, never tabs.

```yaml
key: value
list:
  - first
  - second
nested:
  inner: value
text: |
  Several lines,
  kept as they are.
```

`${{ ... }}` is GitHub's own expression syntax, filled in before the step runs. `$NAME` inside a `run:` block is an ordinary shell variable on the Mac.

## When a phone build fails

`ship` tells you it failed and prints a run number.

```powershell
gh run view <id> --log-failed
```

That prints only the failing step. The usual causes, most common first:

1. **A source file missing from `ENGINE_SOURCES`.**
2. **Code that Windows' compiler accepts and Apple's does not.** Clang is stricter about unused variables, missing `#include` lines and signed/unsigned comparisons.
3. **A mistake in an `ios/*.mm` file**, which no PC can check.
4. **A YAML indentation error.** The run fails at once, before any step.

Fix, then `ship` again.

## Exercises

??? example "1. Which line makes sure a standalone release contains no sync code?"
    `if(NOT SINGLE_GAME AND NOT WIN32)` around `set(WITH_SYNC ON)`. Only when `WITH_SYNC` is on does `engine/devsync.cpp` join the build and `LOADER_SYNC` get defined.

??? example "2. You pushed a change to `games/net-demo/main.lua` only. Does GitHub build an app?"
    No. The `paths:` filter does not match.

??? example "3. Where would you add the text iOS shows when asking for microphone permission?"
    In `ios/Info.plist.in`, as a new `<key>NSMicrophoneUsageDescription</key>` with a `<string>` after it.

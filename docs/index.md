# Welcome to Game Lab

Game Lab is one repository for making 3D iPhone games on a Windows PC, with no Mac. A C++ engine draws with Vulkan, games are written in Lua, shaders in Slang, and one Python tool runs the whole workflow. You edit a Lua file, save, and the game restarts on your PC and on the phone on your desk about a second later.

This handbook is where a new team member starts and where everyone looks things up afterwards.

## Pick your path

<div class="grid cards" markdown>

-   **I started today**

    Set up your machine, run a game, change it, see it on the phone.

    [Your first day](onboarding/day-one.md)

-   **I want to make a game**

    Twenty minutes from an empty folder to something you can play.

    [Your first game](onboarding/first-game.md)

-   **I need to learn a language we use**

    One tutorial per language, each taught through this codebase.

    [Which language goes where](languages/index.md)

-   **I am changing the engine**

    How the C++ is laid out and what each part is responsible for.

    [The big picture](engine/overview.md)

-   **I need to look something up**

    Every Lua function, every C++ header, every command.

    [Lua API](reference/lua-api.md)

-   **Something is broken**

    The tools for finding out why, on the PC and on the phone.

    [Debugging](guides/debugging.md)

</div>

## The project in one table

| Part | Language | Where | Changes reach the phone |
|---|---|---|---|
| Games | Lua | `games/<name>/` | Over Wi-Fi, in about a second |
| Shaders | Slang | `games/<name>/shaders/`, `engine/shaders/` | Over Wi-Fi, in about a second |
| Engine | C++ | `engine/` | By building and installing a new app (about 10 minutes) |
| iPhone-only parts | Objective-C++ | `ios/` | By building and installing a new app |
| Dev tool | Python | `tools/dev.py` | Never: it runs on your PC |
| Builds | CMake, YAML | `CMakeLists.txt`, `.github/workflows/` | By building a new app |

That last column is the most important idea in the project. It is explained in [How work reaches the phone](onboarding/shipping.md).

## This handbook cannot drift from the code

Most documentation goes stale because it is a copy. This one is not a copy:

- **Code on these pages is read from the repository** each time the handbook is built. When a page shows a function, it shows the function as it is right now, with its real line numbers and a link to the file. If someone renames or deletes that function, the build stops and names the page that needs attention.
- **The Lua API reference is checked against the C++.** Every function the engine gives to games must be described, and nothing may be described that the engine does not have. The result of that check is printed at the top of the [Lua API](reference/lua-api.md) page.
- **The same description file drives your editor.** In VS Code, with the Lua extension, typing `gfx.` lists the functions and hovering shows the same text you read here.

[Writing these docs](guides/docs.md) shows how it works and how to add a page.

## Reading it

```
python tools/dev.py docs
```

That builds the handbook and opens it in your browser. It also prints an address for other devices on the same Wi-Fi, so you can read it on a tablet next to your keyboard. It rebuilds itself whenever a page or any code it shows is saved.

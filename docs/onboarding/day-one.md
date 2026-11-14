# Your first day

By the end of this page you will have run a game on your PC, changed it and watched it update, and (if you have the team's test phone) seen the same change arrive there. Allow about an hour, most of it waiting for the first build.

Tick the boxes as you go. They are remembered in this browser.

## 1. Install the tools

Everything is run from PowerShell on Windows.

- [ ] **Git**: `winget install Git.Git`
- [ ] **GitHub CLI**: `winget install GitHub.cli`, then `gh auth login`
- [ ] **Python 3.8 or newer**: `winget install Python.Python.3.14`
- [ ] **CMake**: `winget install Kitware.CMake`
- [ ] **Visual Studio Build Tools** with the "Desktop development with C++" workload (the C++ compiler)
- [ ] **Vulkan SDK** from LunarG. It brings `slangc`, the shader compiler, and the validation layers
- [ ] **VS Code** with the *Lua* extension by sumneko, for autocomplete of the engine's functions

Close and reopen PowerShell after installing, so it can find the new programs.

## 2. Get the code and check your setup

```powershell
git clone https://github.com/Hassleware/myapp-ios.git
cd myapp-ios
python tools/dev.py doctor
```

`doctor` checks each tool and prints the command to fix anything that is missing.

- [ ] `doctor` reports no problems

## 3. Run a game on the PC

```powershell
python tools/dev.py run net-demo
```

The first run downloads SDL3, Lua and the other libraries and compiles the engine. That takes several minutes once; later runs take seconds. A window opens with the game in it.

- [ ] The game window opened

Now try the keys that only exist in the PC build:

| Key | What it does |
|---|---|
| ++f1++ | Steps through iPhone shapes. The window becomes that phone, with its notch or island drawn on top |
| ++f2++ | Turns the phone sideways and back |
| ++f3++ | Shows frame time, draw calls and memory |
| ++f5++ | Restarts the game |
| ++esc++ | Back to the list of games |

- [ ] I pressed ++f1++ until the window looked like an iPhone 11 (the team's test phone)

## 4. Change something and watch it reload

Leave the game window open. In VS Code, open `games/net-demo/main.lua`, find the line with `gfx.clear` and change the three numbers, for example to `0.20, 0.05, 0.10`. Save.

- [ ] The background changed colour without my restarting anything

That is **hot reload**. The engine noticed the file changed and restarted the game's Lua. You will work this way all day.

??? question "What happens if I save a mistake?"
    Try it: delete a closing bracket and save. The game stops and the error, with the file name and line number, is drawn on the screen and printed in the terminal. Fix the line, save, and the game comes back. Nothing crashes.

## 5. Start the dev tool

Close the game window. Then:

```powershell
python tools/dev.py
```

With no command, the tool **watches** the repository and **serves the phone**. It prints a prompt where you can type commands. This is the window you keep open while working.

- [ ] `dev.py` is running and showing its `>` prompt

Type `run net-demo` at the prompt to open the game again, or `docs` to open this handbook.

## 6. See it on the phone

Skip this step if you do not have a phone with Game Lab installed yet; ask whoever onboarded you.

1. Put the phone on the same Wi-Fi as your PC.
2. Open **Game Lab** (inside LiveContainer). It finds your PC by itself and lists the games.
3. Tap `net-demo`.
4. On the PC, change the `gfx.clear` colour again and save.

- [ ] The colour changed on the phone
- [ ] `dev.py` printed a `[live]` line for the file I saved

!!! info "Why this works"
    Apple does not let an app run new compiled code after it is installed. It does let an app read new *data*. Lua scripts, compiled shaders and models are all data, so they can be sent over Wi-Fi. The C++ engine cannot. See [How work reaches the phone](shipping.md).

## 7. Know where things are

```
engine/     the C++ engine
games/      one folder per game
ios/        the iPhone-only files
tools/      dev.py and the new-game template
docs/       this handbook
```

- [ ] I opened each folder and looked at what is in it

## What next

- [Your first game](first-game.md): build something from an empty folder.
- [Which language goes where](../languages/index.md): pick the tutorial for the part you will work on.
- [Glossary](glossary.md): every term this project uses, in plain words.

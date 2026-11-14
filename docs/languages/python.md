# Python: the language of the dev tool

`tools/dev.py` is one Python file of about 800 lines that runs the whole workflow: it watches your files, compiles shaders, serves the phone, builds the PC version, ships phone builds and serves this handbook. It uses **only Python's standard library**, so there is nothing to install to use it.

You will edit it when a repetitive job deserves to be one command.

## How the file is organised

It reads top to bottom in this order:

| Section | What is in it |
|---|---|
| Constants | Paths, the port, which folders count as native code |
| State | A small JSON file remembering what was last built and shipped |
| Fingerprint | The build code |
| Shaders | Finding `slangc` and compiling `.slang` files |
| Games | Listing games and their files, hashing them, creating one from the template |
| The server | The HTTP server the phone talks to |
| The watcher | Noticing saved files and describing what each change needs |
| Commands | One `cmd_` function per command: `run`, `ship`, `status`, `check`, `release`, `doctor`, `docs` |
| The console | The `>` prompt |
| `main` | Chooses a command from the arguments |

## Conventions

**Everything the tool prints goes through `say`**, which sends each line out at once. Without that, output piped to another program (as when `ship` is run by a script) would arrive in late bursts.

::: source tools/dev.py say

**Messages are written for the person reading them.** They state what happened and the exact command to run next. Compare `! Port 8765 is in use. Is this tool already running in another terminal?` with a bare traceback. When you add a message, write the first kind.

**Paths use `pathlib`**, never string joining, so the tool behaves the same on Windows and on GitHub's Mac:

```python
REPO = Path(__file__).resolve().parent.parent
GAMES = REPO / "games"
```

**Outside programs are run with a list of arguments**, never a single string through a shell. That removes a whole class of quoting bugs and injection problems:

::: source tools/dev.py git

## A worked example: the build code

This function is used by the tool, by the GitHub build and by the app. It must give the same answer on Windows and on a Mac, which is why it sorts carefully and normalises line endings.

::: source tools/dev.py native_files fingerprint

!!! bug "A real bug this code once had"
    Files were sorted as path objects. On Windows that comparison ignores letter case; on a Mac it does not. `CMakeLists.txt` sorted differently on the two systems, the hashes differed, and the phone forever claimed it needed an update. Sorting by the text of the path fixed it. When something must match across machines, be explicit about every detail.

## The server the phone talks to

The server is Python's built-in `http.server`. Each request is handled by a method on one class:

::: source tools/dev.py do_GET

Things to notice:

- It only ever serves files that belong to a game, found through `resolve`, which refuses paths that try to climb out of the game's folder.
- Replies are plain text, one item per line, tab-separated. The C++ on the other end can parse that in a few lines with no JSON library.

The whole protocol is listed in [dev.py and its server](../reference/dev-tool.md).

## Adding a command

Say you want `python tools/dev.py count` to print how many lines of Lua each game has.

**1. Write the function**, near the other `cmd_` functions:

```python
def cmd_count():
    for name in game_names():
        total = sum(len(path.read_text(encoding="utf-8").splitlines())
                    for rel, path in game_files(name) if rel.endswith(".lua"))
        say(f"{name}: {total} lines of Lua")
```

**2. Add it to `main`:**

```python
    elif command == "count":
        cmd_count()
```

**3. If it is useful at the prompt too**, add a branch in `console` and a line in `HELP`:

::: source tools/dev.py HELP

**4. Add it to the docstring at the top of the file**, which is what `python tools/dev.py help` prints, and to [dev.py and its server](../reference/dev-tool.md).

`game_names` and `game_files` already exist. Before writing a helper, search the file: it probably has one.

## Rules for this file

- **Standard library only.** A new team member must be able to run the tool right after installing Python. The one exception is the handbook, which needs MkDocs; `cmd_docs` checks for it and prints the install command instead of failing.
- **Python 3.8 compatible.** GitHub's build machine runs the fingerprint with whatever Python it has.
- **Never publish a build.** Builds are fetched with your own GitHub login and served only on your Wi-Fi.
- **One file.** Resist splitting it into a package. Being able to read the whole tool in one sitting is a feature.

## The handbook's Python

The other Python in the repository is `docs/hooks.py`, which MkDocs runs while building these pages. It finds functions in source files by name and checks the Lua API description against the C++. [Writing these docs](../guides/docs.md) explains it.

## Exercises

??? example "1. What does `python tools/dev.py fingerprint` print, and who uses it?"
    The eight-character build code for the files on disk. The GitHub workflow runs exactly this command to stamp the app; search `.github/workflows/build.yml` for `fingerprint`.

??? example "2. Find what decides whether a saved file is live or native"
    Read `describe_changes`. It compares two snapshots of the repository and labels each difference.

??? example "3. Add the `count` command above and run it"
    Four small edits. If it prints a line for `net-demo`, you have the pattern for every future command.

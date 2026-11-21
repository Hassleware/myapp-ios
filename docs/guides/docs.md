# Writing these docs

The handbook is built with **MkDocs** and the **Material** theme, a widely used documentation system: pages are plain Markdown files, and one command turns them into this site. On top of that, a small hook pulls real code out of the repository into the pages so they cannot go stale.

## Reading and editing

```
python tools/dev.py docs
```

This serves the handbook at `http://localhost:8766`, opens it in your browser, and prints an address for other devices on your Wi-Fi. Leave it running: when you save a page, or any source file a page shows, the site rebuilds and the browser refreshes.

The first time, install the two packages it needs:

```powershell
python -m pip install mkdocs-material
```

| Where | What |
|---|---|
| `docs/**/*.md` | The pages |
| `mkdocs.yml` | The site's settings and the menu (`nav:`) |
| `docs/hooks.py` | The code that pulls source into pages |
| `docs/api/gamelab.lua` | The description of the Lua API |
| `.luarc.json` | Tells VS Code's Lua extension to use that description |

## Adding a page

1. Create a `.md` file under `docs/`, in the section it belongs to.
2. Add it to `nav:` in `mkdocs.yml`.
3. Write.

## Showing real code

Never paste engine code into a page. Ask for it by name, and the build fetches the current version. Each instruction is a line of its own that starts with three colons.

**A function, struct or constant:**

```
::: source engine/net.cpp host_allowed
```

gives

::: source engine/net.cpp host_allowed

The block carries the real line numbers and a link to the file. Several names can follow the file. The comment written directly above the thing in the source is included, which is a good reason to write useful comments there.

**A whole file:**

```
::: file engine/handles.h
```

**One table of the Lua API:**

```
::: lua-api save
```

An instruction written inside a code block, as these three are, is shown rather than run.

### What can be found by name

| File type | What a name matches |
|---|---|
| `.cpp`, `.h`, `.mm`, `.slang` | A function, `struct`, `enum` or constant that starts at the left margin |
| `.py` | A `def`, a `class`, a method, or a top-level constant |
| `.lua` | A `function name(...)`, including `lib.name`, or a top-level table |
| `CMakeLists.txt` | A `set(NAME ...)` command |

### When code moves

If a page asks for a name that no longer exists, the build **stops** with a message such as:

```
docs: 'host_allowed' was not found in engine/net.cpp. Was it renamed or removed? Update the page that asks for it.
```

That is the point. The person who renamed the function finds out immediately which page talks about it, and fixes the explanation while the change is fresh.

Here is the part of the hook that does the finding for C-like languages:

::: source docs/hooks.py find_braced

## The Lua API description

`docs/api/gamelab.lua` describes every function the engine gives to games. It is one file with three readers:

1. **This handbook** builds the [Lua API](../reference/lua-api.md) page from it.
2. **VS Code** uses it for autocomplete and hover text, through the Lua extension.
3. **The check** compares it with what `engine/script.cpp` and the `engine/lua_*.cpp` files actually register.

An entry looks like this:

```lua
--- `save.get(key, default) -> value`
--- A value kept between runs of the game, or the default if it was never set.
function save.get(key, default) end
```

The first comment line, in backticks, is the call exactly as someone would write it. The lines after are the explanation. The empty function is for editors.

The check reads the C++ directly:

::: source docs/hooks.py read_registered api_drift

If you add a function to the engine and forget to describe it, building the handbook prints

```
WARNING - Lua API: input.haptic is registered in engine/script.cpp and engine/lua_*.cpp but not described in docs/api/gamelab.lua
```

and the top of the Lua API page turns red and lists it. To make that a hard failure, for example before merging:

```powershell
python -m mkdocs build --strict
```

## Things Markdown can do here

=== "Notes and warnings"

    ```markdown
    !!! tip "A title"
        Indented text is inside the box.

    ??? question "Click to open"
        A collapsed box. Good for exercise answers.
    ```

=== "Tabs"

    ```markdown
    === "First"

        Content of the first tab.

    === "Second"

        Content of the second tab.
    ```

=== "Checklists"

    ```markdown
    - [ ] Something to do
    - [x] Something done
    ```

    Boxes can be ticked in the browser and are remembered there.

=== "Keys"

    ```markdown
    Press ++f1++ or ++ctrl+v++
    ```

## Style

Write for someone clever who started this week.

- **Lead with what the reader can do**, then explain.
- **Say why**, especially for rules. A rule with a reason gets followed; one without gets worked around.
- **Plain words.** If a term is needed, define it or add it to the [Glossary](../onboarding/glossary.md).
- **State what is untested.** A page that says "not yet tried on a real phone" is more useful than one that sounds finished.
- **End tutorials with exercises** whose answers are in collapsed boxes.
- **Show code from the repository** with the instructions above, and write your own examples only for things a reader would type.

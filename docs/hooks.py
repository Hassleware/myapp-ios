"""Keeps the handbook in line with the code. MkDocs runs this file while it builds the site (see mkdocs.yml).

A page asks for real code with a line that starts with three colons:

    ::: source engine/net.cpp host_allowed        one or more functions, structs or constants, found by name
    ::: file games/net-demo/main.lua              a whole file
    ::: lua-api gfx                               the reference for one table of the Lua API (or: globals, callbacks)
    ::: lua-api-status                            whether the Lua API description matches what the C++ registers

An instruction inside a ``` code block is left alone, so pages can show these lines as examples.
If a name can't be found the build stops with an error, so a page can never quietly show code that has gone.
The Lua API is described in docs/api/gamelab.lua and checked against engine/script.cpp on every build.
"""

import logging
import re
from pathlib import Path

from mkdocs.exceptions import PluginError

log = logging.getLogger("mkdocs.hooks.gamelab")

REPO = Path(__file__).resolve().parent.parent
GITHUB = "https://github.com/Hassleware/myapp-ios/blob/main/"
API_FILE = "docs/api/gamelab.lua"
SCRIPT = "engine/script.cpp and engine/lua_*.cpp"   # how pages name the Lua host


def script_files():
    """The Lua host and its binding files (one lua_<table>.cpp per Lua table), host first."""
    return ["engine/script.cpp"] + sorted(p.relative_to(REPO).as_posix() for p in (REPO / "engine").glob("lua_*.cpp"))
HIGHLIGHT = {".cpp": "cpp", ".h": "cpp", ".c": "cpp", ".mm": "objc", ".py": "python", ".lua": "lua", ".slang": "hlsl",
             ".txt": "cmake", ".yml": "yaml", ".cfg": "ini", ".in": "xml", ".json": "json"}
COMMENT = {".py": "#", ".lua": "--", ".txt": "#", ".yml": "#", ".cfg": "#"}


def lines_of(path):
    full = REPO / path
    if not full.is_file():
        raise PluginError(f"docs: no file called {path}")
    return full.read_text(encoding="utf-8").splitlines()


def brace_depth_change(line):
    """How many more { than } a line of C-like code has, ignoring text in quotes and // comments."""
    depth, quote, i = 0, "", 0
    while i < len(line):
        c = line[i]
        if quote:
            if c == "\\":
                i += 1
            elif c == quote:
                quote = ""
        elif c in "\"'":
            quote = c
        elif line.startswith("//", i):
            break
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return depth


def find_braced(lines, symbol):
    name = re.compile(r"(?<![\w.])" + re.escape(symbol) + r"\b")
    starts = [i for i, line in enumerate(lines)
              if line and not line[0].isspace() and line[0] not in "/#}" and name.search(line)
              and ("(" in line or re.match(r"(struct|class|enum|namespace|const|static)\b", line))]
    # A definition if there is one, otherwise the declaration (as in a header).
    for want_definition in (True, False):
        for i in starts:
            code = lines[i].split("//")[0].rstrip()
            is_declaration = code.endswith(";") and brace_depth_change(code) == 0
            if is_declaration == want_definition:
                continue
            if is_declaration:
                return i, i
            depth, seen, j = 0, False, i
            while j < len(lines):
                change = brace_depth_change(lines[j])
                seen = seen or "{" in lines[j]
                depth += change
                if seen and depth <= 0:
                    return i, j
                j += 1
    return None


def find_cmake(lines, symbol):
    """A command such as set(NAME ...), to the bracket that closes it."""
    for i, line in enumerate(lines):
        if re.match(r"\w+\(\s*" + re.escape(symbol) + r"\b", line):
            depth, j = 0, i
            while True:
                depth += lines[j].count("(") - lines[j].count(")")
                if depth <= 0:
                    return i, j
                j += 1
    return None


def find_python(lines, symbol):
    for i, line in enumerate(lines):
        match = re.match(r"(\s*)(def|class)\s+" + re.escape(symbol) + r"\b", line)
        if match:
            indent, j = len(match.group(1)), i + 1
            while j < len(lines) and (not lines[j].strip() or len(lines[j]) - len(lines[j].lstrip()) > indent):
                j += 1
            while not lines[j - 1].strip():
                j -= 1
            return i, j - 1
        if re.match(re.escape(symbol) + r"\s*=", line):
            j = i
            if line.count('"""') == 1:   # a constant holding a block of text
                j += 1
                while '"""' not in lines[j]:
                    j += 1
            return i, j
    return None


def find_lua(lines, symbol):
    for i, line in enumerate(lines):
        if re.match(r"(local\s+)?function\s+" + re.escape(symbol) + r"\s*\(", line):
            if re.search(r"\bend\s*$", line):
                return i, i
            j = i + 1
            while j < len(lines) and lines[j].rstrip() != "end":
                j += 1
            return i, j
        if re.match(r"(local\s+)?" + re.escape(symbol) + r"\s*=", line):
            j = i
            if line.count("{") > line.count("}"):
                while not lines[j].startswith("}"):
                    j += 1
            return i, j
    return None


def locate(path, symbol):
    """First and last line (counting from 1) of a named thing, with the comment written above it."""
    lines = lines_of(path)
    suffix = Path(path).suffix
    finder = {".py": find_python, ".lua": find_lua, ".txt": find_cmake}.get(suffix, find_braced)
    found = finder(lines, symbol)
    if not found:
        raise PluginError(f"docs: '{symbol}' was not found in {path}. Was it renamed or removed? Update the page that asks for it.")
    start, end = found
    marker = COMMENT.get(suffix, "//")
    while start > 0 and lines[start - 1].startswith(marker):
        start -= 1
    return start + 1, end + 1


def code_block(path, first, last, caption):
    lines = lines_of(path)
    body = "\n".join(lines[first - 1:last])
    language = HIGHLIGHT.get(Path(path).suffix, "text")
    link = f"{GITHUB}{path}#L{first}-L{last}"
    return (f'```{language} title="{caption}" linenums="{first}"\n{body}\n```\n\n'
            f'<small>[{path}, lines {first} to {last}]({link}) · taken from the code when this page was built</small>\n')


def render_source(arguments):
    path, symbols = arguments[0], arguments[1:]
    if not symbols:
        raise PluginError(f"docs: '::: source {path}' needs at least one name after the file")
    blocks = []
    for symbol in symbols:
        first, last = locate(path, symbol)
        blocks.append(code_block(path, first, last, f"{path} · {symbol}"))
    return "\n".join(blocks)


def render_file(arguments):
    path = arguments[0]
    return code_block(path, 1, len(lines_of(path)), path)


# ---------------------------------------------------------------- the Lua API

def read_api():
    """docs/api/gamelab.lua as a list of entries: table, name, signature, description, group."""
    entries, comment, group = [], [], ""
    for line in lines_of(API_FILE):
        if line.startswith("-- # "):
            group, comment = line[5:].strip(), []
        elif line.startswith("---") and not line.startswith("---@"):
            comment.append(line[3:].strip())
        else:
            match = (re.match(r"function (\w+)\.(\w+)\(", line) or re.match(r"(\w+)\.([A-Z_]+) = ", line)
                     or re.match(r'(\w+)\["(\w+)"\] = function', line)
                     or re.match(r"function ()(\w+)\(", line) or re.match(r"-- (callback) (\w+)", line))
            if match:
                table, name = match.group(1) or "globals", match.group(2)
                if table == "callback":
                    table = "callbacks"
                signature = comment[0].strip("`") if comment and comment[0].startswith("`") else name
                description = " ".join(comment[1:] if comment and comment[0].startswith("`") else comment)
                entries.append({"table": table, "name": name, "signature": signature, "description": description, "group": group})
            if not line.startswith("---@"):
                comment = []
    return entries


def read_registered():
    """What engine/script.cpp really gives to games: {(table, name): C++ function}."""
    text = "\n".join("\n".join(lines_of(f)) for f in script_files())
    arrays = {name: re.findall(r'\{"(\w+)",\s*(\w+)\}', body)
              for name, body in re.findall(r"const luaL_Reg (\w+)\[\] = \{(.*?)\};", text, re.S)}
    registered = {}
    for table, array in re.findall(r'register_table\("(\w+)", (\w+)\)', text):
        for name, function in arrays.get(array, []):
            registered[(table, name)] = function
    for name, function in re.findall(r'lua_register\(L, "(\w+)", (\w+)\)', text):
        registered[("globals", name)] = function
    for name in re.findall(r'lua_setfield\(L, -2, "([A-Z_]+)"\)', text):
        registered[("gfx", name)] = ""
    for name in set(re.findall(r'\bcall(?:_with)?\("(\w+)"', text)) | set(re.findall(r'"(on_\w+)"', text)):
        registered[("callbacks", name)] = ""
    return registered


def api_drift():
    documented = {(e["table"], e["name"]) for e in read_api()}
    registered = set(read_registered())
    spell = lambda key: key[1] if key[0] in ("globals", "callbacks") else f"{key[0]}.{key[1]}"
    return sorted(map(spell, registered - documented)), sorted(map(spell, documented - registered))


def render_lua_api(arguments):
    table = arguments[0]
    registered = read_registered()
    script_lines = [(f, n, line) for f in script_files() for n, line in enumerate(lines_of(f), 1)]
    out, group = [], None
    for entry in read_api():
        if entry["table"] != table:
            continue
        if entry["group"] != group:
            group = entry["group"]
            out.append(f"### {group}\n")
        full = entry["name"] if table in ("globals", "callbacks") else f"{table}.{entry['name']}"
        out.append(f"#### {full}\n")
        out.append(f"```lua\n{entry['signature']}\n```\n")
        out.append(entry["description"] + "\n")
        function = registered.get((table, entry["name"]))
        if function:
            for path, number, line in script_lines:
                if re.match(r"int " + function + r"\(lua_State", line):
                    out.append(f"<small>C++: [`{function}` in {path}, line {number}]({GITHUB}{path}#L{number})</small>\n")
                    break
    if not out:
        raise PluginError(f"docs: '::: lua-api {table}' matched nothing in {API_FILE}")
    return "\n".join(out)


def render_lua_api_status(arguments):
    missing, stale = api_drift()
    if not missing and not stale:
        count = len(read_api())
        return (f'!!! success "In line with the code"\n    All {count} functions, constants and callbacks that `{SCRIPT}` gives to games '
                f"are described in `{API_FILE}`, and nothing is described that the engine does not have. Checked when this page was built.\n")
    text = '!!! failure "The API description and the code disagree"\n'
    if missing:
        text += f"    In the engine but not described in `{API_FILE}`: " + ", ".join(f"`{n}`" for n in missing) + "\n\n"
    if stale:
        text += f"    Described but no longer in the engine: " + ", ".join(f"`{n}`" for n in stale) + "\n"
    return text


RENDER = {"source": render_source, "file": render_file, "lua-api": render_lua_api, "lua-api-status": render_lua_api_status}


def on_pre_build(config):
    missing, stale = api_drift()
    for name in missing:
        log.warning(f"Lua API: {name} is registered in {SCRIPT} but not described in {API_FILE}")
    for name in stale:
        log.warning(f"Lua API: {name} is described in {API_FILE} but the engine no longer has it")


def on_page_markdown(markdown, page, config, files):
    out, fence = [], ""
    for line in markdown.split("\n"):
        stripped = line.strip()
        marker = re.match(r"`{3,}|~{3,}", stripped)
        if marker and not fence:
            fence = marker.group(0)
        elif fence and stripped == fence:
            fence = ""
        instruction = None if fence else re.match(r"(\s*)::: (.+)$", line)
        if not instruction:   # ordinary text, or an instruction being shown as an example inside a code block
            out.append(line)
            continue
        indent = instruction.group(1)
        kind, *arguments = instruction.group(2).split()
        if kind not in RENDER:
            raise PluginError(f"docs: unknown instruction '::: {kind}' in {page.file.src_path}")
        # An instruction inside a note or a list is indented, so what replaces it must be too.
        out.extend(indent + rendered if rendered else "" for rendered in RENDER[kind](arguments).split("\n"))
    return "\n".join(out)

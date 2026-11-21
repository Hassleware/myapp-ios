"""The CEV measurer's own check: python tools/cev/test_cev.py (prints ok, or fails on an assert)."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cev  # noqa: E402

# Comments and strings are blanked but keep their length and line breaks.
code, comments = cev.strip_c('int a = 1; // "x" {\nconst char* s = "}{"; /* {\n} */ int b;')
assert "{" not in code and code.count("\n") == 2 and len(code) == len('int a = 1; // "x" {\nconst char* s = "}{"; /* {\n} */ int b;')
assert comments == {0, 1, 2}
lua, lc = cev.strip_lua('local s = "end" -- function\nlocal t = [[\nend]] --[[ if\n]] x = 1')
assert "end" not in lua and "function" not in lua and "if" not in lua and lua.count("\n") == 3

# Functions are found with their extent and nesting, in C++ and Lua.
src = "int f(int x) {\n    if (x) {\n        for (;;) { break; }\n    }\n    return 0;\n}\n"
fs = cev.c_functions(cev.strip_c(src)[0])
assert [(f["name"], f["start"], f["end"]) for f in fs] == [("f", 1, 6)] and cev.c_nesting(fs[0]["body"]) == 2
lsrc = "local function g(a)\n  for i = 1, a do\n    if i then end\n  end\nend\nlocal h = function() end\n"
lfs = cev.lua_functions(cev.strip_lua(lsrc)[0])
assert [(f["name"], f["start"], f["end"]) for f in lfs] == [("g", 1, 5), ("h", 6, 6)], lfs
assert cev.lua_nesting(lfs[0]["body"]) == 2

# A Lua module wrapper (return function(game) ... end) is not reported as one long function.
with tempfile.TemporaryDirectory() as d:
    p = os.path.join(d, "mod.lua")
    with open(p, "w") as f:
        f.write("return function(game)\n" + "local x = 1\n" * 100 + "local function inner() end\nreturn {}\nend\n")
    cfg = {"max_file_lines": 800, "max_function_lines": 80, "max_nesting": 4, "max_line_width": 120}
    r = cev.measure_file(p, d, cfg)
    assert [f["name"] for f in r["functions"]] == ["inner"] and not r["findings"], r["findings"]
print("ok")

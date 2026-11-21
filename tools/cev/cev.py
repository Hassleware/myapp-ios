"""CEV: Cognitive Ease Verification. Measures how hard source files are to read, in any language this repo uses.

    python tools/cev/cev.py [paths...] [--config tools/cev/thresholds.json] [--json out.json]
                            [--baseline old.json] [--mode advisory|warn|strict]

With no paths it scans the paths listed in the config. Every number says how it was obtained:
  measured   exact counts (lines, widths, sizes, directory depth)
  parsed     a real parser for the language (Python, through its own ast module)
  heuristic  careful text scanning (C, C++, Objective-C, Slang, Lua): comments and strings are skipped, and
             functions are found by their headers and matched braces or block keywords; good, not perfect
Modes: advisory always exits 0; warn exits 1 when a threshold is crossed; strict also exits 1 when anything got
worse than the baseline. Nothing is sent anywhere: it reads files and writes a report.
"""
import ast
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LANGS = {
    ".cpp": "c", ".cc": "c", ".c": "c", ".h": "c", ".hpp": "c", ".mm": "c", ".m": "c", ".slang": "c",
    ".lua": "lua", ".py": "python", ".sh": "shell", ".cmake": "shell", ".yml": "shell", ".yaml": "shell",
    ".md": "text", ".cfg": "shell", ".txt": "shell", ".in": "text",
}
SPECIAL = {"CMakeLists.txt": "shell"}


# ---------------------------------------------------------------- text, with comments and strings blanked out

def strip_c(text):
    """Blank comments and string/char literals in C-like text, keeping line breaks. Returns (code, comment_lines)."""
    out, comment_lines, i, n, line = [], set(), 0, len(text), 0
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            comment_lines.add(line)
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            chunk = text[i:j]
            for k in range(chunk.count("\n") + 1):
                comment_lines.add(line + k)
            out.append(re.sub(r"[^\n]", " ", chunk))
            line += chunk.count("\n")
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * max(0, min(j, n) - i - 1) + (c if j < n and text[j] == c else ""))
            i = j + 1 if j < n and text[j] == c else j
        else:
            if c == "\n":
                line += 1
            out.append(c)
            i += 1
    return "".join(out), comment_lines


def strip_lua(text):
    """Blank comments and strings in Lua (including --[[ ]] and [[ ]] long forms)."""
    out, comment_lines, i, n, line = [], set(), 0, len(text), 0
    long_open = re.compile(r"\[(=*)\[")
    while i < n:
        c = text[i]
        if text.startswith("--", i):
            m = long_open.match(text, i + 2)
            if m:
                close = "]" + m.group(1) + "]"
                j = text.find(close, m.end())
                j = n if j < 0 else j + len(close)
            else:
                j = text.find("\n", i)
                j = n if j < 0 else j
            chunk = text[i:j]
            for k in range(chunk.count("\n") + 1):
                comment_lines.add(line + k)
            out.append(re.sub(r"[^\n]", " ", chunk))
            line += chunk.count("\n")
            i = j
            continue
        m = long_open.match(text, i)
        if m:
            close = "]" + m.group(1) + "]"
            j = text.find(close, m.end())
            j = n if j < 0 else j + len(close)
            chunk = text[i:j]
            out.append('"' + re.sub(r"[^\n]", " ", chunk[1:-1]) + '"' if len(chunk) > 1 else chunk)
            line += chunk.count("\n")
            i = j
            continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + " " * max(0, min(j, n) - i - 1) + (c if j < n and text[j] == c else ""))
            i = j + 1 if j < n and text[j] == c else j
            continue
        if c == "\n":
            line += 1
        out.append(c)
        i += 1
    return "".join(out), comment_lines


def strip_hash(text):
    """Blank # comments (shell, CMake, YAML, cfg) for counting."""
    lines, comment_lines = [], set()
    for k, l in enumerate(text.split("\n")):
        s = l.lstrip()
        if s.startswith("#"):
            comment_lines.add(k)
            lines.append(" " * len(l))
        else:
            lines.append(l)
    return "\n".join(lines), comment_lines


# ---------------------------------------------------------------- functions

C_HEADER = re.compile(r"(?:^|\n)([ \t]*)((?:[\w:<>,\*&~\[\]]+[ \t\*&]+)+)([\w:~]+)[ \t]*\(([^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)[^;{}()=]*\{")
C_CONTROL = {"if", "for", "while", "switch", "catch", "return", "else", "do", "sizeof", "case"}


def match_brace(code, open_at):
    depth, i = 0, open_at
    while i < len(code):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return len(code) - 1


def c_functions(code):
    """Function definitions in blanked C-like code: name, first and last line (1-based), body."""
    found = []
    for m in C_HEADER.finditer(code):
        name = m.group(3).split("::")[-1]
        if name in C_CONTROL or m.group(2).strip().split()[-1] in C_CONTROL:
            continue
        brace = m.end() - 1
        end = match_brace(code, brace)
        start_line = code.count("\n", 0, m.start(3)) + 1
        found.append({"name": m.group(3), "start": start_line, "end": code.count("\n", 0, end) + 1,
                      "body": code[brace:end + 1], "how": "heuristic"})
    # keep outermost only for lambdas matched as functions inside functions? keep all named, drop ones fully inside others
    found.sort(key=lambda f: (f["start"], -f["end"]))
    out = []
    for f in found:
        if out and f["start"] >= out[-1]["start"] and f["end"] <= out[-1]["end"] and out[-1]["name"] != f["name"]:
            # a definition inside another (a local struct method, say) is still worth listing
            f["nested_in"] = out[-1]["name"]
        out.append(f)
    return out


LUA_OPEN = re.compile(r"\b(function|if|for|while|repeat|do|end|until|then|elseif|else)\b")
LUA_FUNC = re.compile(r"(?:local\s+)?function\s+([\w.:\[\]\"]+)\s*\(|([\w.\[\]\"]+)\s*=\s*function\s*\(")


def lua_functions(code):
    """Named Lua functions with their extent, by matching block keywords to `end`/`until`."""
    tokens = [(m.start(), m.group(1)) for m in LUA_OPEN.finditer(code)]
    # `for ... do` and `while ... do` open one block, not two: a `do` right after a for/while header is not new
    blocks, stack, pending = [], [], []
    for pos, tok in tokens:
        if tok in ("for", "while"):
            stack.append([tok, pos])
            pending.append(len(stack) - 1)
        elif tok == "do":
            if pending:
                pending.pop()
            else:
                stack.append(["do", pos])
        elif tok in ("function", "if", "repeat"):
            stack.append([tok, pos])
        elif tok in ("end", "until"):
            if stack:
                kind, start = stack.pop()
                blocks.append((kind, start, pos, len(stack)))
    out = []
    for kind, start, end, depth in blocks:
        if kind != "function":
            continue
        head = code[max(0, start - 120):start + 200]
        m = None
        for mm in LUA_FUNC.finditer(code, max(0, start - 80), start + 200):
            if mm.start() <= start <= mm.end():
                m = mm
        name = (m.group(1) or m.group(2)) if m else "(anonymous)"
        out.append({"name": name, "start": code.count("\n", 0, start) + 1, "end": code.count("\n", 0, end) + 1,
                    "body": code[start:end], "how": "heuristic", "depth_at": depth})
    out.sort(key=lambda f: f["start"])
    return out


def py_functions(text):
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return None
    out = []

    def depth_of(node, d=0):
        best = d
        for ch in ast.iter_child_nodes(node):
            inc = 1 if isinstance(ch, (ast.If, ast.For, ast.While, ast.With, ast.Try, ast.AsyncFor, ast.AsyncWith)) else 0
            best = max(best, depth_of(ch, d + inc))
        return best

    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            branches = sum(isinstance(n, (ast.If, ast.For, ast.While, ast.Try, ast.BoolOp, ast.IfExp)) for n in ast.walk(node))
            stmts = sum(isinstance(n, ast.stmt) for n in ast.walk(node)) - 1
            out.append({"name": node.name, "start": node.lineno, "end": node.end_lineno, "how": "parsed",
                        "nesting": depth_of(node), "branches": branches, "statements": stmts})
    return sorted(out, key=lambda f: f["start"])


def c_nesting(body):
    depth, best = 0, 0
    for ch in body:
        if ch == "{":
            depth += 1
            best = max(best, depth)
        elif ch == "}":
            depth -= 1
    return max(0, best - 1)          # the function's own braces don't count


def lua_nesting(body):
    depth, best, pending = 0, 0, 0
    for m in LUA_OPEN.finditer(body):
        t = m.group(1)
        if t in ("for", "while"):
            depth += 1
            pending += 1
        elif t == "do":
            if pending:
                pending -= 1
            else:
                depth += 1
        elif t in ("if", "function", "repeat"):
            depth += 1
        elif t in ("end", "until"):
            depth -= 1
        best = max(best, depth)
    return max(0, best - 1)


# ---------------------------------------------------------------- one file

def measure_file(path, root, cfg):
    rel = os.path.relpath(path, root).replace("\\", "/")
    name = os.path.basename(path)
    lang = SPECIAL.get(name) or LANGS.get(os.path.splitext(name)[1].lower())
    if not lang:
        return None
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    raw = text.split("\n")
    if lang == "c":
        code, comments = strip_c(text)
    elif lang == "lua":
        code, comments = strip_lua(text)
    elif lang in ("python", "shell"):
        code, comments = strip_hash(text)
    else:
        code, comments = text, set()
    code_lines = code.split("\n")
    blank = sum(1 for l in raw if not l.strip())
    comment_only = sum(1 for k, l in enumerate(code_lines) if not l.strip() and raw[k].strip() and k in comments)
    widths = [len(l.rstrip("\r")) for l in raw]
    long_at = cfg["max_line_width"]
    rep = {
        "path": rel, "language": lang, "bytes": len(text.encode("utf-8")),
        "lines": {"physical": len(raw), "blank": blank, "comment": comment_only,
                  "code": len(raw) - blank - comment_only, "how": "measured" if lang != "text" else "measured"},
        "width": {"max": max(widths or [0]), "average": round(sum(widths) / max(1, len(widths)), 1),
                  "over_limit": sum(1 for w in widths if w > long_at),
                  "histogram": {b: sum(1 for w in widths if lo <= w < hi) for b, lo, hi in
                                (("0-79", 0, 80), ("80-99", 80, 100), ("100-119", 100, 120), ("120+", 120, 10 ** 6))},
                  "how": "measured"},
        "directory_depth": rel.count("/"),
        "functions": [], "findings": [],
    }
    if lang == "c":
        stmts = code.count(";") + len(re.findall(r"\b(if|for|while|switch|return)\b", code)) - len(re.findall(r"\bfor\s*\(", code)) * 2
        conds = len(re.findall(r"&&|\|\|", code))
        funcs = c_functions(code)
        for f in funcs:
            f["nesting"] = c_nesting(f["body"])
            f["statements"] = f["body"].count(";")
            f["branches"] = len(re.findall(r"\b(if|for|while|case|catch)\b|\?|&&|\|\|", f["body"]))
    elif lang == "lua":
        stmts = len([l for l in code_lines if l.strip()])
        conds = len(re.findall(r"\band\b|\bor\b", code))
        funcs = lua_functions(code)
        for f in funcs:
            f["nesting"] = lua_nesting(f["body"])
            f["statements"] = len([l for l in f["body"].split("\n") if l.strip()])
            f["branches"] = len(re.findall(r"\b(if|elseif|for|while|and|or)\b", f["body"]))
    elif lang == "python":
        funcs = py_functions(text) or []
        try:
            tree = ast.parse(text)
            stmts = sum(isinstance(n, ast.stmt) for n in ast.walk(tree))
            conds = sum(len(n.values) - 1 for n in ast.walk(tree) if isinstance(n, ast.BoolOp))
        except SyntaxError:
            stmts, conds = None, None
    else:
        funcs, stmts, conds = [], None, None
    rep["statements"] = {"count": stmts, "per_code_line": round(stmts / max(1, rep["lines"]["code"]), 2) if stmts else None,
                         "how": "parsed" if lang == "python" else "heuristic"}
    rep["compound_conditions"] = {"count": conds, "how": "parsed" if lang == "python" else "heuristic"}
    for f in funcs:
        f.pop("body", None)
        f["length"] = f["end"] - f["start"] + 1
    # A Lua module written as `return function(deps) ... end` is one anonymous function around the whole file: it is
    # the file, not a long function, so it is reported as the module wrapper and left out of the findings.
    funcs = [f for f in funcs if not (lang == "lua" and f["name"] == "(anonymous)" and f["length"] > 0.8 * len(raw))]
    rep["functions"] = funcs
    rep["max_nesting"] = max([f.get("nesting", 0) for f in funcs] or [0])
    # findings against the thresholds
    t = cfg
    if rep["lines"]["physical"] > t["max_file_lines"]:
        rep["findings"].append(f"file is {rep['lines']['physical']} lines (limit {t['max_file_lines']})")
    if rep["width"]["over_limit"]:
        rep["findings"].append(f"{rep['width']['over_limit']} lines wider than {long_at}")
    for f in funcs:
        if f["length"] > t["max_function_lines"]:
            rep["findings"].append(f"{f['name']} is {f['length']} lines (limit {t['max_function_lines']}, {f['how']})")
        if f.get("nesting", 0) > t["max_nesting"]:
            rep["findings"].append(f"{f['name']} nests {f['nesting']} deep (limit {t['max_nesting']}, {f['how']})")
    return rep


def walk(paths, root, cfg):
    skip = re.compile(cfg.get("skip", "^$"))
    for p in paths:
        full = os.path.join(root, p)
        if os.path.isfile(full):
            yield full
            continue
        for d, dirs, files in os.walk(full):
            dirs[:] = sorted(x for x in dirs if not skip.search(os.path.relpath(os.path.join(d, x), root).replace("\\", "/")))
            for f in sorted(files):
                fp = os.path.join(d, f)
                if not skip.search(os.path.relpath(fp, root).replace("\\", "/")):
                    yield fp


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--config", default=os.path.join(HERE, "thresholds.json"))
    ap.add_argument("--json")
    ap.add_argument("--baseline")
    ap.add_argument("--mode", choices=["advisory", "warn", "strict"])
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(HERE)))
    a = ap.parse_args(argv)
    with open(a.config, encoding="utf-8") as f:
        cfg = json.load(f)
    mode = a.mode or cfg.get("mode", "advisory")
    files = [r for r in (measure_file(p, a.root, cfg) for p in walk(a.paths or cfg["paths"], a.root, cfg)) if r]
    total = {"files": len(files), "physical": sum(f["lines"]["physical"] for f in files),
             "code": sum(f["lines"]["code"] for f in files), "comment": sum(f["lines"]["comment"] for f in files),
             "functions": sum(len(f["functions"]) for f in files), "findings": sum(len(f["findings"]) for f in files)}
    report = {"tool": "cev 1", "thresholds": cfg, "totals": total, "files": files}
    worse = []
    if a.baseline and os.path.exists(a.baseline):
        with open(a.baseline, encoding="utf-8") as f:
            base = {x["path"]: x for x in json.load(f)["files"]}
        for x in files:
            b = base.get(x["path"])
            if b and len(x["findings"]) > len(b["findings"]):
                worse.append(f"{x['path']}: {len(b['findings'])} -> {len(x['findings'])} findings")
        report["worse_than_baseline"] = worse
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
    for x in sorted(files, key=lambda r: -len(r["findings"])):
        if x["findings"]:
            print(f"{x['path']}  ({x['lines']['physical']} lines, max nesting {x['max_nesting']})")
            for line in x["findings"][:12]:
                print("   - " + line)
            if len(x["findings"]) > 12:
                print(f"   ... and {len(x['findings']) - 12} more")
    print(f"\nCEV: {total['files']} files, {total['physical']} lines ({total['code']} code, {total['comment']} comment), "
          f"{total['functions']} functions, {total['findings']} findings. Mode: {mode}.")
    for w in worse:
        print("worse than baseline: " + w)
    if mode == "warn" and total["findings"]:
        return 1
    if mode == "strict" and (total["findings"] or worse):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

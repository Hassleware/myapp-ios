#!/usr/bin/env python3
"""The one tool for working in this repo. Needs Python 3.8+ and nothing else.

    python tools/dev.py              watch the repo, serve the phone, and say what each change needs
    python tools/dev.py run <game>   build the PC version and play a game
    python tools/dev.py new <name>   create a game from the template
    python tools/dev.py ship [msg]   commit, push, wait for the iPhone build, print the install link
    python tools/dev.py status       what is out of date, and what to do about it
    python tools/dev.py release <game>   build one game as its own standalone app
    python tools/dev.py doctor       check that the tools this repo needs are installed

Two kinds of change, and the tool tells you which one you made:
    live     Lua, shaders, levels and other game files. Sent to the phone over Wi-Fi as you save.
    native   C++ in engine/, CMakeLists.txt, ios/. Needs a rebuild: `run` for the PC, `ship` for the phone.
"""

import hashlib
import json
import os
import platform
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

PORT = 8765
REPO = Path(__file__).resolve().parent.parent
GAMES = REPO / "games"
TOOLS = REPO / "tools"
STATE_FILE = TOOLS / ".state.json"
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,39}$")
SLANG_VERSION = "2026.22"
IPA_NAME = "GameLab.ipa"

NATIVE_DIRS = ["engine", "ios"]
NATIVE_FILES = ["CMakeLists.txt"]
BINARY_SUFFIXES = {".spv", ".png", ".jpg", ".bin"}
SHADER_STAGES = [("vertex", "vsMain", "vert"), ("fragment", "fsMain", "frag")]


def say(text=""):
    print(text, flush=True)


# ---------------------------------------------------------------- state

def load_state():
    try:
        return json.loads(STATE_FILE.read_text())
    except (OSError, ValueError):
        return {}


def save_state(**changes):
    state = load_state()
    state.update(changes)
    STATE_FILE.write_text(json.dumps(state, indent=2))


# ---------------------------------------------------------------- native code fingerprint

def native_files():
    files = [REPO / name for name in NATIVE_FILES]
    for folder in NATIVE_DIRS:
        files += [p for p in (REPO / folder).rglob("*") if p.is_file() and p.suffix != ".slang"]
    return sorted(p for p in files if p.is_file())


def fingerprint():
    """A short code for the current native code. The app carries the code it was built from,
    so the PC can tell when the phone is running older C++ than the repo has."""
    digest = hashlib.sha1()
    for path in native_files():
        data = path.read_bytes()
        if path.suffix not in BINARY_SUFFIXES:
            data = data.replace(b"\r\n", b"\n")   # the same on Windows and on GitHub's Mac
        digest.update(path.relative_to(REPO).as_posix().encode() + b"\0" + data + b"\0")
    return digest.hexdigest()[:8]


# ---------------------------------------------------------------- shaders

def find_slangc():
    override = os.environ.get("GAMELAB_SLANGC")
    if override:
        return override
    exe = "slangc.exe" if os.name == "nt" else "slangc"
    found = shutil.which("slangc")
    if found:
        return found
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        for sub in ("Bin", "bin"):
            candidate = Path(sdk) / sub / exe
            if candidate.is_file():
                return str(candidate)
    for candidate in (TOOLS / ".bin").rglob(exe):
        return str(candidate)
    return None


def download_slangc():
    """Fetches the Slang compiler into tools/.bin. Returns its path, or None if that didn't work."""
    system = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}.get(platform.system())
    machine = "aarch64" if platform.machine().lower() in ("arm64", "aarch64") else "x86_64"
    if not system:
        return None
    url = f"https://github.com/shader-slang/slang/releases/download/v{SLANG_VERSION}/slang-{SLANG_VERSION}-{system}-{machine}.zip"
    target = TOOLS / ".bin" / "slang"
    try:
        say(f"Downloading the Slang shader compiler ({url}) ...")
        target.mkdir(parents=True, exist_ok=True)
        archive = target / "slang.zip"
        urllib.request.urlretrieve(url, archive)
        with zipfile.ZipFile(archive) as z:
            z.extractall(target)
        archive.unlink()
        if os.name != "nt":
            for exe in target.rglob("slangc"):
                exe.chmod(0o755)
    except Exception as problem:   # any failure here just means "install it yourself"
        say(f"  Couldn't download it: {problem}")
        return None
    return find_slangc()


_slangc_warned = False


def slangc_or_explain():
    global _slangc_warned
    found = find_slangc() or (None if _slangc_warned else download_slangc())
    if not found and not _slangc_warned:
        _slangc_warned = True
        say("! No Slang compiler (slangc) found, so shaders can't be compiled.")
        say("  Install the Vulkan SDK from https://vulkan.lunarg.com (it includes slangc), or download Slang from")
        say("  https://github.com/shader-slang/slang/releases and unzip it into tools/.bin/slang. Then restart this tool.")
    return found


def shader_sources():
    sources = list((REPO / "engine" / "shaders").glob("*.slang"))
    sources += list(GAMES.glob("*/shaders/*.slang"))
    return sorted(sources)


def compile_shader(source, force=False):
    """Compiles one .slang file to <name>.vert.spv and <name>.frag.spv beside it. Returns True if all is well."""
    outputs = [source.with_name(f"{source.stem}.{suffix}.spv") for _, _, suffix in SHADER_STAGES]
    if not force and all(o.is_file() and o.stat().st_mtime >= source.stat().st_mtime for o in outputs):
        return True
    slangc = slangc_or_explain()
    if not slangc:
        return False
    rel = source.relative_to(REPO).as_posix()
    for (stage, entry, _), output in zip(SHADER_STAGES, outputs):
        temp = output.with_suffix(".spv.tmp")
        result = subprocess.run(
            [slangc, str(source), "-target", "spirv", "-matrix-layout-column-major",
             "-entry", entry, "-stage", stage, "-o", str(temp)],
            capture_output=True, text=True)
        if result.returncode != 0 or not temp.is_file():
            temp.unlink(missing_ok=True)
            say(f"! Shader error in {rel}:")
            say("  " + (result.stderr or result.stdout).strip().replace("\n", "\n  "))
            say("  The game keeps using the last version that compiled.")
            return False
        temp.replace(output)   # swap in one step, so the game never reads a half-written file
    say(f"  [shader] compiled {rel}")
    return True


def compile_all_shaders():
    return all([compile_shader(source) for source in shader_sources()])


# ---------------------------------------------------------------- games

def game_names():
    if not GAMES.is_dir():
        return []
    return sorted(p.name for p in GAMES.iterdir() if p.is_dir() and NAME_RE.match(p.name) and (p / "main.lua").is_file())


def game_files(name):
    """Files the phone needs: everything except shader sources and hidden files."""
    root = GAMES / name
    out = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if path.is_file() and path.suffix not in (".slang", ".tmp") and not any(part.startswith(".") for part in rel.parts):
            out.append((rel.as_posix(), path))
    return out


_hash_cache = {}


def file_hash(path):
    stat = path.stat()
    key = (str(path), stat.st_mtime_ns, stat.st_size)
    if key not in _hash_cache:
        _hash_cache[key] = hashlib.sha1(path.read_bytes()).hexdigest()
    return _hash_cache[key]


def games_revision():
    digest = hashlib.sha1()
    for name in game_names():
        for rel, path in game_files(name):
            try:
                stat = path.stat()
            except OSError:
                continue
            digest.update(f"{name}/{rel}:{stat.st_mtime_ns}:{stat.st_size};".encode())
    return digest.hexdigest()[:16]


def create_game(name):
    if not NAME_RE.match(name):
        return "Names use lowercase letters, numbers and dashes, for example: sky-runner"
    target = GAMES / name
    if target.exists():
        return f"'{name}' already exists."
    shutil.copytree(TOOLS / "template", target)
    return (f"Created games/{name}. It is on the phone's game list now.\n"
            f"  Play it on the PC with: python tools/dev.py run {name}")


# ---------------------------------------------------------------- server for the phone

_lock = threading.Lock()
_command = {"id": int(time.time()), "action": "none", "game": ""}
_phone = {"seen": 0.0, "build": None, "address": None}


def send_command(action, game=""):
    with _lock:
        _command.update(id=_command["id"] + 1, action=action, game=game)


def resolve(name, rel):
    if not NAME_RE.match(name):
        return None
    root = (GAMES / name).resolve()
    target = (root / rel).resolve()
    if root not in target.parents or not target.is_file():
        return None
    return target


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, body, status=200, kind="text/plain; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        parts = [unquote(p) for p in url.path.split("/") if p]
        if parts == ["api", "hello"]:
            return self.reply("gamelab\n")
        if parts == ["api", "state"]:
            build = (parse_qs(url.query).get("build") or [""])[0]
            note_phone(self.client_address[0], build)
            with _lock:
                command = dict(_command)
            return self.reply(f"{games_revision()}\n{command['id']}\n{command['action']}\n{command['game']}\n")
        if parts == ["api", "manifest"]:
            lines = []
            for name in game_names():
                for rel, path in game_files(name):
                    try:
                        lines.append(f"{name}\t{rel}\t{file_hash(path)}")
                    except OSError:
                        pass
            return self.reply("\n".join(lines) + "\n")
        if len(parts) >= 3 and parts[0] == "files":
            target = resolve(parts[1], "/".join(parts[2:]))
            if target:
                return self.reply(target.read_bytes(), kind="application/octet-stream")
        self.reply("Not found", 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        if urlparse(self.path).path == "/api/log":
            level, _, rest = body.partition("\t")
            game, _, message = rest.partition("\t")
            tag = "ERROR" if level == "error" else "print"
            say(f"  [phone {game}] {tag}: " + message.replace("\n", "\n      "))
            return self.reply("ok\n")
        self.reply("Not found", 404)


def note_phone(address, build):
    now = time.time()
    with _lock:
        was_away = now - _phone["seen"] > 6 or _phone["build"] != build
        _phone.update(seen=now, build=build, address=address)
    if was_away:
        say(f"  [phone] connected from {address}, running app build {build or 'unknown'}")
        advise_phone_build(build)


def phone_connected():
    with _lock:
        return time.time() - _phone["seen"] < 6


def advise_phone_build(build):
    current = fingerprint()
    if build and build != "dev" and build != current:
        say(f"! The app on the phone was built from older C++ (app {build}, repo {current}).")
        say("  Game files still sync, but engine changes are missing there. To update it, type: ship")


def lan_address():
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("10.255.255.255", 1))
        return probe.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        probe.close()


# ---------------------------------------------------------------- watching the repo

def snapshot():
    """Every file we care about, with its modified time."""
    files = {}
    for path in native_files() + shader_sources():
        try:
            files[path] = path.stat().st_mtime_ns
        except OSError:
            pass
    for name in game_names():
        for _, path in game_files(name):
            try:
                files[path] = path.stat().st_mtime_ns
            except OSError:
                pass
    return files


def describe_changes(before, after, known_games):
    changed = [p for p in after if before.get(p) != after[p]] + [p for p in before if p not in after]
    if not changed:
        return
    native_set = set(native_files())
    native_changed, live_changed = [], []
    for path in sorted(set(changed)):
        rel = path.relative_to(REPO).as_posix()
        if path.suffix == ".slang":
            compiled = path.is_file() and compile_shader(path, force=True)
            if rel.startswith("engine/"):
                native_changed.append(rel)
            elif compiled:
                live_changed.append(rel)
        elif path in native_set or rel.startswith(("engine/", "ios/")) or rel == "CMakeLists.txt":
            if path.suffix != ".spv":
                native_changed.append(rel)
        else:
            live_changed.append(rel)

    for name in game_names():
        if name not in known_games:
            known_games.add(name)
            say(f"  [new game] '{name}' is on the phone's game list now. On the PC: python tools/dev.py run {name}")

    if live_changed:
        where = "PC and phone" if phone_connected() else "PC (no phone connected right now)"
        shown = ", ".join(live_changed[:3]) + (f" and {len(live_changed) - 3} more" if len(live_changed) > 3 else "")
        say(f"  [live] {shown} -> reloading on the {where}")
    if native_changed:
        say(f"! [native] {', '.join(native_changed[:4])} changed. This is C++ or build code, so it can't be sent live.")
        say("    PC:    close the game window and type: run <game>      (rebuilds in seconds)")
        say("    Phone: when it works on the PC, type: ship            (GitHub builds a new app, a few minutes)")


def watch_loop(stop):
    known_games = set(game_names())
    before = snapshot()
    while not stop.is_set():
        time.sleep(0.5)
        after = snapshot()
        if after != before:
            try:
                describe_changes(before, after, known_games)
            except Exception as problem:   # keep watching whatever happens
                say(f"  (watch problem: {problem})")
            before = snapshot()


# ---------------------------------------------------------------- building and running on the PC

def run_checked(command, **kwargs):
    say("  $ " + " ".join(str(c) for c in command))
    return subprocess.run(command, **kwargs).returncode == 0


def build_pc():
    build_dir = REPO / "build"
    compile_all_shaders()
    if not shutil.which("cmake"):
        say("! CMake isn't installed. Run: python tools/dev.py doctor")
        return None
    if not (build_dir / "CMakeCache.txt").is_file():
        say("First build: downloading SDL3, Lua and the Vulkan headers. This takes a few minutes, once.")
        if not run_checked(["cmake", "-S", str(REPO), "-B", str(build_dir)]):
            say("! CMake couldn't set up the build. The lines above say why. `python tools/dev.py doctor` checks your tools.")
            return None
    if not run_checked(["cmake", "--build", str(build_dir), "--config", "RelWithDebInfo", "--parallel"]):
        say("! The C++ didn't compile. The first 'error' line above is the one to fix.")
        return None
    exe_name = "gamelab.exe" if os.name == "nt" else "gamelab"
    candidates = sorted(build_dir.rglob(exe_name), key=lambda p: p.stat().st_mtime, reverse=True)
    candidates = [c for c in candidates if c.is_file() and "_deps" not in c.parts]
    if not candidates:
        say("! Built, but couldn't find the program in build/.")
        return None
    save_state(pc_build=fingerprint())
    return candidates[0]


def cmd_run(game=None):
    if game and game not in game_names():
        say(f"No game called '{game}'. Games: {', '.join(game_names()) or 'none yet'}")
        return
    exe = build_pc()
    if exe:
        say(f"Starting {game or 'the game list'}. Esc goes back to the list, F5 restarts the game.")
        subprocess.Popen([str(exe)] + ([game] if game else []), cwd=str(REPO))


# ---------------------------------------------------------------- git, GitHub and the phone build

def git(*args):
    return subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True)


def repo_slug():
    result = subprocess.run(["gh", "repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner"],
                            cwd=REPO, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def wait_for_run(sha):
    say("Waiting for GitHub to start the build ...")
    for _ in range(30):
        result = subprocess.run(["gh", "run", "list", "--commit", sha, "--limit", "1", "--json", "databaseId", "--jq", ".[0].databaseId"],
                                cwd=REPO, capture_output=True, text=True)
        run_id = result.stdout.strip()
        if run_id and run_id != "null":
            return run_id
        time.sleep(4)
    return None


def cmd_ship(message=None):
    if not shutil.which("git") or not shutil.which("gh"):
        say("! `ship` needs git and the GitHub CLI (gh). Run: python tools/dev.py doctor")
        return
    if not compile_all_shaders():
        say("! Fix the shader problem above first, so the app isn't built with a stale shader.")
        return
    native_now = fingerprint()
    git("add", "-A")
    if git("diff", "--cached", "--quiet").returncode != 0:
        result = git("commit", "-m", message or "Update")
        if result.returncode != 0:
            say("! Couldn't commit:\n" + result.stdout + result.stderr)
            return
        say("Committed your changes.")
    result = git("push")
    if result.returncode != 0:
        say("! Couldn't push:\n" + result.stderr.strip())
        return
    sha = git("rev-parse", "HEAD").stdout.strip()
    state = load_state()
    if state.get("shipped") == native_now:
        say("Pushed. No C++ changed since the last phone build, so no new app is needed.")
        say("Game files reach the phone over Wi-Fi while this tool is watching.")
        return
    run_id = wait_for_run(sha)
    if not run_id:
        say("Pushed, but no build started for this commit. If only game files changed, that is expected.")
        say("To build anyway: gh workflow run build.yml")
        return
    ok = subprocess.run(["gh", "run", "watch", run_id, "--exit-status"], cwd=REPO).returncode == 0
    if not ok:
        say(f"! The iPhone build failed. See why with: gh run view {run_id} --log-failed")
        return
    save_state(shipped=native_now)
    slug = repo_slug() or "<you>/<repo>"
    say("")
    say(f"The app is built (build {native_now}). Install it on the phone:")
    say(f"  https://github.com/{slug}/releases/latest/download/{IPA_NAME}")
    say("  Open that link in Safari on the iPhone, then install the file with SideStore or LiveContainer.")


def cmd_release(game):
    if game not in game_names():
        say(f"No game called '{game}'. Games: {', '.join(game_names()) or 'none yet'}")
        return
    title = game.replace("-", " ").title()
    bundle = "com.starter." + game.replace("-", "")
    say(f"Asking GitHub to build '{game}' as its own app (name \"{title}\", id {bundle}).")
    say("Commit and push first if you have changes: the build uses what is on GitHub.")
    ok = subprocess.run(["gh", "workflow", "run", "build.yml", "-f", f"game={game}", "-f", f"app_name={title}", "-f", f"bundle_id={bundle}"],
                        cwd=REPO).returncode == 0
    if ok:
        say("Started. Watch it with: gh run watch")
        say(f"When it finishes, {game}.ipa appears under Releases. It contains only that game and no Wi-Fi syncing.")
        say("It is unsigned: fine for SideStore. For the App Store it still needs signing with your Apple developer account.")


def cmd_status():
    native_now = fingerprint()
    state = load_state()
    say(f"Native code fingerprint: {native_now}")
    say("")
    todo = []

    stale = [s for s in shader_sources()
             if not all(s.with_name(f"{s.stem}.{suffix}.spv").is_file() and
                        s.with_name(f"{s.stem}.{suffix}.spv").stat().st_mtime >= s.stat().st_mtime for _, _, suffix in SHADER_STAGES)]
    if stale:
        todo.append("Shaders need compiling: " + ", ".join(s.relative_to(REPO).as_posix() for s in stale) +
                    "\n    -> run `python tools/dev.py` (it compiles them as it starts)")

    if state.get("pc_build") != native_now:
        todo.append("The PC build is older than your C++.\n    -> python tools/dev.py run <game>")
    if state.get("shipped") != native_now:
        last = state.get("shipped")
        todo.append(("The phone app has never been built from this machine." if not last else
                     f"The phone app (build {last}) is older than your C++ ({native_now}).") +
                    "\n    -> python tools/dev.py ship")

    if shutil.which("git") and (REPO / ".git").exists():
        dirty = [line for line in git("status", "--porcelain").stdout.splitlines() if line.strip()]
        native_dirty = [l for l in dirty if l[3:].startswith(("engine/", "ios/", "CMakeLists.txt", ".github/"))]
        if dirty:
            todo.append(f"{len(dirty)} file(s) not committed ({len(native_dirty)} of them native code)."
                        "\n    -> `python tools/dev.py ship \"what changed\"` commits and pushes everything")
        ahead = git("rev-list", "--count", "@{u}..HEAD")
        if ahead.returncode == 0 and ahead.stdout.strip() not in ("", "0"):
            todo.append(f"{ahead.stdout.strip()} commit(s) not pushed to GitHub.\n    -> python tools/dev.py ship")

    if todo:
        say("To do:")
        for item in todo:
            say("  - " + item)
    else:
        say("Everything is up to date: shaders compiled, PC build current, phone app current, all pushed.")
    say("")
    say("Games: " + (", ".join(game_names()) or "none yet"))


def cmd_doctor():
    def check(label, found, fix):
        say(f"  {'ok     ' if found else 'MISSING'} {label}" + ("" if found else f"\n          -> {fix}"))
        return bool(found)

    say("Checking the tools this repo uses:")
    ok = True
    ok &= check("Python 3.8+", sys.version_info >= (3, 8), "winget install Python.Python.3.12")
    ok &= check("git", shutil.which("git"), "winget install Git.Git")
    ok &= check("GitHub CLI (gh)", shutil.which("gh"), "winget install GitHub.cli   then: gh auth login")
    ok &= check("CMake", shutil.which("cmake"), "winget install Kitware.CMake   then reopen the terminal")
    if os.name == "nt":
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        compiler = False
        if vswhere.is_file():
            out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
                                  "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                                 capture_output=True, text=True).stdout.strip()
            compiler = bool(out)
        compiler = compiler or shutil.which("cl") or shutil.which("clang++") or shutil.which("g++")
        ok &= check("C++ compiler (Visual Studio Build Tools)", compiler,
                    'winget install Microsoft.VisualStudio.2022.BuildTools   and tick "Desktop development with C++"')
    else:
        ok &= check("C++ compiler", shutil.which("c++") or shutil.which("g++") or shutil.which("clang++"), "install g++ or clang")
    ok &= check("Slang shader compiler (slangc)", find_slangc() or download_slangc(),
                "install the Vulkan SDK from https://vulkan.lunarg.com, or unzip a Slang release into tools/.bin/slang")
    say("")
    say("All set." if ok else "Install what is missing, reopen the terminal, and run this again.")
    say("A graphics driver with Vulkan 1.2 is also needed to run games on the PC. Any recent NVIDIA, AMD or Intel driver has it.")


# ---------------------------------------------------------------- the interactive session

HELP = """Commands:
  run <game>     build the PC version and play a game (run on its own shows the game list)
  new <name>     create a game from the template
  play <game>    open a game on the phone          menu     back to the phone's game list
  reload         restart the game on the phone     list     show all games
  status         what is out of date               ship [message]   build a new app for the phone
  quit"""


def console():
    say(HELP)
    while True:
        try:
            line = input("> ").strip()
        except (EOFError, KeyboardInterrupt):
            return
        if not line:
            continue
        word, _, rest = line.partition(" ")
        rest = rest.strip()
        if word == "new" and rest:
            say(create_game(rest))
        elif word == "play" and rest:
            if rest in game_names():
                send_command("play", rest)
                say(f"Told the phone to open '{rest}'." + ("" if phone_connected() else " (No phone is connected yet.)"))
            else:
                say(f"No game called '{rest}'. Type: list")
        elif word == "menu":
            send_command("menu")
        elif word == "reload":
            send_command("reload")
        elif word == "list":
            say("\n".join(game_names()) or "No games yet. Type: new mygame")
        elif word == "run":
            cmd_run(rest or None)
        elif word == "status":
            cmd_status()
        elif word == "ship":
            cmd_ship(rest.strip('"') or None)
        elif word in ("quit", "exit"):
            return
        else:
            say(HELP)


def cmd_watch():
    GAMES.mkdir(exist_ok=True)
    compile_all_shaders()
    try:
        server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    except OSError:
        say(f"! Port {PORT} is in use. Is this tool already running in another terminal?")
        return
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    stop = threading.Event()
    threading.Thread(target=watch_loop, args=(stop,), daemon=True).start()

    say(f"Watching {REPO}")
    say(f"Serving the phone at {lan_address()}:{PORT}. The Game Lab app finds this by itself on the same Wi-Fi.")
    say(f"Native code fingerprint: {fingerprint()}")
    state = load_state()
    if state.get("pc_build") != fingerprint():
        say("  The PC build is older than your C++ (or was never built). Type: run <game>")
    say("")
    if sys.stdin and sys.stdin.isatty():
        console()
    else:
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass
    stop.set()
    server.shutdown()


def main():
    args = sys.argv[1:]
    command = args[0] if args else "watch"
    if command == "watch":
        cmd_watch()
    elif command == "run":
        cmd_run(args[1] if len(args) > 1 else None)
    elif command == "new" and len(args) > 1:
        say(create_game(args[1]))
    elif command == "ship":
        cmd_ship(" ".join(args[1:]) or None)
    elif command == "status":
        cmd_status()
    elif command == "release" and len(args) > 1:
        cmd_release(args[1])
    elif command == "doctor":
        cmd_doctor()
    elif command == "fingerprint":
        print(fingerprint())
    elif command == "shaders":
        sys.exit(0 if compile_all_shaders() else 1)
    else:
        say(__doc__)


if __name__ == "__main__":
    main()

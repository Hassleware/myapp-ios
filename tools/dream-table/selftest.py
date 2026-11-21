"""Dream Table's behaviour test: plays the game against itself, hidden and fast, and checks the result.

    python tools/dream-table/selftest.py           both tests (about a minute)
    python tools/dream-table/selftest.py swipes    one of them

Each test appends a probe from selftest/ to games/dream-table/main.lua, runs the PC build hidden and silent at 60
steps a frame, then puts main.lua back. The probe prints a hash of where the ball was every half second and the
score. Same hash: gameplay is unchanged. A different hash after a change you meant to make: check that the game
still plays right, then put the new hash in EXPECTED below.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAIN = REPO / "games" / "dream-table" / "main.lua"
STEPS = 30000
EXPECTED = {"swipes": "3781b6b0", "moves": "199a66a6"}   # checked on 21 November 2026


def gamelab():
    for config in ("Release", "RelWithDebInfo", "Debug", "."):
        for name in ("gamelab.exe", "gamelab"):
            exe = REPO / "build" / config / name
            if exe.is_file():
                return exe
    sys.exit("No PC build found in build/. Build it first: python tools/dev.py run dream-table")


def run(test, exe):
    original = MAIN.read_bytes()
    probe = (Path(__file__).parent / "selftest" / f"{test}.lua").read_text(encoding="utf-8")
    env = dict(os.environ, GAMELAB_TURBO="60", GAMELAB_NO_MSAA="1", GAMELAB_HIDDEN="1", GAMELAB_DEVICE="1u",
               GAMELAB_SHOT=str(REPO / "build" / f"selftest-{test}.bmp"), GAMELAB_SHOT_FRAME=str(STEPS // 60 + 120))
    try:
        MAIN.write_bytes(f"PROBE_STEPS = {STEPS}\n".encode() + original + b"\n" + probe.encode("utf-8"))
        out = subprocess.run([str(exe), "dream-table"], cwd=REPO, env=env, capture_output=True, text=True, timeout=600)
    finally:
        MAIN.write_bytes(original)   # always: a probe left in main.lua would sync to the phone
    text = out.stdout + out.stderr
    found = re.search(r"PROBE steps=\d+ returns=(\d+) swipes=\d+ hash=(\w+)", text)
    if not found:
        print(f"FAIL  {test}: no result. The game said:\n" + "\n".join(text.splitlines()[-15:]))
        return False
    returns, got = found.groups()
    ok = got == EXPECTED[test]
    print(f"{'ok  ' if ok else 'FAIL'}  {test}: hash {got} (expected {EXPECTED[test]}), {returns} returns")
    return ok


if __name__ == "__main__":
    tests = sys.argv[1:] or list(EXPECTED)
    exe = gamelab()
    results = [run(t, exe) for t in tests]
    sys.exit(0 if all(results) else 1)

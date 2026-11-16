"""Cuts a long mocap take of many table tennis shots into one clip per shot, and guesses which shot each is.

    python tools/split_shots.py shots-1

The mocap game beeps during a shots take: a high beep every PERIOD seconds (the first at FIRST_HIT), two ticks
before each. One different shot is hit on each high beep. So shot k is hit at FIRST_HIT + k * PERIOD seconds, and
its clip runs half a period either side, from one ready stance to the next. The last shot is dropped if the take
stopped less than AFTER seconds after its beep (stopping the recording spoils it).

Reads captures/<name>-sam3d.npz, writes captures/<name>-<k>-<guess>-sam3d.npz (same keys, frames sliced), and
prints what it measured. The guess comes from the right (bat) wrist around the hit, in the body's own frame:
which side of the body (forehand or backhand), how high, how fast, and how much the swing rises or falls.
It is a guess: check it, then rename a file if it is wrong. The mannequin export takes any of these files.
Keep FIRST_HIT and PERIOD the same as in games/mocap/main.lua.
"""

import sys
from pathlib import Path

import numpy as np

FIRST_HIT, PERIOD = 4.0, 4.0
AFTER = 1.0          # seconds a shot needs after its beep to count as finished


def guess(side, height, rise, speed):
    hand = "fh" if side > 0 else "bh"
    if speed < 1.2:
        return f"{hand}-block" if hand == "bh" and abs(rise) < 0.15 else f"{hand}-push"
    if height > 0.45 and rise < -0.2:
        return f"{hand}-smash"
    if rise > 0.45:
        return f"{hand}-loop"
    if speed < 2.2 and height < 0.1:
        return f"{hand}-flick"
    return f"{hand}-drive"


def split(name):
    captures = Path(__file__).resolve().parent.parent / "captures"
    take = np.load(captures / f"{name}-sam3d.npz")
    data = {k: take[k] for k in take.files}
    fps, positions, names = float(data["fps"]), data["positions"], list(data["names"])
    j = {n: names.index(n) for n in ("root", "l_upleg", "r_upleg", "r_wrist", "c_spine3")}
    frames = len(positions)
    print(f"{name}: {frames / fps:.1f} s")
    print(" k   hit s   side m  height m  rise m  speed m/s  guess")
    k = 0
    while True:
        hit = FIRST_HIT + k * PERIOD
        if hit + AFTER > frames / fps:
            break
        f = int(round(hit * fps))
        p = positions
        # The body's frame at the hit: right = left hip to right hip, flat; up = y; forward = up x right.
        right = p[f, j["r_upleg"]] - p[f, j["l_upleg"]]
        right[1] = 0
        right /= np.linalg.norm(right)
        wrist = p[:, j["r_wrist"]]
        rel = wrist[f] - p[f, j["root"]]
        side = float(rel @ right)                                   # + right of the hips: forehand side
        height = float(wrist[f, 1] - p[f, j["root"], 1])            # above the hips
        a, b = max(0, f - int(0.4 * fps)), min(frames - 1, f + int(0.4 * fps))
        rise = float(wrist[b, 1] - wrist[a, 1])
        window = wrist[max(0, f - 6):min(frames, f + 7)]
        speed = float(np.max(np.linalg.norm(np.diff(window, axis=0), axis=1)) * fps)
        label = guess(side, height, rise, speed)
        start, end = max(0, int((hit - PERIOD / 2) * fps)), min(frames, int((hit + PERIOD / 2) * fps))
        clip = dict(data)
        clip["positions"], clip["rotations"] = positions[start:end], data["rotations"][start:end]
        out = captures / f"{name}-{k + 1}-{label}-sam3d.npz"
        np.savez_compressed(out, **clip)
        print(f"{k + 1:2d}  {hit:5.1f}  {side:+6.2f}  {height:+7.2f}  {rise:+6.2f}  {speed:8.1f}   {label}  -> {out.name}")
        k += 1
    if k == 0:
        print("no complete shots in this take")


if __name__ == "__main__":
    split(sys.argv[1] if len(sys.argv) > 1 else "shots-1")

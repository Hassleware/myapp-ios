"""Cuts a long mocap take of many table tennis shots into one clip per shot, and guesses which shot each is.

    python tools/split_shots.py shots-1

The mocap game counts in each shot with a high beep every PERIOD seconds (the first at FIRST_HIT). One different
shot is hit on each. The real hit is found as the bat wrist's fastest moment near each beep, and the clip runs half
a period either side of it, from one ready stance to the next. The last shot is dropped (walking over to stop
the recording spoils it).

Reads captures/<name>-sam3d.npz, writes captures/<name>-<k>-<guess>-sam3d.npz (same keys, frames sliced), and
prints what it measured. The guess comes from the right (bat) wrist a quarter second either side of the hit, in
the body's own frame: right to left across the body is a forehand, left to right a backhand; then how much it
rises or falls, how high it starts and how fast it is. Checked on shots-2 against watching the video: 9 of 11.
It is a guess: check it, then rename a file if it is wrong. The mannequin export takes any of these files.
Keep FIRST_HIT and PERIOD the same as in games/mocap/main.lua.
"""

import sys
from pathlib import Path

import numpy as np

FIRST_HIT, PERIOD = 4.0, 4.0
SEARCH = (-0.7, 1.6)  # the real hit is the bat wrist's fastest moment this long around each beep: sound and video
                      # timing on the phone put it about 0.8 s after the beep in shots-2


def guess(across, up, start_height, peak):
    """Forehand swings right to left across the body, backhand left to right. Then by the swing's shape."""
    hand = "fh" if across < 0 else "bh"
    if up < -0.3 and start_height > 0.4:
        return f"{hand}-smash" if peak >= 2.8 else f"{hand}-push"
    if up < -0.15:
        return f"{hand}-push"
    if up > 0.25:
        return f"{hand}-loop" if start_height < 0.15 else f"{hand}-drive"
    return f"{hand}-flick" if peak >= 2.6 else f"{hand}-block"


def split(name):
    captures = Path(__file__).resolve().parent.parent / "captures"
    take = np.load(captures / f"{name}-sam3d.npz")
    data = {k: take[k] for k in take.files}
    fps, p, names = float(data["fps"]), data["positions"], list(data["names"])
    rw, root, lu, ru = (names.index(n) for n in ("r_wrist", "root", "l_upleg", "r_upleg"))
    frames = len(p)
    speed = np.convolve(np.r_[0, np.linalg.norm(np.diff(p[:, rw], axis=0), axis=1) * fps], np.ones(3) / 3, "same")
    hits = []
    k = 0
    while FIRST_HIT + k * PERIOD + SEARCH[1] <= frames / fps:
        beep = FIRST_HIT + k * PERIOD
        a, b = int((beep + SEARCH[0]) * fps), int((beep + SEARCH[1]) * fps)
        hits.append(a + int(np.argmax(speed[a:b])))
        k += 1
    hits = hits[:-1]   # the last one is spoilt by walking over to stop the recording
    print(f"{name}: {frames / fps:.1f} s, {len(hits)} shots (the last beep's dropped)")
    print(" k   hit s  late s  across    up  start h  peak m/s  guess")
    half = int(PERIOD / 2 * fps)
    for k, f in enumerate(hits):
        right = p[f, ru] - p[f, lu]
        right[1] = 0
        right /= np.linalg.norm(right)
        s, e = max(0, f - int(0.25 * fps)), min(frames - 1, f + int(0.25 * fps))
        move = p[e, rw] - p[s, rw]
        across, up, start_height = float(move @ right), float(move[1]), float(p[s, rw, 1] - p[f, root, 1])
        label = guess(across, up, start_height, float(speed[f]))
        # The clip: PERIOD/2 either side of the hit, so the hit is always PERIOD/2 in (Dream Table's CONTACT).
        start, end = f - half, f + half
        idx = np.clip(np.arange(start, end), 0, frames - 1)
        clip = dict(data)
        clip["positions"], clip["rotations"] = p[idx], data["rotations"][idx]
        out = captures / f"{name}-{k + 1}-{label}-sam3d.npz"
        np.savez_compressed(out, **clip)
        beep = FIRST_HIT + k * PERIOD
        print(f"{k + 1:2d}  {f / fps:5.1f}  {f / fps - beep:+5.2f}  {across:+6.2f}  {up:+5.2f}  {start_height:+6.2f}  {speed[f]:7.1f}   {label}  -> {out.name}")


if __name__ == "__main__":
    split(sys.argv[1] if len(sys.argv) > 1 else "shots-1")

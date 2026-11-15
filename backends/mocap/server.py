"""The mocap game's backend: turns a take recorded on the phone into a .bvh file for Blender.

The phone sends   POST /take?name=walk   with the recording as text (see ios/bodytrack.mm for the format).
This writes      captures/walk.bvh   in the repo (git ignores the folder) and answers with where it went.

In Blender: File > Import > Motion Capture (.bvh), scale 1 (one unit is one metre), forward -Z, up Y.
Then retarget onto your own armature (for example with Rokoko's free Blender add-on).
Change anything here and save: the next take uses the new code, with no app build.
"""

import json
import math
import re
from pathlib import Path

CAPTURES = Path(__file__).resolve().parents[2] / "captures"
FPS = 60


def parse(text):
    lines = text.split("\n")
    if not lines[0].startswith("gamelab-mocap"):
        raise ValueError("not a Game Lab mocap take")
    count = int(lines[1].split()[1])
    joints = []
    for line in lines[2:2 + count]:
        name, parent, offset = line.split("\t")
        joints.append({"name": name, "parent": int(parent), "offset": [float(v) for v in offset.split()]})
    frames = [[float(v) for v in line.split()] for line in lines[3 + count:] if line.strip()]
    return joints, frames


def quat_multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def zxy_degrees(q):
    """The Z, X, Y angles (applied in that order, as the BVH channels below say) that make rotation q."""
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1
    x, y, z, w = x / n, y / n, z / n, w / n
    m01 = 2 * (x * y - w * z)
    m11 = 1 - 2 * (x * x + z * z)
    m20 = 2 * (x * z - w * y)
    m21 = 2 * (y * z + w * x)
    m22 = 1 - 2 * (x * x + y * y)
    rx = math.asin(max(-1, min(1, m21)))
    rz = math.atan2(-m01, m11)
    ry = math.atan2(-m20, m22)
    return math.degrees(rz), math.degrees(rx), math.degrees(ry)


def to_bvh(joints, frames):
    children = {i: [] for i in range(len(joints))}
    root = 0
    for i, joint in enumerate(joints):
        if joint["parent"] < 0:
            root = i
        else:
            children[joint["parent"]].append(i)

    order, out = [], ["HIERARCHY"]

    def write(i, depth):
        pad = "  " * depth
        joint = joints[i]
        order.append(i)
        out.append(f"{pad}{'ROOT' if i == root else 'JOINT'} {joint['name']}")
        out.append(pad + "{")
        offset = (0, 0, 0) if i == root else joint["offset"]
        out.append(f"{pad}  OFFSET {offset[0]:.5f} {offset[1]:.5f} {offset[2]:.5f}")
        if i == root:
            out.append(f"{pad}  CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation")
        else:
            out.append(f"{pad}  CHANNELS 3 Zrotation Xrotation Yrotation")
        for child in children[i]:
            write(child, depth + 1)
        if not children[i]:
            out.append(f"{pad}  End Site")
            out.append(pad + "  {")
            out.append(pad + "    OFFSET 0 0 0.02")
            out.append(pad + "  }")
        out.append(pad + "}")

    write(root, 0)

    # Frames arrive whenever ARKit had one; BVH wants a steady rate, so take the nearest frame to each tick.
    duration = frames[-1][0]
    ticks = max(1, int(duration * FPS) + 1)
    out += ["MOTION", f"Frames: {ticks}", f"Frame Time: {1 / FPS:.6f}"]
    k = 0
    for tick in range(ticks):
        t = tick / FPS
        while k + 1 < len(frames) and abs(frames[k + 1][0] - t) <= abs(frames[k][0] - t):
            k += 1
        f = frames[k]
        position, body_turn = f[1:4], f[4:8]
        values = []
        for i in order:
            q = tuple(f[8 + i * 4: 12 + i * 4])
            if i == root:
                values += position
                q = quat_multiply(body_turn, q)   # where the body faces in the room, then the root joint's own turn
            values += zxy_degrees(q)
        out.append(" ".join(f"{v:.4f}" for v in values))
    return "\n".join(out) + "\n", ticks, duration


def handle(method, path, query, body, headers):
    if method == "POST" and path == "/take":
        name = re.sub(r"[^a-z0-9-]", "", (query.get("name") or ["take"])[0].lower())[:40] or "take"
        try:
            joints, frames = parse(body)
            if not frames:
                return 400, json.dumps({"error": "the take has no frames"})
            text, ticks, seconds = to_bvh(joints, frames)
        except (ValueError, IndexError) as problem:
            return 400, json.dumps({"error": f"couldn't read the take: {problem}"})
        CAPTURES.mkdir(exist_ok=True)
        target = CAPTURES / f"{name}.bvh"
        target.write_text(text, encoding="utf-8")
        print(f"  [mocap] saved {target} ({len(joints)} joints, {ticks} frames, {seconds:.1f} s)", flush=True)
        return 200, json.dumps({"file": f"captures/{name}.bvh", "frames": ticks, "seconds": round(seconds, 1)})
    return 404, json.dumps({"error": f"nothing at {method} {path}"})

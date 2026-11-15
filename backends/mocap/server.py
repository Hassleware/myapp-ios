"""The mocap game's backend: turns takes recorded on the phone into .bvh files for Blender.

Two kinds of take arrive from the phone:
  * POST /take?name=walk   ARKit's own skeleton, converted here at once to captures/walk.bvh
  * POST /video?name=walk  the take's video. SAM 3D Body (Meta's stronger model, on this PC's graphics card, in WSL)
                           turns it into captures/walk-sam3d.bvh in the background. The phone asks GET /job?name=walk for
                           progress and fetches GET /result?name=walk (joint positions) to replay it.
SAM 3D Body is installed by tools/sam3d/install.sh; tools/sam3d/run_take.py is what runs for each video.

The phone sends   POST /take?name=walk   with the recording as text (see ios/bodytrack.mm for the format).
This writes      captures/walk.bvh   in the repo (git ignores the folder) and answers with where it went.

In Blender: File > Import > Motion Capture (.bvh), scale 1 (one unit is one metre), forward -Z, up Y.
Then retarget onto your own armature (for example with Rokoko's free Blender add-on).
Change anything here and save: the next take uses the new code, with no app build.
"""

import json
import math
import re
import subprocess
import sys
import threading
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CAPTURES = REPO / "captures"
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


# ---------------------------------------------------------------- GVHMR jobs

def wsl_path(path):
    """C:/Users/... as WSL sees it: /mnt/c/Users/..."""
    p = Path(path).resolve().as_posix()
    return "/mnt/" + p[0].lower() + p[2:]


def set_status(name, **fields):
    (CAPTURES / f"{name}.job.json").write_text(json.dumps(fields))


def start_worker():
    """SAM 3D Body in WSL, loaded once and kept for the next takes (tools/sam3d/run_take.py serve). It quits by
    itself after 10 idle minutes and is started again when needed. Kept on sys so a reload of this file reuses it."""
    worker = getattr(sys, "_gamelab_mocap_worker", None)
    if worker is None or worker.poll() is not None:
        command = ("source ~/miniforge3/etc/profile.d/conda.sh && conda activate sam3d && cd ~/sam-3d-body && "
                   f"python '{wsl_path(REPO / 'tools/sam3d/run_take.py')}' serve")
        worker = subprocess.Popen(["wsl", "-d", "Ubuntu", "--", "bash", "-lc", command], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                  errors="replace", bufsize=1)
        sys._gamelab_mocap_worker = worker
    return worker


def run_on_worker(name, upright, hands):
    """Hands one take to the worker and follows its STAGE lines until JOB DONE. One take at a time."""
    lock = sys.__dict__.setdefault("_gamelab_mocap_lock", threading.Lock())
    with lock:
        for attempt in range(2):   # a worker that idled out just as the take arrived is started again, once
            worker, tail, started = start_worker(), [], False
            try:
                worker.stdin.write(f"{wsl_path(upright)}\t{wsl_path(CAPTURES / name)}\t{'hands' if hands else ''}\n")
                worker.stdin.flush()
            except OSError:
                continue
            for line in worker.stdout:
                tail = (tail + [line.rstrip()])[-25:]
                if line.startswith("STAGE "):
                    started = True
                    set_status(name, state="running", stage=line[6:].strip())
                    if not line.startswith("STAGE frame") or "1 of" in line[:12]:
                        print(f"  [mocap] {name}: {line[6:].strip()}", flush=True)
                elif line.startswith("JOB DONE"):
                    return
                elif line.startswith("JOB FAILED"):
                    raise RuntimeError("SAM 3D Body stopped:\n" + "\n".join(tail))
                elif line.startswith("WORKER IDLE") and not started:
                    break
            if started:
                raise RuntimeError("SAM 3D Body's worker stopped:\n" + "\n".join(tail))
        raise RuntimeError("couldn't start SAM 3D Body in WSL:\n" + "\n".join(tail))


def run_job(name, hands=False):
    """Runs in its own thread: ffmpeg makes the video upright at 30 frames a second, then SAM 3D Body runs on it."""
    raw, upright = CAPTURES / f"{name}.mov", CAPTURES / f"{name}.mp4"
    try:
        set_status(name, state="running", stage="preparing the video")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(raw), "-r", "30", "-c:v", "libx264", "-crf", "18",
                        "-pix_fmt", "yuv420p", str(upright)], check=True, capture_output=True)
        set_status(name, state="running", stage="waiting for the PC's model")
        run_on_worker(name, upright, hands)
        set_status(name, state="done", stage="done", file=f"captures/{name}-sam3d.bvh")
        print(f"  [mocap] saved captures/{name}-sam3d.bvh", flush=True)
    except Exception as problem:
        detail = getattr(problem, "stderr", b"") or b""
        message = str(problem) + ("\n" + detail.decode(errors="replace") if detail else "")
        set_status(name, state="failed", stage=message.strip().splitlines()[-1][:200], detail=message[-4000:])
        print(f"  [mocap] {name} FAILED:\n{message}", flush=True)


def take_name(query):
    return re.sub(r"[^a-z0-9-]", "", (query.get("name") or ["take"])[0].lower())[:40] or "take"


def handle(method, path, query, body, headers):
    if method == "POST" and path == "/video":
        name = take_name(query)
        CAPTURES.mkdir(exist_ok=True)
        (CAPTURES / f"{name}.mov").write_bytes(body.encode("utf-8") if isinstance(body, str) else body)
        set_status(name, state="queued", stage="queued")
        hands = (query.get("hands") or ["0"])[0] == "1"   # the phone's HANDS button: finer fingers, four times slower
        threading.Thread(target=run_job, args=(name, hands), daemon=True).start()
        return 202, json.dumps({"ok": True})
    if method == "GET" and path == "/job":
        status = CAPTURES / f"{take_name(query)}.job.json"
        return (200, status.read_text()) if status.exists() else (404, json.dumps({"error": "no such job"}))
    if method == "GET" and path == "/result":
        result = CAPTURES / f"{take_name(query)}-sam3d.txt"
        return (200, result.read_text(), "text/plain") if result.exists() else (404, json.dumps({"error": "not ready"}))

    if method == "POST" and path == "/take":
        name = take_name(query)
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

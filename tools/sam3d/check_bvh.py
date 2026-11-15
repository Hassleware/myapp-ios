"""Checks a mocap .bvh against the joint positions it was made from: rebuilds every joint's position from the BVH
(as Blender does when it imports it) and reports the largest difference.

    python check_bvh.py take-sam3d.bvh take-sam3d.txt
"""

import sys

import numpy as np
from scipy.spatial.transform import Rotation


def read_bvh(path):
    lines = open(path).read().split("\n")
    parents, offsets, stack, i = [], [], [], 0
    while not lines[i].startswith("MOTION"):
        words = lines[i].split()
        if words and words[0] in ("ROOT", "JOINT"):
            parents.append(stack[-1] if stack else -1)
            stack.append(len(parents) - 1)
        elif words and words[0] == "End":
            stack.append(None)
        elif words and words[0] == "OFFSET" and stack and stack[-1] is not None and len(offsets) < len(parents):
            offsets.append([float(v) for v in words[1:4]])
        elif words and words[0] == "}":
            stack.pop()
        i += 1
    motion = np.array([[float(v) for v in l.split()] for l in lines[i + 3:] if l.strip()])
    return parents, np.array(offsets), motion


def positions(parents, offsets, motion):
    frames, count = len(motion), len(parents)
    out = np.zeros((frames, count, 3))
    for f in range(frames):
        world_rot = [None] * count
        for j in range(count):   # joints are written parent first, so parents are always done
            angles = motion[f, 3 + 3 * j: 6 + 3 * j]
            local = Rotation.from_euler("ZXY", angles, degrees=True).as_matrix()
            if parents[j] < 0:
                world_rot[j], out[f, j] = local, motion[f, :3]
            else:
                p = parents[j]
                world_rot[j] = world_rot[p] @ local
                out[f, j] = out[f, p] + world_rot[p] @ offsets[j]
    return out


if __name__ == "__main__":
    parents, offsets, motion = read_bvh(sys.argv[1])
    rebuilt = positions(parents, offsets, motion)
    lines = open(sys.argv[2]).read().split("\n")
    file_parents = [int(p) for p in lines[1].split()]
    expected = np.array([[float(v) for v in l.split()] for l in lines[2:] if l.strip()]).reshape(len(motion), -1, 3)
    # The BVH writes joints depth first; map them back to the file's order through the parent lists.
    order, kids = [], {}
    for j, p in enumerate(file_parents):
        kids.setdefault(p, []).append(j)
    def walk(j):
        order.append(j)
        for c in kids.get(j, []):
            walk(c)
    walk(kids[-1][0])
    error = np.linalg.norm(rebuilt - expected[:, order], axis=2)
    print(f"{len(parents)} joints, {len(motion)} frames: mean error {error.mean() * 1000:.1f} mm, worst {error.max() * 1000:.1f} mm")

"""Runs SAM 3D Body (Meta, 2026) on one take's video and writes what the mocap game needs. Runs inside WSL, in the
sam3d environment, from ~/sam-3d-body (backends/mocap/server.py starts it):

    python run_take.py <video.mp4> <output prefix>

Writes <prefix>-sam3d.bvh (the motion on the MHR skeleton, for Blender) and <prefix>-sam3d.txt (joint positions for
the phone's replay). Prints "STAGE ..." lines as it goes.

SAM 3D Body works on single images, so this adds what a video needs: YOLO finds the person in each frame (the
biggest person nearest last frame's), the iPhone 11's lens sets the camera (26 mm equivalent), and a short moving
average over time steadies the result. The camera is assumed not to move.
"""

import sys
from pathlib import Path

import cv2
import numpy as np
import torch

CHECKPOINT = "checkpoints/sam-3d-body-dinov3/model.ckpt"
MHR = "checkpoints/sam-3d-body-dinov3/assets/mhr_model.pt"
FOCAL_MM = 26          # the iPhone 11's wide camera, as a full-frame equivalent
SMOOTH = 5             # frames in the moving average (odd); 1 turns smoothing off
FLIP = np.diag([1.0, -1.0, -1.0])   # camera space (y down, z ahead) to y up, facing the camera


def stage(text):
    print("STAGE " + text, flush=True)


def skeleton_parents(model):
    """The MHR skeleton's parent of each joint, from the rig inside mhr_model.pt."""
    mhr = model.head_pose.mhr
    for path in ("character_torch.skeleton.joint_parents", "character.skeleton.joint_parents", "skeleton.joint_parents"):
        obj = mhr
        try:
            for part in path.split("."):
                obj = getattr(obj, part)
            return [int(p) for p in (obj.tolist() if hasattr(obj, "tolist") else obj)]
        except AttributeError:
            continue
    raise RuntimeError("couldn't find the MHR skeleton's joint parents in mhr_model.pt")


def joint_names(model, count):
    mhr = model.head_pose.mhr
    for path in ("character_torch.skeleton.joint_names", "character.skeleton.joint_names", "skeleton.joint_names"):
        obj = mhr
        try:
            for part in path.split("."):
                obj = getattr(obj, part)
            return [str(n) for n in obj]
        except AttributeError:
            continue
    return [f"joint_{i}" for i in range(count)]


def smooth(values):
    """Centred moving average over time (axis 0), shrinking at the ends."""
    if SMOOTH <= 1 or len(values) < 3:
        return values
    half, out = SMOOTH // 2, np.empty_like(values)
    for t in range(len(values)):
        out[t] = values[max(0, t - half): t + half + 1].mean(axis=0)
    return out


def orthonormal(rotations):
    """Nearest true rotations to (averaged) matrices."""
    u, _, vt = np.linalg.svd(rotations)
    fixed = u @ vt
    bad = np.linalg.det(fixed) < 0
    u[bad, :, -1] *= -1
    return u @ vt


def main():
    video, prefix = sys.argv[1], sys.argv[2]
    sys.path.insert(0, ".")   # SAM 3D Body's own code, from the folder this runs in
    stage("loading SAM 3D Body")
    from sam_3d_body import SAM3DBodyEstimator, load_sam_3d_body
    from ultralytics import YOLO

    model, cfg = load_sam_3d_body(CHECKPOINT, device="cuda", mhr_path=MHR)
    estimator = SAM3DBodyEstimator(sam_3d_body_model=model, model_cfg=cfg)
    detector = YOLO("checkpoints/yolov8x.pt")

    reader = cv2.VideoCapture(video)
    fps = reader.get(cv2.CAP_PROP_FPS) or 30
    total = int(reader.get(cv2.CAP_PROP_FRAME_COUNT))
    positions, rotations, previous = [], [], None
    frame = 0
    while True:
        ok, bgr = reader.read()
        if not ok:
            break
        frame += 1
        if frame % 15 == 1:
            stage(f"frame {frame} of {total}")
        height, width = bgr.shape[:2]
        # The person: the biggest box, or the one nearest last frame's once there is one.
        boxes = detector(bgr, classes=[0], verbose=False)[0].boxes.xyxy.cpu().numpy()
        if len(boxes) == 0:
            if positions:   # keep the last pose while the person is hidden
                positions.append(positions[-1]), rotations.append(rotations[-1])
            continue
        if previous is None:
            box = boxes[np.argmax((boxes[:, 2] - boxes[:, 0]) * (boxes[:, 3] - boxes[:, 1]))]
        else:
            box = boxes[np.argmin(np.abs(boxes - previous).sum(axis=1))]
        previous = box
        focal = FOCAL_MM / 43.27 * np.hypot(width, height)   # full-frame equivalent, measured on the diagonal
        cam = torch.tensor([[[focal, 0, width / 2], [0, focal, height / 2], [0, 0, 1]]], dtype=torch.float32)
        out = estimator.process_one_image(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB), bboxes=box[None], cam_int=cam)
        if not out:
            if positions:
                positions.append(positions[-1]), rotations.append(rotations[-1])
            continue
        person = out[0]
        joints = np.asarray(person["pred_joint_coords"], dtype=np.float64) + np.asarray(person["pred_cam_t"], dtype=np.float64)
        positions.append(joints @ FLIP.T)
        # SAM 3D Body flips its joint positions into camera space but leaves the rotations in the body model's own
        # y-up frame, so after flipping the positions back the rotations already match (checked: bones then stay
        # rigid to about 5 mm across frames, against 60 mm with any other reading).
        rotations.append(np.asarray(person["pred_global_rots"], dtype=np.float64))
    if not positions:
        raise RuntimeError("no person was found in the video")

    stage("smoothing and writing the BVH")
    positions = smooth(np.stack(positions))
    rotations = orthonormal(smooth(np.stack(rotations)))
    frames, count = positions.shape[:2]
    parents = skeleton_parents(model)[:count]
    names = joint_names(model, count)

    # Feet on the floor (y = 0) and the take starting at the middle of the room.
    shift = np.array([positions[0, 0, 0], positions[:, :, 1].min(), positions[0, 0, 2]])
    positions = positions - shift
    write_bvh(prefix + "-sam3d.bvh", positions, rotations, parents, names, fps)

    # The phone's replay: every other frame at centimetre precision keeps a minute's take under 2 MB.
    text = [f"fps {round(fps / 2)} joints {count}", " ".join(str(p) for p in parents)]
    text += [" ".join(f"{v:.2f}" for v in positions[f].reshape(-1)) for f in range(0, frames, 2)]
    Path(prefix + "-sam3d.txt").write_text("\n".join(text) + "\n")
    stage("done")


def write_bvh(path, positions, rotations, parents, names, fps):
    from scipy.spatial.transform import Rotation

    frames, count = positions.shape[:2]
    roots = [i for i in range(count) if parents[i] < 0]
    root = roots[0]
    children = {i: [c for c in range(count) if parents[c] == i] for i in range(count)}
    # Each bone's offset in its parent's frame is fixed for a rigid skeleton; take the median over the take.
    offsets = np.zeros((count, 3))
    for j in range(count):
        p = parents[j]
        if p >= 0:
            local = np.einsum("fji,fj->fi", rotations[:, p], positions[:, j] - positions[:, p])
            offsets[j] = np.median(local, axis=0)

    lines, order = ["HIERARCHY"], []

    def write(i, depth):
        pad = "  " * depth
        order.append(i)
        name = "".join(c if c.isalnum() or c == "_" else "_" for c in names[i]) or f"joint_{i}"
        lines.append(f"{pad}{'ROOT' if i == root else 'JOINT'} {name}")
        lines.append(pad + "{")
        o = np.zeros(3) if i == root else offsets[i]
        lines.append(f"{pad}  OFFSET {o[0]:.5f} {o[1]:.5f} {o[2]:.5f}")
        lines.append(f"{pad}  CHANNELS {'6 Xposition Yposition Zposition ' if i == root else '3 '}Zrotation Xrotation Yrotation")
        for c in children[i]:
            write(c, depth + 1)
        if not children[i]:
            lines.extend([pad + "  End Site", pad + "  {", pad + "    OFFSET 0 0.02 0", pad + "  }"])
        lines.append(pad + "}")

    write(root, 0)
    local = rotations.copy()
    for j in range(count):
        if parents[j] >= 0:
            local[:, j] = np.einsum("fji,fjk->fik", rotations[:, parents[j]], rotations[:, j])
    euler = Rotation.from_matrix(local.reshape(-1, 3, 3)).as_euler("ZXY", degrees=True).reshape(frames, count, 3)
    lines += ["MOTION", f"Frames: {frames}", f"Frame Time: {1 / fps:.6f}"]
    for f in range(frames):
        values = list(positions[f, root])
        for i in order:
            values += list(euler[f, i])
        lines.append(" ".join(f"{v:.4f}" for v in values))
    Path(path).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()

"""Runs GVHMR on one take's video and writes what the mocap game needs. Runs inside WSL, in the gvhmr environment,
from the GVHMR folder (backends/mocap/server.py starts it):

    python run_take.py <video.mp4> <output prefix>

Writes <prefix>-gvhmr.bvh (the motion, for Blender) and <prefix>-gvhmr.txt (joint positions, for the phone's replay).
Prints "STAGE ..." lines as it goes, so the phone can show progress.

It reuses GVHMR's own demo pipeline (tools/demo/demo.py) but skips its video renders, which take longer than the
motion itself. The camera is assumed fixed (-s), with the iPhone 11's 26 mm-equivalent wide lens.
"""

import importlib.util
import sys
from pathlib import Path

import numpy as np
import torch


def stage(text):
    print("STAGE " + text, flush=True)


def main():
    video, prefix = sys.argv[1], sys.argv[2]
    sys.path.insert(0, ".")   # GVHMR's own code (hmr4d), from the GVHMR folder this runs in
    stage("loading GVHMR")
    spec = importlib.util.spec_from_file_location("gvhmr_demo", "tools/demo/demo.py")
    demo = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(demo)

    sys.argv = ["demo.py", f"--video={video}", "-s", "--output_root=outputs/gamelab", "--f_mm=26"]
    cfg = demo.parse_args_to_cfg()
    stage("finding the person and their 2D pose (YOLO, ViTPose)")
    demo.run_preprocess(cfg)
    data = demo.load_data_dict(cfg)

    if not Path(cfg.paths.hmr4d_results).exists():
        stage("estimating 3D motion (GVHMR)")
        import hydra

        model = hydra.utils.instantiate(cfg.model, _recursive_=False)
        model.load_pretrained_model(cfg.ckpt_path)
        model = model.eval().cuda()
        with torch.no_grad():
            pred = demo.detach_to_cpu(model.predict(data, static_cam=cfg.static_cam))
        torch.save(pred, cfg.paths.hmr4d_results)
    pred = torch.load(cfg.paths.hmr4d_results, weights_only=False)

    stage("writing the BVH")
    export(pred["smpl_params_global"], prefix)
    stage("done")


def export(params, prefix):
    import smplx
    from scipy.spatial.transform import Rotation

    body_pose = params["body_pose"].float()          # (L, 63): 21 joints, each turned from its parent (axis-angle)
    global_orient = params["global_orient"].float()  # (L, 3)
    transl = params["transl"].float()                # (L, 3)
    betas = params["betas"].float()                  # (L, 10): body shape
    frames = body_pose.shape[0]

    model = smplx.create("inputs/checkpoints/body_models", model_type="smplx", gender="neutral", ext="npz",
                         num_betas=10, use_pca=False, batch_size=frames)
    with torch.no_grad():
        posed = model(betas=betas, body_pose=body_pose, global_orient=global_orient, transl=transl).joints[:, :22].numpy()
        rest_model = smplx.create("inputs/checkpoints/body_models", model_type="smplx", gender="neutral", ext="npz",
                                  num_betas=10, use_pca=False, batch_size=1)
        rest = rest_model(betas=betas[:1]).joints[0, :22].numpy()
    parents = model.parents[:22].tolist()
    parents[0] = -1
    names = ["pelvis", "left_hip", "right_hip", "spine1", "left_knee", "right_knee", "spine2", "left_ankle", "right_ankle",
             "spine3", "left_foot", "right_foot", "neck", "left_collar", "right_collar", "head", "left_shoulder",
             "right_shoulder", "left_elbow", "right_elbow", "left_wrist", "right_wrist"]

    # Feet on the floor (y = 0) and the take starting at the middle of the room.
    shift = np.array([posed[0, 0, 0], posed[:, :, 1].min(), posed[0, 0, 2]])
    posed = posed - shift

    # ---- BVH: one ROOT (pelvis, with position) and 21 JOINTs, ZXY rotations like the ARKit takes.
    children = {i: [c for c in range(22) if parents[c] == i] for i in range(22)}
    lines, order = ["HIERARCHY"], []

    def write(i, depth):
        pad = "  " * depth
        order.append(i)
        offset = np.zeros(3) if parents[i] < 0 else rest[i] - rest[parents[i]]
        lines.append(f"{pad}{'ROOT' if parents[i] < 0 else 'JOINT'} {names[i]}")
        lines.append(pad + "{")
        lines.append(f"{pad}  OFFSET {offset[0]:.5f} {offset[1]:.5f} {offset[2]:.5f}")
        lines.append(f"{pad}  CHANNELS {'6 Xposition Yposition Zposition ' if parents[i] < 0 else '3 '}Zrotation Xrotation Yrotation")
        for c in children[i]:
            write(c, depth + 1)
        if not children[i]:
            lines += [pad + "  End Site", pad + "  {", pad + "    OFFSET 0 0.05 0", pad + "  }"]
        lines.append(pad + "}")

    write(0, 0)
    rotations = np.concatenate([global_orient.numpy()[:, None], body_pose.numpy().reshape(frames, 21, 3)], axis=1)
    euler = Rotation.from_rotvec(rotations.reshape(-1, 3)).as_euler("ZXY", degrees=True).reshape(frames, 22, 3)
    lines += ["MOTION", f"Frames: {frames}", "Frame Time: 0.033333"]
    for f in range(frames):
        values = list(posed[f, 0])
        for i in order:
            values += list(euler[f, i])
        lines.append(" ".join(f"{v:.4f}" for v in values))
    Path(prefix + "-gvhmr.bvh").write_text("\n".join(lines) + "\n")

    # ---- For the phone: frames per second and joint count, the parents, then one line of x y z per joint per frame.
    text = [f"fps 30 joints 22", " ".join(str(p) for p in parents)]
    text += [" ".join(f"{v:.3f}" for v in posed[f].reshape(-1)) for f in range(frames)]
    Path(prefix + "-gvhmr.txt").write_text("\n".join(text) + "\n")


if __name__ == "__main__":
    main()

"""Imports a mocap take into Blender with the right settings: one unit a metre, forward -Z, up Y, its own frame rate.

In Blender:  Scripting tab > Open this file > Run Script. It imports the newest .bvh in the repo's captures folder.
From a terminal (also how it is tested):
    blender --background --python tools/blender_import_take.py -- captures/take-3-sam3d.bvh [save.blend]
"""

import sys
from pathlib import Path

import bpy

CAPTURES = Path(__file__).resolve().parent.parent / "captures"


def import_take(path):
    text = Path(path).read_text().split("MOTION")[1].split("\n")
    frames = int(text[1].split()[1])
    fps = round(1 / float(text[2].split()[2]))
    bpy.context.scene.render.fps = fps
    bpy.ops.import_anim.bvh(filepath=str(path), global_scale=1.0, axis_forward="-Z", axis_up="Y",
                            update_scene_fps=False, update_scene_duration=True, rotate_mode="NATIVE")
    armature = bpy.context.object
    bpy.context.scene.frame_end = frames
    print(f"imported {Path(path).name}: armature '{armature.name}' with {len(armature.data.bones)} bones, "
          f"{frames} frames at {fps} fps")
    return armature


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    take = Path(args[0]) if args else max(CAPTURES.glob("*.bvh"), key=lambda p: p.stat().st_mtime)
    armature = import_take(take)
    if len(args) > 1:
        # A check for tests: where the feet and head are on the last frame, then save.
        scene = bpy.context.scene
        scene.frame_set(scene.frame_end)
        for name in ("l_ball", "r_ball", "head"):
            bone = armature.pose.bones.get(name)
            if bone:
                print(f"  {name}: {tuple(round(v, 3) for v in armature.matrix_world @ bone.head)}")
        bpy.ops.wm.save_as_mainfile(filepath=str(Path(args[1]).resolve()))

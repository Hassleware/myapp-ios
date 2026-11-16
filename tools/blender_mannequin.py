"""A wooden artist's mannequin (egg head, wooden segments, ball joints, screw heads) on a 22-bone humanoid rig,
and the retargeting that puts a mocap take onto it.

    blender --background --python tools/blender_mannequin.py -- captures/take-1-sam3d.npz games/mannequin/mannequin.glb
        builds the mannequin, retargets the take onto it and exports a .glb with the animation (named after the take)
    blender --background --python tools/blender_mannequin.py -- - games/mannequin/mannequin.glb
        just the mannequin, standing in its T-pose

In Blender's Scripting tab, Run Script builds the mannequin and retargets the newest take in captures/.

The mannequin's parts are rigid: every vertex belongs fully to one bone, like the real thing, so nothing stretches.
Retargeting (see retarget): each mannequin bone follows one joint of the take. Its rotation is the take joint's
rotation, plus a fixed correction worked out on the take's first frame by turning the mannequin's bone to point
the same way as the take's. That makes it independent of how either skeleton happens to stand at rest.
"""

import math
import sys
from pathlib import Path

import bpy
import bmesh
import numpy as np
from mathutils import Matrix, Quaternion, Vector

# Bone: (head, tail, parent, take joint whose rotation it follows, take joints whose line gives its direction).
# Blender's axes: z up, the mannequin faces -y, its left side is +x. Metres; about 1.82 m tall.
BONES = {
    "hips":       ((0, 0, 1.00), (0, 0, 1.10), None, "root", ("root", "c_spine1")),
    "spine":      ((0, 0, 1.10), (0, 0, 1.30), "hips", "c_spine1", ("c_spine0", "c_spine2")),
    "chest":      ((0, 0, 1.30), (0, 0, 1.50), "spine", "c_spine3", ("c_spine2", "c_neck")),
    "neck":       ((0, 0, 1.50), (0, 0, 1.60), "chest", "c_neck", ("c_neck", "c_head")),
    "head":       ((0, 0, 1.60), (0, 0, 1.82), "neck", "c_head", ("c_head", "c_head_null")),
}
for side, sx in (("L", 1), ("R", -1)):
    s = side.lower()
    BONES.update({
        f"shoulder.{side}":  ((0.03 * sx, 0, 1.46), (0.18 * sx, 0, 1.46), "chest", f"{s}_clavicle", (f"{s}_clavicle", f"{s}_uparm")),
        f"upper_arm.{side}": ((0.18 * sx, 0, 1.46), (0.46 * sx, 0, 1.46), f"shoulder.{side}", f"{s}_uparm", (f"{s}_uparm", f"{s}_lowarm")),
        f"forearm.{side}":   ((0.46 * sx, 0, 1.46), (0.71 * sx, 0, 1.46), f"upper_arm.{side}", f"{s}_lowarm", (f"{s}_lowarm", f"{s}_wrist")),
        f"hand.{side}":      ((0.71 * sx, 0, 1.46), (0.87 * sx, 0, 1.46), f"forearm.{side}", f"{s}_wrist", (f"{s}_wrist", f"{s}_middle1")),
        f"thigh.{side}":     ((0.10 * sx, 0, 1.00), (0.10 * sx, 0, 0.55), "hips", f"{s}_upleg", (f"{s}_upleg", f"{s}_lowleg")),
        f"shin.{side}":      ((0.10 * sx, 0, 0.55), (0.10 * sx, 0, 0.10), f"thigh.{side}", f"{s}_lowleg", (f"{s}_lowleg", f"{s}_foot")),
        f"foot.{side}":      ((0.10 * sx, 0, 0.10), (0.10 * sx, -0.16, 0.03), f"shin.{side}", f"{s}_foot", (f"{s}_foot", f"{s}_ball")),
    })

WOOD = (0.80, 0.62, 0.42, 1)      # beech
JOINT_WOOD = (0.66, 0.47, 0.30, 1)
METAL = (0.55, 0.56, 0.58, 1)


def material(name, color, metallic=0.0, roughness=0.55):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    shader = m.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = color
    shader.inputs["Metallic"].default_value = metallic
    shader.inputs["Roughness"].default_value = roughness
    m.diffuse_color = color   # what the solid viewport shows
    return m


# ---------------------------------------------------------------- the mesh, part by part

PARTS = []   # (bmesh geometry maker, bone, material index)


def add(bm, bone, slot, verts):
    layer = bm.verts.layers.deform.active   # made once in build(): adding a layer invalidates vertices in hand
    for v in verts:
        v[layer][GROUPS[bone]] = 1.0
    for f in {f for v in verts for f in v.link_faces}:
        f.material_index = slot
        f.smooth = True


def ellipsoid(bm, bone, slot, centre, size, segments=24):
    made = bmesh.ops.create_uvsphere(bm, u_segments=segments, v_segments=segments // 2, radius=1.0)["verts"]
    bmesh.ops.scale(bm, vec=size, verts=made)
    bmesh.ops.translate(bm, vec=centre, verts=made)
    add(bm, bone, slot, made)


def ball(bm, bone, centre, radius):
    ellipsoid(bm, bone, 1, centre, (radius, radius, radius), segments=16)


def limb(bm, bone, start, end, r1, r2, slot=0):
    """A tapered wooden segment from start to end, rounded at both ends by its neighbouring ball joints."""
    a, b = Vector(start), Vector(end)
    made = bmesh.ops.create_cone(bm, cap_ends=True, segments=16, radius1=r1, radius2=r2, depth=(b - a).length)["verts"]
    bmesh.ops.transform(bm, matrix=(b - a).to_track_quat("Z", "Y").to_matrix().to_4x4(), verts=made)
    bmesh.ops.translate(bm, vec=(a + b) / 2, verts=made)
    add(bm, bone, slot, made)


def screw(bm, bone, centre, normal, radius=0.012):
    """A screw head: a flat round head with a slot cut across it, sitting on a joint."""
    n = Vector(normal).normalized()
    turn = n.to_track_quat("Z", "Y").to_matrix().to_4x4()
    head = bmesh.ops.create_cone(bm, cap_ends=True, segments=12, radius1=radius, radius2=radius * 0.85, depth=0.006)["verts"]
    bmesh.ops.transform(bm, matrix=turn, verts=head)
    bmesh.ops.translate(bm, vec=Vector(centre) + n * 0.003, verts=head)
    add(bm, bone, 2, head)
    slot = bmesh.ops.create_cube(bm, size=1.0)["verts"]
    bmesh.ops.scale(bm, vec=(radius * 1.7, radius * 0.25, 0.002), verts=slot)
    bmesh.ops.transform(bm, matrix=turn, verts=slot)
    bmesh.ops.translate(bm, vec=Vector(centre) + n * 0.0065, verts=slot)
    add(bm, bone, 1, slot)


GROUPS = {}


def build():
    """Makes the 'mannequin' armature and its mesh, standing in a T-pose at the origin. Returns the armature."""
    for name in ("mannequin", "mannequin body"):
        if name in bpy.data.objects:
            bpy.data.objects.remove(bpy.data.objects[name])

    armature_data = bpy.data.armatures.new("mannequin")
    armature = bpy.data.objects.new("mannequin", armature_data)
    bpy.context.scene.collection.objects.link(armature)
    bpy.context.view_layer.objects.active = armature
    bpy.ops.object.mode_set(mode="EDIT")
    for name, (head, tail, parent, _, _) in BONES.items():
        bone = armature_data.edit_bones.new(name)
        bone.head, bone.tail = head, tail
        bone.roll = 0
        if parent:
            bone.parent = armature_data.edit_bones[parent]
            bone.use_connect = Vector(bone.parent.tail) == Vector(head)
    bpy.ops.object.mode_set(mode="OBJECT")

    mesh = bpy.data.meshes.new("mannequin body")
    body = bpy.data.objects.new("mannequin body", mesh)
    bpy.context.scene.collection.objects.link(body)
    for i, name in enumerate(BONES):
        GROUPS[name] = body.vertex_groups.new(name=name).index
    for m in (material("wood", WOOD), material("joint wood", JOINT_WOOD, roughness=0.45), material("screw", METAL, 1.0, 0.35)):
        mesh.materials.append(m)

    bm = bmesh.new()
    bm.verts.layers.deform.verify()
    # Head and neck
    ellipsoid(bm, "head", 0, (0, 0, 1.715), (0.085, 0.095, 0.115))
    limb(bm, "neck", (0, 0, 1.52), (0, 0, 1.62), 0.028, 0.026)
    ball(bm, "chest", (0, 0, 1.505), 0.034)
    # Torso: chest, waist ball, pelvis
    ellipsoid(bm, "chest", 0, (0, 0, 1.36), (0.16, 0.095, 0.15))
    ball(bm, "spine", (0, 0, 1.19), 0.065)
    limb(bm, "spine", (0, 0, 1.12), (0, 0, 1.24), 0.05, 0.055)
    ellipsoid(bm, "hips", 0, (0, 0, 1.03), (0.15, 0.095, 0.095))
    for side, sx in (("L", 1), ("R", -1)):
        # Arm
        ball(bm, f"upper_arm.{side}", (0.185 * sx, 0, 1.46), 0.045)
        screw(bm, f"upper_arm.{side}", (0.23 * sx, 0, 1.46), (sx, 0, 0))
        limb(bm, f"upper_arm.{side}", (0.215 * sx, 0, 1.46), (0.44 * sx, 0, 1.46), 0.036, 0.031)
        ball(bm, f"forearm.{side}", (0.46 * sx, 0, 1.46), 0.033)
        screw(bm, f"forearm.{side}", (0.46 * sx, 0.033, 1.46), (0, 1, 0), 0.01)
        screw(bm, f"forearm.{side}", (0.46 * sx, -0.033, 1.46), (0, -1, 0), 0.01)
        limb(bm, f"forearm.{side}", (0.485 * sx, 0, 1.46), (0.695 * sx, 0, 1.46), 0.03, 0.024)
        ball(bm, f"hand.{side}", (0.71 * sx, 0, 1.46), 0.024)
        ellipsoid(bm, f"hand.{side}", 0, (0.79 * sx, 0, 1.455), (0.075, 0.018, 0.045))   # a flat mitten hand
        # Leg
        ball(bm, f"thigh.{side}", (0.10 * sx, 0, 0.985), 0.058)
        screw(bm, f"thigh.{side}", (0.158 * sx, 0, 0.985), (sx, 0, 0))
        limb(bm, f"thigh.{side}", (0.10 * sx, 0, 0.95), (0.10 * sx, 0, 0.58), 0.058, 0.044)
        ball(bm, f"shin.{side}", (0.10 * sx, 0, 0.55), 0.044)
        screw(bm, f"shin.{side}", (0.144 * sx, 0, 0.55), (sx, 0, 0))
        screw(bm, f"shin.{side}", (0.056 * sx, 0, 0.55), (-sx, 0, 0))
        limb(bm, f"shin.{side}", (0.10 * sx, 0, 0.52), (0.10 * sx, 0, 0.125), 0.042, 0.032)
        ball(bm, f"foot.{side}", (0.10 * sx, 0, 0.10), 0.033)
        foot = bmesh.ops.create_cube(bm, size=1.0)["verts"]
        bmesh.ops.scale(bm, vec=(0.075, 0.2, 0.055), verts=foot)
        bmesh.ops.translate(bm, vec=(0.10 * sx, -0.06, 0.035), verts=foot)
        add(bm, f"foot.{side}", 0, foot)
        for f in {f for v in foot for f in v.link_faces}:
            f.smooth = False
    bm.to_mesh(mesh)
    bm.free()

    body.parent = armature
    modifier = body.modifiers.new("armature", "ARMATURE")
    modifier.object = armature
    armature.data.display_type = "STICK"
    return armature


# ---------------------------------------------------------------- retargeting

TO_BLENDER = Matrix(((1, 0, 0), (0, 0, -1), (0, 1, 0)))   # the take's y up to Blender's z up


def retarget(armature, npz_path, name=None):
    """Keys the take onto the mannequin as an action named after the take."""
    take = np.load(npz_path)
    names = [str(n) for n in take["names"]]
    joint = {n: i for i, n in enumerate(names)}
    positions, rotations, fps = take["positions"], take["rotations"], float(take["fps"])
    frames = len(positions)
    A = np.array(TO_BLENDER)
    pos = positions @ A.T                                     # (F, J, 3) in Blender's axes
    rot = np.einsum("ab,fjbc,dc->fjad", A, rotations, A)      # A R A^T

    # Root height: the mannequin's hips stand at 1.00 m; scale the take's travel to match.
    root = joint["root"]
    scale = 1.0 / max(float(pos[0, root, 2]), 0.3)

    rest = {b.name: b.matrix_local.to_3x3() for b in armature.data.bones}
    correction = {}
    for bone, (head, tail, _, follow, (a, b)) in BONES.items():
        rest_dir = (Vector(tail) - Vector(head)).normalized()
        take_dir = Vector(pos[0, joint[b]] - pos[0, joint[a]]).normalized()
        first = rest_dir.rotation_difference(take_dir).to_matrix() @ rest[bone]   # the bone turned to match frame 1
        correction[bone] = Matrix(rot[0, joint[follow]].tolist()).transposed() @ first

    bpy.context.scene.render.fps = round(fps)
    action = bpy.data.actions.new(name or Path(npz_path).stem.replace("-sam3d", ""))
    armature.animation_data_create()
    armature.animation_data.action = action
    order = list(BONES)   # parents come before children in BONES
    previous = {}
    for f in range(frames):
        world = {bone: Matrix(rot[f, joint[BONES[bone][3]]].tolist()) @ correction[bone] for bone in order}
        for bone in order:
            parent = BONES[bone][2]
            if parent:
                expected = world[parent] @ rest[parent].transposed() @ rest[bone]   # where the bone would be unposed
            else:
                expected = rest[bone]
            q = (expected.transposed() @ world[bone]).to_quaternion()
            if bone in previous and previous[bone].dot(q) < 0:
                q.negate()   # the short way round, so keys don't spin
            previous[bone] = q
            pose = armature.pose.bones[bone]
            pose.rotation_mode = "QUATERNION"
            pose.rotation_quaternion = q
            pose.keyframe_insert("rotation_quaternion", frame=f + 1)
            if not parent:
                travel = Vector(pos[f, root] - pos[0, root]) * scale
                travel.z = (pos[f, root, 2] * scale) - 1.0
                pose.location = rest[bone].transposed() @ travel
                pose.keyframe_insert("location", frame=f + 1)
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = 1, frames
    print(f"retargeted {Path(npz_path).name}: {frames} frames at {fps:g} fps onto {len(order)} bones")
    return action


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    armature = build()
    take = args[0] if args else None
    if take is None:
        here = next((Path(bpy.path.abspath(t.filepath)) for t in bpy.data.texts if t.filepath.endswith("blender_mannequin.py")),
                    Path(globals().get("__file__", "tools/blender_mannequin.py")))
        found = [p for p in (here.resolve().parent.parent / "captures").glob("*-sam3d.npz") if not p.name.startswith("zz-")]
        take = str(max(found, key=lambda p: p.stat().st_mtime)) if found else "-"
    if take != "-":
        retarget(armature, take)
    if len(args) > 1:
        Path(args[1]).parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.export_scene.gltf(filepath=str(Path(args[1]).resolve()), export_format="GLB", export_animations=True,
                                  export_yup=True)
        print("exported", args[1])

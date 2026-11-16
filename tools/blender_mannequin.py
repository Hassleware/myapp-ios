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

WOOD_TINT = (1.0, 1.0, 1.0, 1)        # multiplies the grain texture
JOINT_TINT = (0.80, 0.72, 0.64, 1)    # ball joints: the same wood, a little darker, as on the real thing
METAL = (0.62, 0.62, 0.64, 1)
TILE = 0.12          # metres of wood one copy of the grain texture covers, along and around every part
GRAIN_LINES = 7      # grain lines across one copy: so about 1.7 cm apart on every part, whatever its size


def wood_images(size=1024, seed=7):
    """A beech-like grain, generated: straight lines along the length (v) with slow waves, finer streaks and a
    little noise, made only of whole-period waves so it tiles seamlessly. Returns (colour, normal map) images."""
    rng = np.random.default_rng(seed)
    u, v = np.meshgrid(np.arange(size) / size, np.arange(size) / size)

    def waves(count, max_u, max_v, amplitude):
        total = np.zeros_like(u)
        for _ in range(count):
            fu, fv = rng.integers(0, max_u + 1), rng.integers(1, max_v + 1)
            total += amplitude / (1 + fu + fv) * np.sin(2 * np.pi * (fu * u + fv * v) + rng.uniform(0, 2 * np.pi))
        return total

    # Long, gently curving lines: the wander only changes slowly along the grain (v) and is small.
    warp = waves(8, 3, 2, 0.22)
    lines = 0.5 + 0.5 * np.sin(2 * np.pi * (GRAIN_LINES * u + warp))
    late = lines ** 12                                                     # thin late-wood lines
    streaks = 0.5 + 0.5 * np.sin(2 * np.pi * (GRAIN_LINES * 6 * u + 2 * warp + waves(6, 2, 3, 0.15)))
    flecks = np.clip(waves(30, 60, 4, 1.0), 0, None) ** 2                  # beech's small rays, stretched along v
    dark = np.clip(0.32 * late + 0.10 * streaks ** 4 + 0.10 * flecks, 0, 1)[..., None]
    light, deep = np.array([0.86, 0.70, 0.50]), np.array([0.62, 0.43, 0.26])
    colour = light * (1 - dark) + deep * dark

    height = 0.6 * late + 0.15 * streaks ** 3
    gu = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * size / 2
    gv = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * size / 2
    strength = 0.0015                                                      # gentle: varnished wood is smooth
    normal = np.stack([-gu * strength, -gv * strength, np.ones_like(u)], -1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)

    images = []
    for name, rgb, data in (("wood grain", colour, False), ("wood grain normal", normal * 0.5 + 0.5, True)):
        image = bpy.data.images.get(name) or bpy.data.images.new(name, size, size)
        image.colorspace_settings.name = "Non-Color" if data else "sRGB"
        pixels = np.concatenate([rgb[::-1], np.ones((size, size, 1))], -1).astype(np.float32)   # Blender's rows go up
        image.pixels.foreach_set(pixels.ravel())
        image.pack()
        images.append(image)
    return images


def material(name, tint, metallic=0.0, roughness=0.5, images=None):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.use_nodes = True
    nodes, links = m.node_tree.nodes, m.node_tree.links
    shader = nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = tint
    shader.inputs["Metallic"].default_value = metallic
    shader.inputs["Roughness"].default_value = roughness
    m.diffuse_color = tint
    if images:
        colour = nodes.new("ShaderNodeTexImage")
        colour.image = images[0]
        if tint[:3] == (1.0, 1.0, 1.0):
            links.new(colour.outputs["Color"], shader.inputs["Base Color"])
        else:
            mix = nodes.new("ShaderNodeMix")   # texture times tint: the glTF exporter writes it as a base colour factor
            mix.data_type, mix.blend_type = "RGBA", "MULTIPLY"
            mix.inputs["Factor"].default_value = 1.0
            links.new(colour.outputs["Color"], mix.inputs[6])
            mix.inputs[7].default_value = tint
            links.new(mix.outputs[2], shader.inputs["Base Color"])
        bump = nodes.new("ShaderNodeTexImage")
        bump.image = images[1]
        normal_map = nodes.new("ShaderNodeNormalMap")
        links.new(bump.outputs["Color"], normal_map.inputs["Color"])
        links.new(normal_map.outputs["Normal"], shader.inputs["Normal"])
    return m


# ---------------------------------------------------------------- turned wooden parts

GROUPS = {}


def lathe(bm, bone, slot, start, end, profile, side=(1, 0, 0), rings=28, segments=32):
    """A part turned like wood on a lathe: profile(t) gives the (width, depth) half-sizes at t from 0 (start) to 1
    (end), and is zero at both ends so they close smoothly. The grain texture runs along the part, a whole number
    of copies round it, and a copy covers TILE metres either way, so small parts show few lines and big parts many."""
    a, b = Vector(start), Vector(end)
    axis = b - a
    length = axis.length
    z = axis / length
    side = Vector(side)
    if abs(z.dot(side.normalized())) > 0.9:   # side must cross the axis, or the part collapses to a line
        side = Vector((0, 0, 1)) if abs(z.z) < 0.9 else Vector((0, 1, 0))
    x = (side - z * z.dot(side)).normalized()
    y = z.cross(x)
    ts = [(1 - math.cos(math.pi * i / rings)) / 2 for i in range(rings + 1)]   # closer together at the rounded ends
    sizes = [profile(t) for t in ts]
    around = max(math.tau * math.sqrt((rx * rx + ry * ry) / 2) for rx, ry in sizes)
    copies = max(1, round(around / TILE))
    along = [0.0]
    for i in range(1, len(ts)):
        dr = (sum(sizes[i]) - sum(sizes[i - 1])) / 2
        along.append(along[-1] + math.hypot((ts[i] - ts[i - 1]) * length, dr))

    uv_layer = bm.loops.layers.uv.verify()
    deform = bm.verts.layers.deform.active
    rows = []
    for t, (rx, ry) in zip(ts, sizes):
        centre = a + axis * t
        if rx < 1e-6 and ry < 1e-6:
            rows.append(None)
            pole = bm.verts.new(centre)
            pole[deform][GROUPS[bone]] = 1.0
            rows[-1] = pole
            continue
        row = []
        for j in range(segments):
            angle = math.tau * j / segments
            vert = bm.verts.new(centre + x * (rx * math.cos(angle)) + y * (ry * math.sin(angle)))
            vert[deform][GROUPS[bone]] = 1.0
            row.append(vert)
        rows.append(row)

    def uv(j, i):
        return (copies * j / segments, along[i] / TILE)

    for i in range(len(rows) - 1):
        r0, r1 = rows[i], rows[i + 1]
        for j in range(segments):
            k = j + 1
            if isinstance(r0, list) and isinstance(r1, list):
                face = bm.faces.new((r0[j], r0[k % segments], r1[k % segments], r1[j]))
                uvs = (uv(j, i), uv(k, i), uv(k, i + 1), uv(j, i + 1))
            elif isinstance(r1, list):   # r0 is the start pole
                face = bm.faces.new((r0, r1[k % segments], r1[j]))
                uvs = (uv(j + 0.5, i), uv(k, i + 1), uv(j, i + 1))
            else:                        # r1 is the end pole
                face = bm.faces.new((r0[j], r0[k % segments], r1))
                uvs = (uv(j, i), uv(k, i), uv(j + 0.5, i + 1))
            for loop, coords in zip(face.loops, uvs):
                loop[uv_layer].uv = coords
            face.material_index = slot
            face.smooth = True


def capped(t, r_start, r_end, length, bulge=0.0):
    """A tapered rod with hemispherical ends: the radius at t, with a gentle swell in the middle (bulge)."""
    base = (r_start + (r_end - r_start) * t) * (1 + bulge * math.sin(math.pi * t))
    e0, e1 = r_start / length, r_end / length
    if t < e0:
        base *= math.sqrt(max(0.0, 1 - ((e0 - t) / e0) ** 2))
    elif t > 1 - e1:
        base *= math.sqrt(max(0.0, 1 - ((t - (1 - e1)) / e1) ** 2))
    return base


def rod(bm, bone, start, end, r_start, r_end, bulge=0.0, flat=1.0, side=(1, 0, 0), slot=0):
    length = (Vector(end) - Vector(start)).length
    lathe(bm, bone, slot, start, end, lambda t: (capped(t, r_start, r_end, length, bulge),
                                                 flat * capped(t, r_start, r_end, length, bulge)), side)


def ball(bm, bone, centre, radius):
    c = Vector(centre)
    lathe(bm, bone, 1, c - Vector((0, 0, radius)), c + Vector((0, 0, radius)),
          lambda t: (radius * math.sqrt(max(0.0, 1 - (2 * t - 1) ** 2)),) * 2, rings=16, segments=20)


def boxy(t, power):
    """1 in the middle, falling to 0 at the ends: a superellipse, squarer as power grows."""
    return max(0.0, 1 - abs(2 * t - 1) ** power) ** (1 / power)


def screw(bm, bone, centre, normal, radius=0.011):
    """A domed metal screw head with a slot across it."""
    n = Vector(normal).normalized()
    c = Vector(centre)
    lathe(bm, bone, 2, c - n * 0.001, c + n * 0.0045, lambda t: (radius * boxy(t, 3.0) if t < 0.5 else
                                                                  radius * math.sqrt(max(0.0, 1 - (2 * t - 1) ** 2)),) * 2,
          side=n.orthogonal(), rings=8, segments=16)
    turn = n.to_track_quat("Z", "Y").to_matrix().to_4x4()
    slot = bmesh.ops.create_cube(bm, size=1.0)["verts"]
    bmesh.ops.scale(bm, vec=(radius * 1.8, radius * 0.22, 0.0016), verts=slot)
    bmesh.ops.transform(bm, matrix=turn, verts=slot)
    bmesh.ops.translate(bm, vec=c + n * 0.0047, verts=slot)
    deform = bm.verts.layers.deform.active
    for v in slot:
        v[deform][GROUPS[bone]] = 1.0
    for f in {f for v in slot for f in v.link_faces}:
        f.material_index = 1


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
    for name in BONES:
        GROUPS[name] = body.vertex_groups.new(name=name).index
    grain = wood_images()
    for m in (material("wood", WOOD_TINT, roughness=0.45, images=grain),
              material("joint wood", JOINT_TINT, roughness=0.4, images=grain),
              material("screw", METAL, metallic=1.0, roughness=0.3)):
        mesh.materials.append(m)

    bm = bmesh.new()
    bm.verts.layers.deform.verify()
    bm.loops.layers.uv.verify()

    # Head: an egg, the cranium fuller than the chin, a touch deeper than wide.
    lathe(bm, "head", 0, (0, 0.006, 1.600), (0, -0.004, 1.838),
          lambda t: (0.079 * boxy(t, 2.0) * (1 + 0.15 * (2 * t - 1)), 0.093 * boxy(t, 2.0) * (1 + 0.12 * (2 * t - 1))))
    rod(bm, "neck", (0, 0, 1.495), (0, 0, 1.64), 0.027, 0.024)
    ball(bm, "neck", (0, 0, 1.505), 0.033)

    # Chest: a ribcage block, broad at the shoulders and narrowing to the waist, flatter front to back.
    lathe(bm, "chest", 0, (0, 0, 1.235), (0, 0, 1.535),
          lambda t: ((0.112 + 0.05 * t) * boxy(t, 2.6), (0.086 + 0.008 * t) * boxy(t, 2.6)), rings=32)
    # Waist: a ball between chest and pelvis, as on every artist's mannequin.
    ball(bm, "spine", (0, 0, 1.18), 0.064)
    # Pelvis: rounded, broader at the hips.
    lathe(bm, "hips", 0, (0, 0, 0.925), (0, 0, 1.115),
          lambda t: ((0.148 - 0.025 * t) * boxy(t, 2.4), 0.098 * boxy(t, 2.4)), rings=28)

    for side, sx in (("L", 1), ("R", -1)):
        # Shoulder, arm, hand
        ball(bm, f"upper_arm.{side}", (0.182 * sx, 0, 1.462), 0.043)
        screw(bm, f"upper_arm.{side}", (0.225 * sx, 0, 1.462), (sx, 0, 0))
        rod(bm, f"upper_arm.{side}", (0.200 * sx, 0, 1.462), (0.447 * sx, 0, 1.46), 0.036, 0.029, bulge=0.12)
        ball(bm, f"forearm.{side}", (0.462 * sx, 0, 1.46), 0.03)
        screw(bm, f"forearm.{side}", (0.462 * sx, 0.03, 1.46), (0, 1, 0), 0.009)
        screw(bm, f"forearm.{side}", (0.462 * sx, -0.03, 1.46), (0, -1, 0), 0.009)
        rod(bm, f"forearm.{side}", (0.474 * sx, 0, 1.46), (0.702 * sx, 0, 1.46), 0.03, 0.021, bulge=0.15)
        ball(bm, f"hand.{side}", (0.712 * sx, 0, 1.46), 0.021)
        # Hand: a flat paddle, widest across the knuckles, and a thumb angled forwards.
        lathe(bm, f"hand.{side}", 0, (0.716 * sx, 0, 1.458), (0.885 * sx, 0, 1.452),
              lambda t: ((0.030 + 0.016 * math.sin(math.pi * min(1.0, t * 1.3))) * boxy(t, 2.4), 0.016 * boxy(t, 2.4)),
              side=(0, 0, 1), rings=20)
        rod(bm, f"hand.{side}", (0.735 * sx, -0.012, 1.445), (0.800 * sx, -0.036, 1.420), 0.012, 0.009)
        # Hip, leg, foot
        ball(bm, f"thigh.{side}", (0.10 * sx, 0, 0.982), 0.054)
        screw(bm, f"thigh.{side}", (0.154 * sx, 0, 0.982), (sx, 0, 0))
        rod(bm, f"thigh.{side}", (0.10 * sx, 0, 0.935), (0.10 * sx, 0, 0.568), 0.058, 0.043, bulge=0.1, side=(0, 1, 0))
        ball(bm, f"shin.{side}", (0.10 * sx, 0, 0.55), 0.042)
        screw(bm, f"shin.{side}", (0.142 * sx, 0, 0.55), (sx, 0, 0))
        screw(bm, f"shin.{side}", (0.058 * sx, 0, 0.55), (-sx, 0, 0))
        rod(bm, f"shin.{side}", (0.10 * sx, 0, 0.535), (0.10 * sx, 0, 0.115), 0.043, 0.029, bulge=0.14, side=(0, 1, 0))
        ball(bm, f"foot.{side}", (0.10 * sx, 0, 0.098), 0.03)
        # Foot: heel to toe, wider and lower towards the toes, its sole on the floor.
        lathe(bm, f"foot.{side}", 0, (0.10 * sx, 0.05, 0.036), (0.10 * sx, -0.185, 0.026),
              lambda t: (0.040 * boxy(t, 2.2) * (0.82 + 0.3 * math.sin(math.pi * min(1.0, t * 0.9))),
                         0.036 * boxy(t, 2.2) * (1 - 0.3 * t)), rings=24)
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

    # Bone lengths and hip height come from the armature itself, so a remodelled mannequin with other
    # proportions still fits. The take's travel is scaled to the mannequin's hip height.
    root = joint["root"]
    hip_height = armature.data.bones["hips"].head_local.z
    scale = hip_height / max(float(pos[0, root, 2]), 0.3)

    rest = {b.name: b.matrix_local.to_3x3() for b in armature.data.bones}
    correction = {}
    for bone, (head, tail, _, follow, (a, b)) in BONES.items():
        data = armature.data.bones[bone]
        rest_dir = (data.tail_local - data.head_local).normalized()
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
                travel.z = (pos[f, root, 2] * scale) - hip_height
                pose.location = rest[bone].transposed() @ travel
                pose.keyframe_insert("location", frame=f + 1)
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = 1, frames
    print(f"retargeted {Path(npz_path).name}: {frames} frames at {fps:g} fps onto {len(order)} bones")
    return action


def find_repo():
    here = next((Path(bpy.path.abspath(t.filepath)) for t in bpy.data.texts if t.filepath.endswith("blender_mannequin.py")),
                Path(globals().get("__file__", "tools/blender_mannequin.py")))
    return here.resolve().parent.parent


if __name__ == "__main__":
    # The mannequin to use, first match wins:
    #   1. a 'mannequin' armature already in the open Blender file (you are remodelling it right now);
    #   2. assets/mannequin/mannequin.blend (your saved remodel);
    #   3. the one this script builds.
    # Remodel freely, but keep the armature named 'mannequin', its bone names, the T-pose, and every part
    # weighted to the bone that moves it. Bones may be moved and resized: retargeting reads them from the file.
    #   blender --background --python tools/blender_mannequin.py -- --save-model    writes the built one to (2)
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    repo = find_repo()
    model = repo / "assets" / "mannequin" / "mannequin.blend"
    if args[:1] == ["--save-model"]:
        build()
        model.parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(model))
        print("saved", model)
        sys.exit(0)
    existing = bpy.data.objects.get("mannequin")
    if existing and existing.type == "ARMATURE":
        armature = existing
        print("using the mannequin in the open file")
    elif model.exists():
        bpy.ops.wm.open_mainfile(filepath=str(model))
        armature = bpy.data.objects["mannequin"]
        print("using", model)
    else:
        armature = build()
    take = args[0] if args else None
    if take is None:
        found = [p for p in (repo / "captures").glob("*-sam3d.npz") if not p.name.startswith("zz-")]
        take = str(max(found, key=lambda p: p.stat().st_mtime)) if found else "-"
    if take != "-":
        retarget(armature, take)
    if len(args) > 1:
        Path(args[1]).parent.mkdir(parents=True, exist_ok=True)
        bpy.ops.export_scene.gltf(filepath=str(Path(args[1]).resolve()), export_format="GLB", export_animations=True,
                                  export_yup=True)
        print("exported", args[1])

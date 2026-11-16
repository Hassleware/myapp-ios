"""A table tennis bat duct-taped to a hand: the mannequin game sticks one to each of the mannequin's hands.

    blender --background --python tools/blender_bats.py -- games/mannequin/bat.glb

It measures the mannequin's left hand (assets/mannequin/mannequin.blend if there is one, otherwise the built
mannequin) and builds to fit it: a wooden handle laid along the palm, the blade beyond the fingertips (red rubber
one side, black the other), and a dozen crooked, crumpled wraps of silver duct tape round hand and handle, some
across the blade's neck, one end hanging loose. The tape has a generated texture: woven backing under silver.

The bat is modelled round the left wrist in the T-pose (Blender axes: +x along the hand, +z up). The game
places it on each hand every frame, turned round for the right hand.
"""

import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

TAPE_WIDTH = 0.048       # real duct tape is 48 mm wide
TAPE_THICK = 0.0012
TEXTURE_METRES = 0.06    # one copy of the tape texture covers this many metres


def find_repo():
    here = next((Path(bpy.path.abspath(t.filepath)) for t in bpy.data.texts if t.filepath.endswith("blender_bats.py")),
                Path(globals().get("__file__", "tools/blender_bats.py")))
    return here.resolve().parent.parent


def hand_size(repo):
    """The left hand's extent from the wrist in the T-pose: along the hand (x), up (z) and front to back (y)."""
    model = repo / "assets" / "mannequin" / "mannequin.blend"
    if model.exists():
        bpy.ops.wm.open_mainfile(filepath=str(model))
    else:
        namespace = {"__name__": "builder", "__file__": str(repo / "tools" / "blender_mannequin.py")}
        exec(compile((repo / "tools" / "blender_mannequin.py").read_text(), "blender_mannequin.py", "exec"), namespace)
        namespace["build"]()
    armature = bpy.data.objects["mannequin"]
    wrist = armature.matrix_world @ armature.data.bones["hand.L"].head_local
    points = []
    for obj in bpy.data.objects:
        if obj.type != "MESH" or "hand.L" not in obj.vertex_groups:
            continue
        group = obj.vertex_groups["hand.L"].index
        for vert in obj.data.vertices:
            if any(g.group == group and g.weight > 0.5 for g in vert.groups):
                points.append(obj.matrix_world @ vert.co - wrist)
    if not points:
        raise SystemExit("couldn't find the mannequin's left hand (vertex group 'hand.L')")
    p = np.array([tuple(q) for q in points])
    return p.min(axis=0), p.max(axis=0)


# ---------------------------------------------------------------- textures and materials

def tape_image(size=512, seed=3):
    """Silver duct tape, tileable: a woven cloth backing showing through, creases, a little sheen."""
    rng = np.random.default_rng(seed)
    u, v = np.meshgrid(np.arange(size) / size, np.arange(size) / size)
    weave = 0.5 + 0.25 * np.sin(2 * np.pi * 34 * u) + 0.25 * np.sin(2 * np.pi * 26 * v)   # the cloth's threads
    creases = np.zeros_like(u)
    for _ in range(18):   # straight creases at random angles, as tape gets when it is pressed round things
        a, b = rng.integers(-4, 5), rng.integers(-4, 5)
        if a == 0 and b == 0:
            continue
        line = np.sin(2 * np.pi * (a * u + b * v) + rng.uniform(0, 2 * np.pi))
        creases += rng.uniform(0.2, 0.6) * np.exp(-((line - rng.uniform(-0.9, 0.9)) ** 2) / 0.002) * rng.choice([-1, 1])
    shine = 0.5 + 0.5 * np.sin(2 * np.pi * (2 * u + 1 * v))
    grey = 0.56 + 0.07 * (weave - 0.5) + 0.14 * creases + 0.06 * shine   # duct-tape silver: mid grey, creases catching light
    rgb = np.clip(np.stack([grey * 0.98, grey * 0.99, grey * 1.02], -1), 0, 1)
    image = bpy.data.images.get("duct tape") or bpy.data.images.new("duct tape", size, size)
    image.pixels.foreach_set(np.concatenate([rgb[::-1], np.ones((size, size, 1))], -1).astype(np.float32).ravel())
    image.pack()
    return image


def material(name, color, roughness=0.5, metallic=0.0, image=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    shader = m.node_tree.nodes["Principled BSDF"]
    shader.inputs["Base Color"].default_value = color
    shader.inputs["Roughness"].default_value = roughness
    shader.inputs["Metallic"].default_value = metallic
    m.diffuse_color = color
    if image:
        texture = m.node_tree.nodes.new("ShaderNodeTexImage")
        texture.image = image
        m.node_tree.links.new(texture.outputs["Color"], shader.inputs["Base Color"])
    return m


# ---------------------------------------------------------------- geometry

def finish(bm, verts, slot):
    for f in {f for v in verts for f in v.link_faces}:
        f.material_index = slot


def rounded_box(bm, centre, size, bevel, slot):
    before = set(bm.faces)
    made = bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=size, verts=made["verts"])
    bmesh.ops.translate(bm, vec=centre, verts=made["verts"])
    edges = list({e for v in made["verts"] for e in v.link_edges})
    bmesh.ops.bevel(bm, geom=edges + made["verts"], offset=bevel, segments=3, affect="EDGES", profile=0.5)
    for f in set(bm.faces) - before:   # bevelling replaces the cube's edges, so find its faces afresh
        f.material_index = slot


def disc(bm, centre, rx, rz, thickness, slot, segments=56):
    """A flat oval facing front and back (its thin side along y)."""
    made = bmesh.ops.create_cone(bm, cap_ends=True, segments=segments, radius1=1, radius2=1, depth=thickness)["verts"]
    bmesh.ops.rotate(bm, cent=(0, 0, 0), matrix=Matrix.Rotation(math.pi / 2, 3, "X"), verts=made)
    bmesh.ops.scale(bm, vec=(rx, 1, rz), verts=made)
    bmesh.ops.translate(bm, vec=centre, verts=made)
    finish(bm, made, slot)


def tape_band(bm, uv, rng, x, y_range, z_half, slot, tilt=0.0, lean=0.0, width=TAPE_WIDTH, flap=0.0):
    """One wrap of tape round a rounded-rectangle bundle (y_range front to back, ±z_half up and down), centred at x
    along the hand, tipped by tilt (about up) and lean (about front-back). Crumpled a little. A flap > 0 leaves
    that much loose end hanging off the bottom."""
    y0, y1 = y_range
    cy, hy, hz = (y0 + y1) / 2, (y1 - y0) / 2 + TAPE_THICK, z_half + TAPE_THICK
    corner = min(hy, hz) * 0.7
    # The loop's path in the y-z plane, as points with outward directions.
    path = []
    straight_y, straight_z = hy - corner, hz - corner
    for q, (sy, sz) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        start = math.pi / 2 * q
        for k in range(10):
            a = start + math.pi / 2 * k / 9
            n = Vector((0, math.cos(a), math.sin(a)))
            path.append((Vector((0, cy + sy * straight_y + corner * math.cos(a), sz * straight_z + corner * math.sin(a))), n))
    path.append(path[0])
    if flap > 0:   # a loose end: continues off the bottom and droops
        last, n = path[-1]
        for k in range(1, 6):
            path.append((last + Vector((0, -0.004 * k, -flap * k / 5)), n))
    across = 4
    turn = Matrix.Rotation(tilt, 3, "Z") @ Matrix.Rotation(lean, 3, "Y")
    rows_out, rows_in, lengths = [], [], [0.0]
    for i, (p, n) in enumerate(path):
        if i:
            lengths.append(lengths[-1] + (p - path[i - 1][0]).length)
        bump = rng.normal(0, 0.0007)
        out_row, in_row = [], []
        for j in range(across):
            s = (j / (across - 1) - 0.5) * width
            wrinkle = rng.normal(0, 0.0004)
            base = Vector((s + rng.normal(0, 0.0006), p.y, p.z)) + n * (bump + wrinkle)
            out_row.append(bm.verts.new(turn @ (base + n * TAPE_THICK) + Vector((x, 0, 0))))
            in_row.append(bm.verts.new(turn @ base + Vector((x, 0, 0))))
        rows_out.append(out_row)
        rows_in.append(in_row)
    made = []
    for i in range(len(path) - 1):
        for j in range(across - 1):
            corners = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
            for rows, order in ((rows_out, corners), (rows_in, corners[::-1])):
                face = bm.faces.new([rows[a][b] for a, b in order])
                for loop, (a, b) in zip(face.loops, order):
                    loop[uv].uv = (lengths[a] / TEXTURE_METRES, (b / (across - 1)) * width / TEXTURE_METRES)
                made.append(face)
        for j in (0, across - 1):   # the tape's two edges
            made.append(bm.faces.new((rows_out[i][j], rows_out[i + 1][j], rows_in[i + 1][j], rows_in[i][j])))
    for f in made:
        f.material_index = slot
        f.smooth = True


def build(low, high):
    """The bat and tape round a hand that spans low..high from the wrist (x along, y front-back, z up)."""
    rng = np.random.default_rng(11)
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    mesh = bpy.data.meshes.new("taped bat")
    bat = bpy.data.objects.new("taped bat", mesh)
    bpy.context.scene.collection.objects.link(bat)
    for m in (material("handle wood", (0.78, 0.58, 0.36, 1), 0.6), material("blade edge", (0.86, 0.70, 0.48, 1), 0.6),
              material("red rubber", (0.70, 0.05, 0.05, 1), 0.75), material("black rubber", (0.04, 0.04, 0.045, 1), 0.75),
              material("duct tape", (1, 1, 1, 1), 0.35, 0.6, tape_image())):
        mesh.materials.append(m)

    bm = bmesh.new()
    uv = bm.loops.layers.uv.verify()
    hand_len = high[0]
    hand_front, hand_back = low[1], high[1]
    hand_half = max(abs(low[2]), abs(high[2]))
    mid_z = (low[2] + high[2]) / 2
    # The handle lies along the palm (the front of the hand), and the blade carries on past the fingertips.
    handle_t, handle_h = 0.022, 0.026
    handle_y = hand_front - handle_t / 2
    handle_end = hand_len * 0.92
    rounded_box(bm, (handle_end / 2 + 0.005, handle_y, mid_z), (handle_end - 0.01, handle_t, handle_h), 0.006, 0)
    rounded_box(bm, (handle_end + 0.02, handle_y, mid_z), (0.05, handle_t * 0.8, handle_h * 1.5), 0.008, 0)   # the flared throat
    blade_x = handle_end + 0.035 + 0.075
    disc(bm, (blade_x, handle_y, mid_z), 0.078, 0.073, 0.006, 1)                       # wooden core
    disc(bm, (blade_x, handle_y - 0.004, mid_z), 0.076, 0.071, 0.002, 2)              # red rubber, palm side
    disc(bm, (blade_x, handle_y + 0.004, mid_z), 0.076, 0.071, 0.002, 3)              # black rubber, back

    # A lot of tape: wraps round hand and handle together, crooked and overlapping.
    bundle_y = (handle_y - handle_t / 2, hand_back)
    bundle_z = max(hand_half, handle_h / 2)
    xs = np.linspace(0.015, hand_len - 0.01, 8)
    for i, x in enumerate(xs):
        tape_band(bm, uv, rng, float(x), bundle_y, bundle_z * rng.uniform(0.95, 1.08), 4,
                  tilt=rng.uniform(-0.12, 0.12), lean=rng.uniform(-0.35, 0.35), flap=0.035 if i == 5 else 0.0)
    # Across the throat, where the blade joins: the handle alone, wrapped criss-cross.
    throat = (handle_y - handle_t * 0.8 / 2 - 0.002, handle_y + handle_t * 0.8 / 2 + 0.002)
    for lean in (-0.55, 0.5, 0.0):
        tape_band(bm, uv, rng, handle_end + 0.02, throat, handle_h * 0.78, 4, lean=lean, tilt=rng.uniform(-0.1, 0.1))
    # Two long wraps from the back of the hand diagonally onto the throat.
    for lean in (-0.75, 0.8):
        tape_band(bm, uv, rng, hand_len * 0.8, bundle_y, bundle_z * 1.05, 4, lean=lean, width=TAPE_WIDTH * 0.9)
    bm.to_mesh(mesh)
    bm.free()
    return bat


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    repo = find_repo()
    low, high = hand_size(repo)
    print(f"left hand from the wrist: along {high[0]:.3f} m, up/down {low[2]:.3f} to {high[2]:.3f}, front/back {low[1]:.3f} to {high[1]:.3f}")
    bat = build(low, high)
    out = Path(args[0]) if args else repo / "games" / "mannequin" / "bat.glb"
    bpy.ops.export_scene.gltf(filepath=str(out.resolve()), export_format="GLB", export_yup=True)
    print("exported", out)

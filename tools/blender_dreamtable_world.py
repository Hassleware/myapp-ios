"""Dream Table's world, made in Blender to fit a PS1's budget (see games/dream-table/budget.lua).

    blender -b -P tools/blender_dreamtable_world.py -- backdrop

island    The cloud island the table stands on (flat under the cobble patch, low mounds, a puffy rim) and vines
          along the patch's edge, as one low-poly mesh with sunlight, skylight and shadow baked into its vertex
          colours: island.glb, drawn unlit.
pebbles   pebbles.png: 128 x 128, repeating, oval river stones in staggered rows with their lighting baked in;
          alpha holds each stone's number (scenery.lua fades the patch's edge with it).
sprite    cloud_sprite.png: one volumetric cloud on a clear background, 128 x 64, for drifting billboards.

backdrop  Renders everything far away (sky, towering volumetric cumulus, a cloud sea below, beanstalks) once
          with Cycles from where the game's camera sits, as a slice of panorama 110 degrees wide (yaw -55 to 55)
          and 85 tall (pitch 35 down to -50), and saves it as two 256 x 256 pictures, backdrop_l.png and
          backdrop_r.png, for a low-poly dome in scenery.lua. Across a picture, yaw is linear; down it, pitch.

Coordinates: the game has y up and the camera looking along -z; Blender has z up, so game (x, y, z) is Blender
(x, -z, y). Runs headless (no window).
"""
import math
import os
import random
import sys

import bpy
from mathutils import Vector

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(REPO, "games", "dream-table")
EYE = (0.0, 3.4, 6.0)                       # the game camera's resting place (main.lua CAM_HEIGHT, z 6)
SUN = (0.75, 0.6, 0.15)                     # toward the sun, game axes (scenery.lua's gfx.light, reversed)
YAW, PITCH = (-55, 55), (-50, 35)


def b(x, y, z):
    """Game coordinates to Blender's."""
    return Vector((x, -z, y))


def lin(c):
    return tuple(v ** 2.2 for v in c)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.view_settings.view_transform = "Standard"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for kind in ("OPTIX", "CUDA"):
        try:
            prefs.compute_device_type = kind
            prefs.get_devices()
            if any(d.type == kind for d in prefs.devices):
                for d in prefs.devices:
                    d.use = True
                sc.cycles.device = "GPU"
                break
        except TypeError:
            pass
    return sc


def node_material(name):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    return m, nt, out


def sky_world(sc):
    """A painted gradient: deep blue overhead, bright at the horizon, blue below (the drop under the island)."""
    w = bpy.data.worlds.new("sky")
    sc.world = w
    w.use_nodes = True
    nt = w.node_tree
    nt.nodes.clear()
    coord = nt.nodes.new("ShaderNodeTexCoord")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    bg = nt.nodes.new("ShaderNodeBackground")
    out = nt.nodes.new("ShaderNodeOutputWorld")
    nt.links.new(coord.outputs["Generated"], sep.inputs[0])
    mapr = nt.nodes.new("ShaderNodeMapRange")        # z -1..1 to 0..1
    mapr.inputs["From Min"].default_value = -1
    nt.links.new(sep.outputs["Z"], mapr.inputs["Value"])
    nt.links.new(mapr.outputs["Result"], ramp.inputs["Fac"])
    els = ramp.color_ramp.elements
    stops = [(0.0, (0.20, 0.42, 0.85)), (0.38, (0.36, 0.60, 0.96)), (0.5, (0.62, 0.82, 1.0)),
             (0.56, (0.30, 0.60, 0.97)), (0.75, (0.08, 0.32, 0.80)), (1.0, (0.03, 0.17, 0.60))]
    els[0].position, els[0].color = stops[0][0], lin(stops[0][1]) + (1,)
    els[1].position, els[1].color = stops[-1][0], lin(stops[-1][1]) + (1,)
    for pos, col in stops[1:-1]:
        e = els.new(pos)
        e.color = lin(col) + (1,)
    nt.links.new(ramp.outputs["Color"], bg.inputs["Color"])
    nt.links.new(bg.outputs[0], out.inputs[0])


def sun(sc):
    d = Vector(b(*SUN)).normalized()
    lamp = bpy.data.lights.new("sun", "SUN")
    lamp.energy = 5.0
    lamp.angle = math.radians(3)
    lamp.color = (1.0, 0.97, 0.9)
    o = bpy.data.objects.new("sun", lamp)
    sc.collection.objects.link(o)
    o.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()


def cloud_material():
    """Fluffy volume: dense inside, noise eating at the edges."""
    m, nt, out = node_material("cloud")
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    noise = nt.nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 0.09
    noise.inputs["Detail"].default_value = 5
    nt.links.new(geo.outputs["Position"], noise.inputs["Vector"])
    ramp = nt.nodes.new("ShaderNodeMapRange")
    ramp.inputs["From Min"].default_value = 0.38
    ramp.inputs["From Max"].default_value = 0.55
    ramp.inputs["To Max"].default_value = 0.5
    nt.links.new(noise.outputs["Fac"], ramp.inputs["Value"])
    vol = nt.nodes.new("ShaderNodeVolumePrincipled")
    vol.inputs["Color"].default_value = (1, 1, 1, 1)
    vol.inputs["Anisotropy"].default_value = 0.2
    nt.links.new(ramp.outputs["Result"], vol.inputs["Density"])
    nt.links.new(vol.outputs[0], out.inputs["Volume"])
    return m


def cumulus(sc, mat, x, y, z, wide, tall, seed):
    """A towering cloud: a broad flat base and puffs stacked up, joined into one volume."""
    rng = random.Random(seed)
    parts = []
    def puff(px, py, pz, r, squash=0.8):
        bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=3, radius=r, location=b(px, py, pz))
        o = bpy.context.object
        o.scale = (1, 1, squash)
        parts.append(o)
    for i in range(8):
        a = i / 8 * math.tau + rng.uniform(-0.3, 0.3)
        puff(x + math.cos(a) * wide * 0.4, y, z + math.sin(a) * wide * 0.3, wide * rng.uniform(0.25, 0.32), 0.6)
    levels = max(1, round(tall / wide * 3))
    for l in range(1, levels + 1):
        f = l / (levels + 1)
        for _ in range(4):
            a = rng.uniform(0, math.tau)
            s = wide * 0.3 * (1 - f * 0.6)
            puff(x + math.cos(a) * s, y + tall * f, z + math.sin(a) * s * 0.6, wide * 0.26 * (1 - f * 0.4) * rng.uniform(0.85, 1.1))
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    cloud = bpy.context.object
    cloud.data.materials.append(mat)
    return cloud


def cloud_sea(sc, mat):
    """A layer of big flattened puffs far below the island, gaps between them showing the sky further down."""
    rng = random.Random(99)
    parts = []
    for _ in range(70):
        a, d = rng.uniform(-2.4, -0.7), rng.uniform(40, 420)
        r = rng.uniform(22, 45) * (0.6 + d / 420)
        bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=2, radius=r, location=b(math.cos(a) * d, rng.uniform(-62, -50), math.sin(a) * d))
        o = bpy.context.object
        o.scale = (1, 1, 0.35)
        parts.append(o)
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    bpy.context.object.data.materials.append(mat)


def leaf_object(size, steps=8):
    """A pointed leaf along +x from the origin, folded along its vein and drooping at the tip."""
    import bmesh
    bm = bmesh.new()
    rows = []
    for i in range(steps + 1):
        t = i / steps
        w = 0.42 * math.sin(math.pi * t) ** 0.8 + 0.02
        droop = -0.25 * t * t
        rows.append([bm.verts.new((t * size, -w * size, droop * size)), bm.verts.new((t * size, 0, (droop + 0.05) * size)),
                     bm.verts.new((t * size, w * size, droop * size))])
    for i in range(len(rows) - 1):
        for j in range(2):
            bm.faces.new((rows[i][j], rows[i + 1][j], rows[i + 1][j + 1], rows[i][j + 1]))   # facing up
    me = bpy.data.meshes.new("leaf")
    bm.to_mesh(me)
    o = bpy.data.objects.new("leaf", me)
    bpy.context.scene.collection.objects.link(o)
    return o


def beanstalk(sc, x, z, height, seed):
    rng = random.Random(seed)
    curve = bpy.data.curves.new("stalk", "CURVE")
    curve.dimensions = "3D"
    curve.bevel_depth = 1.0
    curve.bevel_resolution = 3
    spline = curve.splines.new("POLY")
    steps, turns, wind = 40, rng.uniform(1.0, 1.6), rng.uniform(2, 3.5)
    spline.points.add(steps)
    base = -70
    for i in range(steps + 1):
        t = i / steps
        a = t * turns * math.tau + seed
        spline.points[i].co = tuple(b(x + math.sin(a) * wind * (1 - t * 0.5), base + t * height, z + math.cos(a) * wind * (1 - t * 0.5))) + (1,)
        spline.points[i].radius = 2.4 - 1.7 * t
    o = bpy.data.objects.new("stalk", curve)
    sc.collection.objects.link(o)
    m, nt, out = node_material("stalk")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Base Color"].default_value = lin((0.20, 0.55, 0.30)) + (1,)
    bsdf.inputs["Roughness"].default_value = 0.45
    wave = nt.nodes.new("ShaderNodeTexWave")
    wave.wave_type = "RINGS"
    wave.inputs["Scale"].default_value = 0.6
    bump = nt.nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.4
    nt.links.new(wave.outputs["Fac"], bump.inputs["Height"])
    nt.links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    nt.links.new(bsdf.outputs[0], out.inputs["Surface"])
    o.data.materials.append(m)
    leaf_mat, lnt, lout = node_material("leaf")
    lb = lnt.nodes.new("ShaderNodeBsdfPrincipled")
    lb.inputs["Base Color"].default_value = lin((0.25, 0.68, 0.28)) + (1,)
    lnt.links.new(lb.outputs[0], lout.inputs["Surface"])
    for k in range(7):
        t = rng.uniform(0.35, 0.92)
        a = t * turns * math.tau + seed
        px, py, pz = x + math.sin(a) * wind * (1 - t * 0.5), base + t * height, z + math.cos(a) * wind * (1 - t * 0.5)
        yaw = rng.uniform(0, math.tau)
        size = rng.uniform(5, 8)
        leaf = leaf_object(size)
        leaf.location = b(px + math.cos(yaw) * 1.5, py, pz + math.sin(yaw) * 1.5)
        leaf.rotation_euler = (0, rng.uniform(-0.5, -0.1), -yaw)
        leaf.data.materials.append(leaf_mat)
    return b(x, base + height, z)


def backdrop():
    sc = reset()
    sky_world(sc)
    sun(sc)
    mat = cloud_material()
    cumulus(sc, mat, 36, -16, -78, 30, 46, 1)
    cumulus(sc, mat, -48, -18, -100, 32, 38, 2)
    cumulus(sc, mat, 8, -14, -170, 46, 26, 3)
    cumulus(sc, mat, -16, -9, -62, 12, 10, 4)
    cumulus(sc, mat, 85, -12, -120, 30, 30, 5)
    for i, (x, z) in enumerate([(-26, -48), (20, -40), (-9, -70), (40, -66)]):
        top = beanstalk(sc, x, z, random.Random(i).uniform(100, 120), i + 10)
        cumulus(sc, mat, x, top.z - 2, z, 9, 6, i + 20)
    cloud_sea(sc, mat)

    cam = bpy.data.cameras.new("eye")
    cam.type = "PANO"
    cam.panorama_type = "EQUIRECTANGULAR"
    cam.longitude_min, cam.longitude_max = math.radians(YAW[0]), math.radians(YAW[1])
    cam.latitude_min, cam.latitude_max = math.radians(PITCH[0]), math.radians(PITCH[1])
    cam.clip_end = 3000
    o = bpy.data.objects.new("eye", cam)
    sc.collection.objects.link(o)
    sc.camera = o
    o.location = b(*EYE)
    o.rotation_euler = (math.radians(90), 0, 0)
    sc.render.resolution_x, sc.render.resolution_y = 1024, 512
    sc.cycles.samples = 96
    sc.cycles.use_denoising = True
    sc.cycles.volume_bounces = 6
    sc.render.film_transparent = False
    tmp = os.path.join(GAME, "_backdrop_full.png")
    sc.render.filepath = tmp
    bpy.ops.render.render(write_still=True)

    img = bpy.data.images.load(tmp)
    img.scale(512, 256)
    px = list(img.pixels)
    for name, x0 in (("backdrop_l.png", 0), ("backdrop_r.png", 256)):
        half = bpy.data.images.new(name, 256, 256)
        out = []
        for y in range(256):
            row = (y * 512 + x0) * 4
            out.extend(px[row:row + 256 * 4])
        half.pixels = out
        half.filepath_raw = os.path.join(GAME, name)
        half.file_format = "PNG"
        half.save()
    os.remove(tmp)
    print("WORLD backdrop saved")


# ---------------------------------------------------------------- the island

PATCH_X, PATCH_Z = 3.1, 5.4                 # must match scenery.lua
GROUND_X, GROUND_Z0, GROUND_Z1 = 6.5, -10.0, 12.0


def wobble(a):
    return 1 + 0.10 * math.sin(3 * a + 1.3) + 0.07 * math.sin(5 * a + 0.4) + 0.04 * math.sin(9 * a + 2.0)


def patch_reach(x, z):
    return math.sqrt((x / PATCH_X) ** 2 + (z / PATCH_Z) ** 2) / wobble(math.atan2(z / PATCH_Z, x / PATCH_X))


def smooth(a, bb, x):
    t = max(0.0, min(1.0, (x - a) / (bb - a)))
    return t * t * (3 - 2 * t)


def join(objects, active):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = active
    bpy.ops.object.join()
    return bpy.context.object


def island():
    import bmesh
    sc = reset()
    sky_world(sc)
    sun(sc)
    bpy.data.lights["sun"].energy = 2.4
    bpy.data.lights["sun"].angle = math.radians(25)    # soft shadows: low-poly can't hold sharp ones
    sc.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 1.6
    rng = random.Random(7)
    # A radial mesh round the patch's outline: flat under the cobbles, then a soft lip of cloud puffs that
    # rolls over and down out of sight. Each spoke follows the outline's angle; along it, `reach` goes from 0
    # (the middle) past 1 (the patch's edge) to the lip and over.
    SPOKES = 32
    PROFILE = [(0.0, 0.0), (0.55, 0.0), (1.1, 0.0), (1.3, 0.12), (1.5, 0.38), (1.7, 0.52), (1.88, 0.42),
               (2.02, 0.05), (2.12, -0.6), (2.18, -1.6)]
    # Scallops: the lip rises and bulges out in rounded puffs, about one every four spokes, each its own size.
    sizes = [rng.uniform(0.7, 1.3) for _ in range(SPOKES // 4 + 1)]
    def puff(k):
        f = k / 4
        return (abs(math.sin(math.pi * f)) ** 0.6) * sizes[int(f) % len(sizes)]
    lip = [0.45 + 0.85 * puff(k) for k in range(SPOKES)]
    bm = bmesh.new()
    centre = bm.verts.new(b(0, -0.03, 0))
    rings = []
    for reach, height in PROFILE[1:]:
        ring = []
        for k in range(SPOKES):
            a = k / SPOKES * math.tau
            w = wobble(a) * reach
            x, z = math.cos(a) * PATCH_X * w, math.sin(a) * PATCH_Z * w
            near = smooth(0.0, 0.8, math.sin(a))           # the side by the camera stays low, out of its way
            y = -0.03 + height * lip[k] * (1 - 0.7 * near) if reach > 1.1 else -0.03
            if reach > 1.1:                                 # spread the lip out further at the sides, and the puffs
                bulge = 1 + 0.12 * puff(k) * smooth(1.2, 1.9, reach)
                x *= (1 + 0.25 * abs(math.cos(a))) * bulge
                z *= bulge
            ring.append(bm.verts.new(b(x, y, z)))
        rings.append(ring)
    for k in range(SPOKES):
        bm.faces.new((centre, rings[0][(k + 1) % SPOKES], rings[0][k]))
    for r in range(len(rings) - 1):
        for k in range(SPOKES):
            k2 = (k + 1) % SPOKES
            bm.faces.new((rings[r][k], rings[r][k2], rings[r + 1][k2], rings[r + 1][k]))
    me = bpy.data.meshes.new("island")
    bm.to_mesh(me)
    land = bpy.data.objects.new("island", me)
    sc.collection.objects.link(land)
    bpy.context.view_layer.objects.active = land
    land.select_set(True)
    bpy.ops.object.shade_smooth()
    m, nt, out = node_material("cloud_ground")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Base Color"].default_value = (0.95, 0.96, 1.0, 1)
    bsdf.inputs["Roughness"].default_value = 1.0
    nt.links.new(bsdf.outputs[0], out.inputs["Surface"])
    land.data.materials.append(m)

    # Vines along the patch's edge: low-poly tubes with a few leaves.
    vine_mat, vnt, vout = node_material("vine")
    vb = vnt.nodes.new("ShaderNodeBsdfPrincipled")
    vb.inputs["Base Color"].default_value = lin((0.16, 0.50, 0.22)) + (1,)
    vnt.links.new(vb.outputs[0], vout.inputs["Surface"])
    vine_parts = []
    for k in range(3):
        a0 = k / 3 * math.tau + rng.uniform(0, 0.5)
        span = rng.uniform(1.4, 1.9)
        curve = bpy.data.curves.new("vine", "CURVE")
        curve.dimensions = "3D"
        curve.bevel_depth = 0.07
        curve.bevel_resolution = 0
        curve.bevel_mode = "ROUND"
        curve.resolution_u = 1
        spline = curve.splines.new("POLY")
        n = 14
        spline.points.add(n - 1)
        for i in range(n):
            a = a0 + span * i / (n - 1)
            reach = (1.08 + 0.05 * math.sin(a * 17 + k)) * wobble(a)
            x, z = math.cos(a) * PATCH_X * reach, math.sin(a) * PATCH_Z * reach
            spline.points[i].co = tuple(b(x, 0.03 + 0.05 * max(0, math.sin(a * 23 + k * 2)), z)) + (1,)
            spline.points[i].radius = 1 - 0.5 * i / n
            if i % 3 == 1:
                leaf = leaf_object(rng.uniform(0.45, 0.65), 3)
                leaf.location = b(x, 0.06, z)
                leaf.rotation_euler = (0, rng.uniform(-0.3, 0.1), -a + (math.pi / 2 if i % 6 == 1 else -math.pi / 2) + rng.uniform(-0.4, 0.4))
                leaf.data.materials.append(vine_mat)
                vine_parts.append(leaf)
        o = bpy.data.objects.new("vine", curve)
        sc.collection.objects.link(o)
        o.data.materials.append(vine_mat)
        bpy.ops.object.select_all(action="DESELECT")
        o.select_set(True)
        bpy.context.view_layer.objects.active = o
        bpy.ops.object.convert(target="MESH")
        vine_parts.append(bpy.context.object)
    for o in vine_parts:                     # vines are thin: their shadows only dirty the bake
        o.visible_shadow = False
    land = join(vine_parts + [land], land)
    land.name = "island"

    # Bake the light into vertex colours.
    attr = land.data.color_attributes.new("baked", "FLOAT_COLOR", "POINT")
    land.data.color_attributes.active_color = attr
    sc.cycles.samples = 256
    sc.render.bake.target = "VERTEX_COLORS"
    bpy.ops.object.select_all(action="DESELECT")
    land.select_set(True)
    bpy.context.view_layer.objects.active = land
    bpy.ops.object.bake(type="COMBINED")
    tris = sum(len(p.vertices) - 2 for p in land.data.polygons)
    bpy.ops.export_scene.gltf(filepath=os.path.join(GAME, "island.glb"), export_format="GLB", use_selection=True,
                              export_vertex_color="ACTIVE", export_materials="NONE", export_yup=True)
    print(f"WORLD island saved: {tris} triangles")


# ---------------------------------------------------------------- pebbles and the cloud sprite

def pebbles():
    """River stones on a repeating 128 x 128 tile, lit from the game's sun (across is game x, down is game z)."""
    import numpy as np
    N, ROWS, COLS = 128, 16, 10
    CW, CH = N / COLS, N / ROWS
    sun_dir = np.array(SUN) / np.linalg.norm(SUN)
    img = np.zeros((N, N, 4))
    img[...] = [0.26, 0.24, 0.21, 0.55]           # grit between the stones
    best_q = np.full((N, N), 1.0)
    ys, xs = np.mgrid[0:N, 0:N] + 0.5
    for i in range(-1, ROWS + 1):
        for j in range(-1, COLS + 1):
            r = np.random.default_rng((i % ROWS) * 100 + (j % COLS))
            cx = (j + 0.5 + (i % 2) * 0.5 + r.uniform(-0.2, 0.2)) * CW
            cy = (i + 0.5 + r.uniform(-0.18, 0.18)) * CH
            ax, by = CW * 0.5 * r.uniform(0.8, 1.15), CH * 0.5 * r.uniform(0.8, 1.15)
            turn = r.uniform(-0.35, 0.35)
            shade, warm, keep = r.uniform(0.62, 0.84), r.uniform(0.0, 0.06), r.uniform(0.55, 1.0)
            c, s_ = math.cos(turn), math.sin(turn)
            for ox in (-N, 0, N):
                for oy in (-N, 0, N):
                    dx, dy = xs - (cx + ox), ys - (cy + oy)
                    u, v = (dx * c + dy * s_) / ax, (-dx * s_ + dy * c) / by
                    q = u * u + v * v
                    inside = q < best_q
                    if not inside.any():
                        continue
                    best_q = np.where(inside, q, best_q)
                    dome = np.sqrt(np.clip(1 - q, 0, 1))
                    nu, nv = u * 1.6, v * 1.6                 # the dome's slope, in the stone's own axes
                    n = np.stack([nu * c - nv * s_, np.ones_like(nu), nu * s_ + nv * c], -1)
                    n /= np.linalg.norm(n, axis=-1, keepdims=True)
                    light = 0.5 + 0.7 * np.clip(n @ sun_dir, 0, 1)
                    glint = np.clip(n @ np.array([0.35, 0.93, 0.1]), 0, 1) ** 40 * 0.3
                    k = (0.72 + 0.28 * dome) * light
                    col = np.stack([(shade + warm) * k + glint, (shade + warm * 0.4) * k + glint,
                                    (shade - warm * 0.6) * k + glint, np.full_like(k, keep)], -1)
                    img = np.where(inside[..., None], col, img)
    img[..., :3] *= np.random.default_rng(1).uniform(0.95, 1.05, (N, N, 1))
    out = bpy.data.images.new("pebbles", N, N, alpha=True)
    out.colorspace_settings.name = "sRGB"
    out.pixels = np.flipud(np.clip(img, 0, 1)).ravel().tolist()
    out.filepath_raw = os.path.join(GAME, "pebbles.png")
    out.file_format = "PNG"
    out.save()
    print("WORLD pebbles saved")


def sprite():
    sc = reset()
    sky_world(sc)
    sun(sc)
    mat = cloud_material()
    cumulus(sc, mat, 0, 0, 0, 20, 10, 31)
    cam = bpy.data.cameras.new("c")
    cam.type = "ORTHO"
    cam.ortho_scale = 64
    o = bpy.data.objects.new("c", cam)
    sc.collection.objects.link(o)
    sc.camera = o
    o.location = b(0, 10, 60)
    o.rotation_euler = (math.radians(90), 0, 0)
    sc.render.resolution_x, sc.render.resolution_y = 256, 128
    sc.render.film_transparent = True
    sc.cycles.samples = 128
    sc.cycles.volume_bounces = 6
    sc.render.image_settings.color_mode = "RGBA"
    tmp = os.path.join(GAME, "_sprite.png")
    sc.render.filepath = tmp
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(tmp)
    img.scale(128, 64)
    img.filepath_raw = os.path.join(GAME, "cloud_sprite.png")
    img.file_format = "PNG"
    img.save()
    os.remove(tmp)
    print("WORLD sprite saved")


what = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else ["backdrop"]
if "backdrop" in what:
    backdrop()
if "island" in what:
    island()
if "pebbles" in what:
    pebbles()
if "sprite" in what:
    sprite()

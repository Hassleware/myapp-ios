"""Makes the small test files used by games/features: textures, sounds and two glTF models.

Run it again after changing it:  python tools/make_demo_assets.py
It needs Pillow (pip install pillow) and, for the .ogg music, ffmpeg. Real games get their files from
Blender and a sound editor instead; this exists so the demo has no hand-made binary files.
"""

import io
import json
import math
import shutil
import struct
import subprocess
import wave
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent.parent / "games" / "features"


# ---------------------------------------------------------------- textures

def checker_png():
    image = Image.new("RGBA", (64, 64))
    draw = ImageDraw.Draw(image)
    for y in range(8):
        for x in range(8):
            shade = 235 if (x + y) % 2 == 0 else 150
            draw.rectangle([x * 8, y * 8, x * 8 + 7, y * 8 + 7], fill=(shade, shade, shade, 255))
    data = io.BytesIO()
    image.save(data, "PNG")
    return data.getvalue()


def spark_png():
    size = 64
    image = Image.new("RGBA", (size, size))
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - size / 2 + 0.5, y - size / 2 + 0.5) / (size / 2)
            image.putpixel((x, y), (255, 255, 255, int(255 * max(0.0, 1 - d) ** 2)))
    data = io.BytesIO()
    image.save(data, "PNG")
    return data.getvalue()


def sky_jpg():
    """A panorama: dark blue overhead, pale at the horizon, a brown ground below, and a sun."""
    w, h = 512, 256
    image = Image.new("RGB", (w, h))
    for y in range(h):
        t = y / (h - 1)
        if t < 0.5:
            k = t / 0.5
            color = (int(40 + 150 * k), int(90 + 130 * k), int(190 + 50 * k))
        else:
            k = (t - 0.5) / 0.5
            color = (int(150 - 90 * k), int(140 - 85 * k), int(120 - 80 * k))
        for x in range(w):
            image.putpixel((x, y), color)
    draw = ImageDraw.Draw(image)
    draw.ellipse([w * 0.70 - 9, h * 0.30 - 9, w * 0.70 + 9, h * 0.30 + 9], fill=(255, 250, 220))
    data = io.BytesIO()
    image.save(data, "JPEG", quality=88)
    return data.getvalue()


# ---------------------------------------------------------------- sounds

def tone(path, notes, seconds_each, volume=0.5, rate=22050):
    frames = bytearray()
    for note in notes:
        count = int(rate * seconds_each)
        for i in range(count):
            fade = min(1.0, i / 200) * min(1.0, (count - i) / (count * 0.6))
            value = math.sin(2 * math.pi * note * i / rate) * volume * fade
            frames += struct.pack("<h", int(value * 32767))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(frames))


# ---------------------------------------------------------------- glTF

class Glb:
    """Collects binary data and the JSON that describes it, then writes one .glb file."""

    def __init__(self):
        self.bin = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "make_demo_assets.py"}, "scene": 0, "scenes": [{"nodes": []}],
                    "nodes": [], "meshes": [], "materials": [], "accessors": [], "bufferViews": [], "buffers": []}

    def view(self, data):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["bufferViews"].append({"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)})
        self.bin += data
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values, kind, component="f"):
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[kind]
        code = {"f": 5126, "H": 5123, "I": 5125, "B": 5121}[component]
        flat = [x for v in values for x in (v if width > 1 else [v])]
        entry = {"bufferView": self.view(struct.pack(f"<{len(flat)}{component}", *flat)), "componentType": code, "count": len(values), "type": kind}
        if kind == "VEC3" and component == "f":
            entry["min"] = [min(v[i] for v in values) for i in range(3)]
            entry["max"] = [max(v[i] for v in values) for i in range(3)]
        if kind == "SCALAR" and component == "f":
            entry["min"], entry["max"] = [min(values)], [max(values)]
        self.doc["accessors"].append(entry)
        return len(self.doc["accessors"]) - 1

    def material(self, color, texture=None):
        pbr = {"baseColorFactor": list(color) + [1.0], "metallicFactor": 0.0, "roughnessFactor": 0.9}
        if texture is not None:
            pbr["baseColorTexture"] = {"index": texture}
        self.doc["materials"].append({"pbrMetallicRoughness": pbr})
        return len(self.doc["materials"]) - 1

    def texture(self, png):
        self.doc.setdefault("images", []).append({"bufferView": self.view(png), "mimeType": "image/png"})
        self.doc.setdefault("samplers", [{}])
        self.doc.setdefault("textures", []).append({"source": len(self.doc["images"]) - 1, "sampler": 0})
        return len(self.doc["textures"]) - 1

    def mesh(self, positions, normals, uvs, indices, material, joints=None, weights=None):
        attributes = {"POSITION": self.accessor(positions, "VEC3"), "NORMAL": self.accessor(normals, "VEC3"), "TEXCOORD_0": self.accessor(uvs, "VEC2")}
        if joints:
            attributes["JOINTS_0"] = self.accessor(joints, "VEC4", "B")
            attributes["WEIGHTS_0"] = self.accessor(weights, "VEC4")
        self.doc["meshes"].append({"primitives": [{"attributes": attributes, "indices": self.accessor(indices, "SCALAR", "H"), "material": material}]})
        return len(self.doc["meshes"]) - 1

    def node(self, name, parent=None, **fields):
        self.doc["nodes"].append({"name": name, **fields})
        index = len(self.doc["nodes"]) - 1
        if parent is None:
            self.doc["scenes"][0]["nodes"].append(index)
        else:
            self.doc["nodes"][parent].setdefault("children", []).append(index)
        return index

    def save(self, path):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["buffers"] = [{"byteLength": len(self.bin)}]
        text = json.dumps(self.doc, separators=(",", ":")).encode()
        text += b" " * (-len(text) % 4)
        total = 12 + 8 + len(text) + 8 + len(self.bin)
        with open(path, "wb") as out:
            out.write(struct.pack("<4sII", b"glTF", 2, total))
            out.write(struct.pack("<I4s", len(text), b"JSON") + text)
            out.write(struct.pack("<I4s", len(self.bin), b"BIN\0") + bytes(self.bin))


def box(sx, sy, sz, rings=1, base=0.0):
    """A box with its triangles facing outward. With rings above 1 it is cut into slices up its height,
    so a skeleton can bend it. base moves its bottom to that height."""
    positions, normals, uvs, indices = [], [], [], []

    def quad(corners, normal):
        start = len(positions)
        positions.extend(corners)
        normals.extend([normal] * 4)
        uvs.extend([(0, 1), (1, 1), (1, 0), (0, 0)])
        a, b, c = corners[0], corners[1], corners[2]
        e1 = [b[i] - a[i] for i in range(3)]
        e2 = [c[i] - a[i] for i in range(3)]
        cross = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        facing = sum(cross[i] * normal[i] for i in range(3)) > 0
        indices.extend([start, start + 1, start + 2, start, start + 2, start + 3] if facing else [start, start + 2, start + 1, start, start + 3, start + 2])

    hx, hz = sx / 2, sz / 2
    for ring in range(rings):
        y0, y1 = base + sy * ring / rings, base + sy * (ring + 1) / rings
        quad([(-hx, y0, hz), (hx, y0, hz), (hx, y1, hz), (-hx, y1, hz)], (0, 0, 1))
        quad([(hx, y0, -hz), (-hx, y0, -hz), (-hx, y1, -hz), (hx, y1, -hz)], (0, 0, -1))
        quad([(hx, y0, hz), (hx, y0, -hz), (hx, y1, -hz), (hx, y1, hz)], (1, 0, 0))
        quad([(-hx, y0, -hz), (-hx, y0, hz), (-hx, y1, hz), (-hx, y1, -hz)], (-1, 0, 0))
    quad([(-hx, base + sy, hz), (hx, base + sy, hz), (hx, base + sy, -hz), (-hx, base + sy, -hz)], (0, 1, 0))
    quad([(-hx, base, -hz), (hx, base, -hz), (hx, base, hz), (-hx, base, hz)], (0, -1, 0))
    return positions, normals, uvs, indices


def quat(axis, degrees):
    half = math.radians(degrees) / 2
    s = math.sin(half)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half)]


def hero_glb(path, checker):
    """A bendy post with a three-joint skeleton and two animations: 'wave' and 'hop'."""
    g = Glb()
    positions, normals, uvs, indices = box(0.5, 2.0, 0.5, rings=10)
    joints, weights = [], []
    for x, y, z in positions:
        k = min(max(y / 2.0, 0.0), 1.0) * 2       # 0 at the feet, 2 at the top
        low = min(int(k), 1)
        joints.append((low, low + 1, 0, 0))
        weights.append((1 - (k - low), k - low, 0.0, 0.0))
    material = g.material((1.0, 0.55, 0.25), g.texture(checker))
    mesh = g.mesh(positions, normals, uvs, indices, material, joints, weights)

    root = g.node("Root")
    middle = g.node("Middle", root, translation=[0, 1, 0])
    top = g.node("Top", middle, translation=[0, 1, 0])
    g.node("Hand", top, translation=[0.4, 0, 0])
    body = g.node("Hero", mesh=mesh, skin=0)
    inverse = [[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -y, 0, 1] for y in (0, 1, 2)]
    g.doc["skins"] = [{"joints": [root, middle, top], "inverseBindMatrices": g.accessor(inverse, "MAT4"), "skeleton": root}]
    assert body == 4

    times = g.accessor([0.0, 0.5, 1.0, 1.5, 2.0], "SCALAR")
    sway = g.accessor([quat((0, 0, 1), a) for a in (0, 35, 0, -35, 0)], "VEC4")
    sway_top = g.accessor([quat((0, 0, 1), a) for a in (0, 40, 0, -40, 0)], "VEC4")
    hop_times = g.accessor([0.0, 0.25, 0.5, 1.0], "SCALAR")
    hop = g.accessor([(0, 0, 0), (0, 0.9, 0), (0, 0, 0), (0, 0, 0)], "VEC3")
    squash = g.accessor([(1, 1, 1), (0.85, 1.25, 0.85), (1.2, 0.75, 1.2), (1, 1, 1)], "VEC3")
    g.doc["animations"] = [
        {"name": "wave",
         "samplers": [{"input": times, "output": sway, "interpolation": "LINEAR"}, {"input": times, "output": sway_top, "interpolation": "LINEAR"}],
         "channels": [{"sampler": 0, "target": {"node": middle, "path": "rotation"}}, {"sampler": 1, "target": {"node": top, "path": "rotation"}}]},
        {"name": "hop",
         "samplers": [{"input": hop_times, "output": hop, "interpolation": "LINEAR"}, {"input": hop_times, "output": squash, "interpolation": "LINEAR"}],
         "channels": [{"sampler": 0, "target": {"node": root, "path": "translation"}}, {"sampler": 1, "target": {"node": root, "path": "scale"}}]},
    ]
    g.save(path)


def level_glb(path, checker):
    """A small level the way Blender would export it: scenery, a lamp, and markers with custom properties."""
    g = Glb()
    tex = g.texture(checker)
    cube = box(1, 1, 1, base=-0.5)
    ground = g.mesh(*cube, g.material((0.45, 0.62, 0.40), tex))
    stone = g.mesh(*cube, g.material((0.62, 0.60, 0.58), tex))
    wood = g.mesh(*cube, g.material((0.70, 0.48, 0.28), tex))

    g.node("Floor", mesh=ground, translation=[0, -0.5, 0], scale=[24, 1, 24])
    g.node("Ramp", mesh=wood, translation=[-5, 0.55, -2], rotation=quat((0, 0, 1), -22), scale=[6, 0.3, 3])
    g.node("Deck", mesh=wood, translation=[-9.2, 1.55, -2], scale=[3, 0.3, 3])
    for i in range(4):
        g.node(f"Step{i + 1}", mesh=stone, translation=[4 + i * 0.7, 0.125 * (i + 1), -3], scale=[0.7, 0.25 * (i + 1), 3])
    g.node("Wall", mesh=stone, translation=[0, 1, -8], scale=[14, 2, 0.6])
    g.node("Pillar", mesh=stone, translation=[3, 1.5, 3], scale=[1, 3, 1])

    g.node("Spawn", translation=[0, 1, 4], extras={"type": "spawn", "speed": 5.5})
    for i, (x, y, z) in enumerate([(-9.2, 2.6, -2), (6.4, 2.2, -3), (3, 0.8, 6), (-3, 0.8, 1)]):
        g.node(f"Coin{i + 1}", translation=[x, y, z], extras={"type": "coin", "value": 1 + i})
    g.node("Zone", translation=[7.5, 1, 5], scale=[3, 2, 3], extras={"type": "zone", "message": "A TRIGGER ZONE FROM THE LEVEL FILE"})

    g.doc["extensionsUsed"] = ["KHR_lights_punctual"]
    g.doc["extensions"] = {"KHR_lights_punctual": {"lights": [{"type": "point", "color": [1.0, 0.75, 0.4], "intensity": 200, "range": 9}]}}
    g.node("Lamp", translation=[3, 3.6, 3], extensions={"KHR_lights_punctual": {"light": 0}})
    g.save(path)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    checker = checker_png()
    (OUT / "checker.png").write_bytes(checker)
    (OUT / "spark.png").write_bytes(spark_png())
    (OUT / "sky.jpg").write_bytes(sky_jpg())
    tone(OUT / "coin.wav", [880, 1320], 0.09)
    tone(OUT / "jump.wav", [300, 420, 560], 0.05)
    tune = OUT / "tune.wav"
    tone(tune, [262, 330, 392, 523, 392, 330, 294, 349, 440, 587, 440, 349], 0.33, volume=0.25, rate=44100)
    if shutil.which("ffmpeg"):
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(tune), "-c:a", "libvorbis", "-q:a", "3", str(OUT / "music.ogg")], check=True)
    else:
        print("ffmpeg not found: music.ogg was not made")
    tune.unlink()
    hero_glb(OUT / "hero.glb", checker)
    level_glb(OUT / "level.glb", checker)
    print("Wrote", ", ".join(sorted(p.name for p in OUT.iterdir() if p.suffix != ".lua")))


if __name__ == "__main__":
    main()

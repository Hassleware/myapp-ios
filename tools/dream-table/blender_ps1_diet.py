"""Puts a .glb on a PS1 diet: fewer triangles, small textures, no normal maps. Skins, bone weights, node names
and animations are kept.

    blender -b -P tools/dream-table/blender_ps1_diet.py -- in.glb out.glb <triangles> <texture size>

Each mesh is decimated (edge collapse) so the whole file has about <triangles>; every image is scaled down to
at most <texture size> on its longer side; normal maps are unplugged. Runs headless (no window).
"""
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:]
src, dst, target, tex_size = args[0], args[1], int(args[2]), int(args[3])

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)


def triangles(obj):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
before = sum(triangles(o) for o in meshes)
ratio = min(1.0, target / max(1, before))
for obj in meshes:
    bpy.context.view_layer.objects.active = obj
    mod = obj.modifiers.new("diet", "DECIMATE")
    mod.ratio = ratio
    mod.use_collapse_triangulate = True
    while obj.modifiers[0] != mod:                      # above the armature, so it is applied to the rest shape
        bpy.ops.object.modifier_move_up(modifier=mod.name)
    bpy.ops.object.modifier_apply(modifier=mod.name)
after = sum(triangles(o) for o in meshes)

for image in bpy.data.images:
    w, h = image.size
    if max(w, h) > tex_size:
        k = tex_size / max(w, h)
        image.scale(max(1, round(w * k)), max(1, round(h * k)))
        image.pack()
for mat in bpy.data.materials:
    if not mat.use_nodes:
        continue
    for node in mat.node_tree.nodes:
        if node.type == "BSDF_PRINCIPLED":
            for link in list(node.inputs["Normal"].links):
                mat.node_tree.links.remove(link)

bpy.ops.export_scene.gltf(filepath=dst, export_format="GLB", export_animations=True, export_animation_mode="ACTIONS",
                          export_skins=True, export_yup=True, export_image_format="AUTO")
print(f"DIET {src}: {before} -> {after} triangles, textures at most {tex_size}, actions {len(bpy.data.actions)}")

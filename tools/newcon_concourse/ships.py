"""Game ship meshes for Blender renders (#553). Runs inside Blender.

The WC meshes in assets/meshes/ships_wcnews/ are OBJ files with a
<stem>.materials.json sidecar ({"materials": [{"name", "diffuse", "normal"?}]})
instead of an MTL. import_ship() loads one in its raw file axes, textures it,
scales it to a real length and parents it to an empty whose local frame is
the ship's: +Y nose, +Z top. Animate the empty.
"""
import json
import math
from pathlib import Path

import bpy
from mathutils import Euler, Vector

REPO = Path(__file__).resolve().parents[2]
MESHES = REPO / "assets/meshes/ships_wcnews"

# Source-axes -> ship-frame rotation (degrees, XYZ) per mesh, found by eye
# with lookdev(): both files are nose -Y, top +Z, so a half turn about Z.
FIX_EULER = {
    "demon": (0.0, 0.0, 180.0),
    "talon5": (0.0, 0.0, 180.0),
}


def _material(stem, entry):
    mat = bpy.data.materials.new(f"{stem}:{entry['name']}")
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = nodes["Principled BSDF"]
    bsdf.inputs["Metallic"].default_value = 0.55
    bsdf.inputs["Roughness"].default_value = 0.42
    tex = nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(str(MESHES / entry["diffuse"]), check_existing=True)
    links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    if entry.get("normal"):
        ntex = nodes.new("ShaderNodeTexImage")
        ntex.image = bpy.data.images.load(str(MESHES / entry["normal"]), check_existing=True)
        ntex.image.colorspace_settings.name = "Non-Color"
        nmap = nodes.new("ShaderNodeNormalMap")
        nmap.inputs["Strength"].default_value = 0.6
        links.new(ntex.outputs["Color"], nmap.inputs["Color"])
        links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def _apply_sidecar(stem, objs):
    sidecar = MESHES / f"{stem}.materials.json"
    if not sidecar.exists():
        return                       # e.g. demon: the OBJ's own MTL is textured
    by_name = {e["name"].lower(): e for e in json.loads(sidecar.read_text())["materials"]}
    for obj in objs:
        for slot in obj.material_slots:
            entry = slot.material and by_name.get(slot.material.name.lower())
            if entry:
                slot.material = _material(stem, entry)


def import_ship(stem, length_m, name=None):
    """-> root empty (+Y nose, +Z top), origin at the hull's bbox centre."""
    before = set(bpy.data.objects)
    bpy.ops.wm.obj_import(filepath=str(MESHES / f"{stem}.obj"),
                          forward_axis='Y', up_axis='Z')        # raw file axes
    objs = [o for o in bpy.data.objects if o not in before and o.type == 'MESH']
    _apply_sidecar(stem, objs)

    fix = Euler([math.radians(a) for a in FIX_EULER.get(stem, (0.0, 0.0, 0.0))], 'XYZ')
    pts = [fix.to_matrix() @ (o.matrix_world @ Vector(c)) for o in objs for c in o.bound_box]
    lo = Vector([min(p[i] for p in pts) for i in range(3)])
    hi = Vector([max(p[i] for p in pts) for i in range(3)])
    scale = length_m / (hi.y - lo.y)
    centre = (lo + hi) / 2

    root = bpy.data.objects.new(name or stem, None)
    bpy.context.scene.collection.objects.link(root)
    for o in objs:
        o.parent = root
        o.rotation_euler = fix
        o.scale = (scale,) * 3
        o.location = -centre * scale
    return root


def lookdev(stems, out_dir, length_m=20.0):
    """Top + side renders with the nose (+Y, red) and top (+Z, blue) marked,
    to find FIX_EULER by eye."""
    sc = bpy.context.scene
    out_dir = Path(out_dir)
    for stem in stems:
        root = import_ship(stem, length_m)
        marks = []
        for axis, rgb in (((0, 1, 0), (1, 0, 0, 1)), ((0, 0, 1), (0, 0.3, 1, 1))):
            bpy.ops.mesh.primitive_uv_sphere_add(radius=0.8,
                                                 location=Vector(axis) * length_m * 0.7)
            m = bpy.context.active_object
            mat = bpy.data.materials.new("mark")
            mat.node_tree.nodes["Principled BSDF"].inputs["Emission Color"].default_value = rgb
            mat.node_tree.nodes["Principled BSDF"].inputs["Emission Strength"].default_value = 5
            m.data.materials.append(mat)
            marks.append(m)
        # Top: image up = +Y. Side/front: image up = +Z (camera Y -> world up).
        for view, loc in (("top", (0, 0, 60)), ("side", (60, 0, 0)), ("front", (0, 60, 0))):
            sc.camera.location = loc
            sc.camera.rotation_euler = ((0.0, 0.0, 0.0) if view == "top" else
                                        (-Vector(loc)).to_track_quat('-Z', 'Y').to_euler())
            sc.render.filepath = str(out_dir / f"{stem}_{view}.png")
            bpy.ops.render.render(write_still=True)
        for o in [root, *root.children, *marks]:
            bpy.data.objects.remove(o)

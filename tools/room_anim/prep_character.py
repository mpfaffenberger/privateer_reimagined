"""Reduce a Meshy AI character GLB to something worth committing (#564).

Run inside Blender (headless):
    blender --background --factory-startup --python tools/room_anim/prep_character.py -- \\
        <source.glb> <out.glb> [--texture 1024] [--ratio 1.0]

Meshy exports ~100k-tri skinned meshes with 2K/4K textures, ~30 MB per clip.
Room layers are pre-rendered sprites, so the engine never sees the mesh, but
the source still has to live in the repo for the pipeline to be
reproducible. This keeps one clip per file and:
  - downsizes every texture to at most TEXTURE px (1K holds up at bar scale;
    31.7 MB -> 7.5 MB on the Rustbound Ranger),
  - drops unskinned meshes (Blender's glTF importer adds an "Icosphere"
    bone-display shape on every import),
  - optionally decimates to RATIO. Triangles cost nothing in a pre-rendered
    pipeline, so the default is 1.0 (none): collapse decimation ignores UV
    islands, and at 0.1 it shredded her face and suit texture along the
    seams. 0.5 still looked clean if file size ever matters more.
"""
import argparse
import sys
from pathlib import Path

import bpy


def is_skinned(obj):
    return any(m.type == 'ARMATURE' for m in obj.modifiers)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--ratio", type=float, default=1.0)
    ap.add_argument("--texture", type=int, default=1024)
    args = ap.parse_args(argv)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=args.src)
    for obj in list(bpy.data.objects):
        if obj.type == 'MESH' and not is_skinned(obj):
            print(f"[prep_character] dropping unskinned mesh {obj.name}")
            bpy.data.meshes.remove(obj.data)            # the data too, or glTF keeps it
    meshes = [o for o in bpy.data.objects if o.type == 'MESH']
    before = sum(len(o.data.polygons) for o in meshes)
    if args.ratio < 1.0:
        for obj in meshes:
            bpy.context.view_layer.objects.active = obj
            mod = obj.modifiers.new("Decimate", 'DECIMATE')
            mod.ratio = args.ratio
            bpy.ops.object.modifier_move_to_index(modifier=mod.name, index=0)   # before the skin
            bpy.ops.object.modifier_apply(modifier=mod.name)
    after = sum(len(o.data.polygons) for o in meshes)
    for img in bpy.data.images:
        w, h = img.size
        if max(w, h) > args.texture:
            k = args.texture / max(w, h)
            img.scale(max(1, round(w * k)), max(1, round(h * k)))
    for act in bpy.data.actions:
        print(f"[prep_character] clip {act.name} frames {tuple(act.frame_range)}")
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=str(Path(args.out).resolve()), export_format='GLB',
                              export_animations=True, export_image_format='JPEG',
                              export_jpeg_quality=90)
    print(f"[prep_character] {before} -> {after} faces, textures <= {args.texture} px")


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:])

"""Reduce a Meshy AI character GLB to something worth committing (#564).

Run inside Blender (headless):
    blender --background --factory-startup --python tools/room_anim/prep_character.py -- \\
        <source.glb> <out.glb> [--texture 1024] [--ratio 1.0] [--clip-from <other.glb>]

Meshy exports ~100k-tri skinned meshes with 2K/4K textures, ~30 MB per clip.
Room layers are pre-rendered sprites, so the engine never sees the mesh, but
the source still has to live in the repo for the pipeline to be
reproducible. This keeps one clip per file and:
  - downsizes every texture to at most TEXTURE px (1K holds up at bar scale;
    31.7 MB -> 7.5 MB on the Rustbound Ranger),
  - drops unskinned meshes (Blender's glTF importer adds an "Icosphere"
    bone-display shape on every import),
  - with --clip-from, swaps in another character's clip: every Meshy rig is
    the same 28-bone Mixamo skeleton, and a straight copy of the action
    tested clean (a world-space retarget twisted the body; #566),
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


def transplant_clip(clip_glb):
    """Replace the scene's action with the one in `clip_glb`, keyed to the
    same bone names. Everything else that file brings is deleted."""
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    own = set(bpy.data.actions)
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=clip_glb)
    for obj in [o for o in bpy.data.objects if o not in before]:
        bpy.data.objects.remove(obj)
    clip = next(a for a in bpy.data.actions if a not in own)
    for act in own:
        bpy.data.actions.remove(act)
    arm.animation_data_create().action = clip
    if hasattr(arm.animation_data, "action_slot"):         # Blender 4.4+: slotted actions
        arm.animation_data.action_slot = clip.slots[0]
    for block in (bpy.data.meshes, bpy.data.armatures, bpy.data.materials, bpy.data.images):
        for data in [d for d in block if d.users == 0]:
            block.remove(data)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--ratio", type=float, default=1.0)
    ap.add_argument("--texture", type=int, default=1024)
    ap.add_argument("--clip-from", help="take the animation from this Meshy GLB instead")
    args = ap.parse_args(argv)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=args.src)
    for obj in list(bpy.data.objects):
        if obj.type == 'MESH' and not is_skinned(obj):
            print(f"[prep_character] dropping unskinned mesh {obj.name}")
            bpy.data.meshes.remove(obj.data)            # the data too, or glTF keeps it
    if args.clip_from:
        transplant_clip(args.clip_from)
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

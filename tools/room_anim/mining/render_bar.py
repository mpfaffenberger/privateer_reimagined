"""Render 3D patrons into the mining bar painting (#564, #566).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/mining/render_bar.py -- --patron patron_orange --frames 0:229:46
    ... --                                  # every patron's full loop

Per-patron placement lives in bar_patrons.json; the camera and the lighting
are the room's, shared by everyone.

The bar is one-point perspective: the back wall faces the camera, and the
seated patrons' heads all sit near y = 405-420, so the eye is at seated head
height and the horizon runs through them. For a level camera,
    y = HORIZON + f (EYE - z) / depth,
and the painted woman's boots on the floor (y 630) against her head top
(y 405, ~1.3 m) give f / depth = 172 px/m there, a self-consistent fit. The
focal length is a free choice at that ratio. Her boots are stretched out in
front of the bench, though: the bench itself sits further back, so the 3D
patron is placed by the HIPS (hip_depth, fitted live against the clean
plate), not the feet.

She's rendered with straight alpha. The floor, the bench she sits on and the
railing behind her are Cycles shadow catchers, so her contact shadows land on
the painted set. Catchers are holdouts to the camera, so they're built from
her measured pose (seat top = the underside of her hips, railing just behind
her back) and can never cut into her. bake_bar.py composites her over the
clean-plate patch.

Writes build/room_anim/mining/bar/<patron>/NNNN.png (+ pass.json).
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Quaternion, Vector

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import prep_character  # noqa: E402
import stage  # noqa: E402
from base import paths  # noqa: E402

MINING = paths("mining")
BUILD = MINING.build / "bar"
CHARACTERS = HERE.parent / "characters"
PATRONS = {k: v for k, v in json.loads((HERE / "bar_patrons.json").read_text()).items()
           if not k.startswith("_")}

HORIZON_Y = 415.0
EYE = 1.25                          # m: seated head height
FOCAL_PX = 1200.0

KEY = (1.0, 0.78, 0.55)             # warm table lamps, up and to the left
FILL = (0.62, 0.72, 1.0)            # the glowing bar counter, low right
# Tuned against the painted woman's tone (build/room_anim/bar_tone.py): her
# shadows matched from the start, but a soft 180 W key left the highlights
# at lum 53 vs the painting's 73 and the contrast at 3.0 vs 4.5 (hazy). A
# harder, brighter key with less fill restores the painting's punch.
KEY_W, KEY_SIZE = 350.0, 0.6
FILL_W, RIM_W = 6.0, 40.0
AMBIENT = (0.008, 0.007, 0.006)     # world: the dim room beyond the lamps


def plate_to_world_x(px, depth):
    """Plate x on the plane at `depth` -> world x."""
    return (px - stage.PLATE_W / 2) * depth / FOCAL_PX


def _camera(sc):
    data = bpy.data.cameras.new("BarCam")
    data.sensor_fit, data.sensor_width = 'HORIZONTAL', 36.0
    data.lens = FOCAL_PX * 36.0 / stage.PLATE_W
    data.shift_y = -(stage.PLATE_H / 2 - HORIZON_Y) / stage.PLATE_W   # horizon above centre
    cam = bpy.data.objects.new("BarCam", data)
    sc.collection.objects.link(cam)
    cam.location = (0.0, 0.0, EYE)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    sc.camera = cam
    stage.attach_plate_reference(cam, str(MINING.room / "bar_bg.png"))


def _as_catcher(obj):
    obj.is_shadow_catcher = True
    obj.visible_shadow = False      # only the patron's shadow: the plate paints the set's own
    return obj


def _catcher(name, size, loc, sc):
    return _as_catcher(stage.box_object(name, size, loc, stage.material(name, (0.2, 0.2, 0.2)),
                                        sc.collection))


def _props(sc, p):
    """Painted furniture in front of / under the patron, as catchers: they
    hold the patron out exactly where the painted prop is and catch their
    shadow (hands on a tabletop). Each: px (plate x of the centre), depth
    (m, centre), z0..z1 (m), and either radius (a round table, stool) or
    size [w, d] (a box: the bar counter)."""
    for prop in p.get("props", []):
        x, y = plate_to_world_x(prop["px"], prop["depth"]), prop["depth"]
        z0, z1 = prop["z0"], prop["z1"]
        if "size" in prop:
            w, d = prop["size"]
            _catcher(prop["name"], (w, d, z1 - z0), (x, y, (z0 + z1) / 2), sc)
            continue
        bpy.ops.mesh.primitive_cylinder_add(vertices=48, radius=prop["radius"], depth=z1 - z0)
        obj = bpy.context.active_object
        obj.name = prop["name"]
        obj.location = (x, y, (z0 + z1) / 2)
        obj.data.materials.append(stage.material(prop["name"], (0.2, 0.2, 0.2)))
        _as_catcher(obj)


def _skin_points(body):
    dg = bpy.context.evaluated_depsgraph_get()
    return [o.matrix_world @ v.co for o in body for v in o.evaluated_get(dg).to_mesh().vertices]


def _set(sc, root, body, p):
    """Shadow catchers for the painted floor, seat and railing, built from the
    pose at frame 0 so none of them intersects the patron."""
    sc.frame_set(0)
    pts = _skin_points(body)
    hx, hy = root.location.x, root.location.y
    # Only what the painted patron really sits on / against: a phantom
    # catcher prints a shadow the painting doesn't have (#566: a railing
    # behind the back-table man caught a wedge across the painted rail).
    parts = p.get("set", ["floor", "seat", "railing"])
    if "floor" in parts:
        _catcher("Floor", (12.0, 12.0, 0.02), (hx, hy, -0.01), sc)
    if "seat" in parts:             # under the hips: needs the root under them too
        reach, depth = p["seat_reach"]
        seat = min(v.z for v in pts
                   if (v.x - hx) ** 2 + (v.y - hy) ** 2 < 0.18 ** 2 and 0.2 < v.z < 0.9) - 0.005
        _catcher("Seat", (2 * reach, depth, seat), (hx, hy + depth / 2 - 0.05, seat / 2), sc)
    if "railing" in parts:
        _catcher("Railing", (6.0, 0.05, 1.2), (hx, max(v.y for v in pts) + 0.08, 0.6), sc)


def _grade(body, rgb=(1.0, 1.0, 1.0), saturation=1.0):
    """Grade every body texture, spliced into the glTF material's base-colour
    link: multiply by `rgb` (a model's clothes needn't match the painted
    ones), then scale saturation (the painter's bartender is far more vivid
    than any render: blue denim and tanned skin, sat 0.46 vs 0.24)."""
    for mat in {slot.material for obj in body for slot in obj.material_slots if slot.material}:
        nodes, links = mat.node_tree.nodes, mat.node_tree.links
        bsdf = next(n for n in nodes if n.type == 'BSDF_PRINCIPLED')
        base = bsdf.inputs["Base Color"]
        if not base.is_linked:
            continue
        src = base.links[0].from_socket
        if tuple(rgb) != (1.0, 1.0, 1.0):
            mul = nodes.new("ShaderNodeMix")
            mul.data_type, mul.blend_type = 'RGBA', 'MULTIPLY'
            mul.inputs["Factor"].default_value = 1.0
            links.new(src, mul.inputs["A"])
            mul.inputs["B"].default_value = (*rgb, 1.0)
            src = mul.outputs["Result"]
        if saturation != 1.0:
            hsv = nodes.new("ShaderNodeHueSaturation")
            hsv.inputs["Saturation"].default_value = saturation
            links.new(src, hsv.inputs["Color"])
            src = hsv.outputs["Color"]
        links.new(src, base)


TORSO = ("Hips", "Spine", "Spine01", "Spine02", "Spine1", "Spine2", "Neck", "Head")


def _fcurves(arm):
    ad = arm.animation_data
    if hasattr(ad.action, "fcurves"):              # legacy actions
        return ad.action.fcurves
    from bpy_extras import anim_utils              # Blender 4.4+: slotted actions
    return anim_utils.action_get_channelbag_for_slot(ad.action, ad.action_slot).fcurves


def _calm(arm, k, bones=TORSO):
    """Scale `bones`' rotation keys towards the clip's first frame by `k`
    (0 = frozen upright, 1 = as animated). The clip starts and ends upright,
    so the loop stays seamless; only the depth of a lean shrinks. Blender
    normalises pose quaternions, so a per-component lerp is safe."""
    f0 = arm.animation_data.action.frame_range[0]
    for fc in _fcurves(arm):
        if not fc.data_path.endswith("rotation_quaternion"):
            continue
        if fc.data_path.split('"')[1].split(":")[-1] not in bones:
            continue
        ref = fc.evaluate(f0)
        for kp in fc.keyframe_points:
            for point in (kp.co, kp.handle_left, kp.handle_right):
                point[1] = ref + (point[1] - ref) * k
        fc.update()


def _anchor_head(root, arm, px, py, depth):
    """Move root until the crown (frame 0) projects to plate (px, py) at `depth`."""
    crown = arm.pose.bones["mixamorig:HeadTop_End"]
    for _ in range(25):                 # the pose is rigid: converges in a few steps
        bpy.context.view_layer.update()
        t = arm.matrix_world @ crown.head
        root.location.x += plate_to_world_x(px, t.y) - t.x
        root.location.y += depth - t.y
        root.location.z += EYE - (py - HORIZON_Y) * t.y / FOCAL_PX - t.z


def _aim(sc, arm, aims):
    """Point bones at fixed world spots: {bone: [plate px, depth, z]}. The
    bartender's arms aim at his painted elbows and hands on the counter, so
    his hands stay planted while the idle sways his body over them (no
    Meshy clip leans on a bar). Damped Track, not IK: nothing to flip."""
    for bone, (px, depth, z) in aims.items():
        spot = bpy.data.objects.new(f"Aim_{bone}", None)
        sc.collection.objects.link(spot)
        spot.location = (plate_to_world_x(px, depth), depth, z)
        track = arm.pose.bones[f"mixamorig:{bone}"].constraints.new('DAMPED_TRACK')
        track.target, track.track_axis = spot, 'TRACK_Y'


def _nod(arm, degrees):
    """Raise the chin by `degrees` through every neck and head key (half
    each), about the bones' local X: undoes a `pitch` lean tipping the gaze
    down, while keeping the idle's head motion."""
    fcs = [fc for fc in _fcurves(arm) if fc.data_path.endswith("rotation_quaternion")]
    offset = Quaternion((1.0, 0.0, 0.0), math.radians(-degrees / 2))
    for bone in ("Neck", "Head"):
        path = f'pose.bones["mixamorig:{bone}"].rotation_quaternion'
        quat = sorted((fc for fc in fcs if fc.data_path == path), key=lambda fc: fc.array_index)
        for keys in zip(*(fc.keyframe_points for fc in quat)):
            turned = Quaternion([k.co[1] for k in keys]) @ offset
            for k, value in zip(keys, turned):
                k.co[1] = k.handle_left[1] = k.handle_right[1] = value
        for fc in quat:
            fc.update()


def _patron(sc, p):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(CHARACTERS / p["model"]))
    new = [o for o in bpy.data.objects if o not in before]
    root = bpy.data.objects.new("Patron", None)
    sc.collection.objects.link(root)
    for obj in new:
        if obj.parent is None:
            obj.parent = root
    # Boots on the floor: the lowest skinned point at frame 0. Only skinned
    # meshes count as her: the glTF importer adds an unskinned "Icosphere"
    # bone-display shape, which is hidden.
    sc.frame_set(0)
    body = [o for o in new if o.type == 'MESH' and prep_character.is_skinned(o)]
    arm = next(o for o in new if o.type == 'ARMATURE')
    feet = min(p.z for p in _skin_points(body))
    for obj in new:
        if obj.type == 'MESH' and obj not in body:
            obj.hide_render = obj.hide_viewport = True
    if "tint" in p or "saturation" in p:
        _grade(body, p.get("tint", (1.0, 1.0, 1.0)), p.get("saturation", 1.0))
    if "lean" in p:
        _calm(arm, p["lean"])
    # `hold`: bone-name prefixes frozen at the clip's first frame. `aim`
    # only points a bone; its twist still comes from the clip. The
    # foreground man's idle flips his hand up (a claw) and twists his
    # forearm, and the bent wrist turns that twist into a 75 px sweep of the
    # fingers onto the bottle. Holding the whole right-arm chain keeps his
    # hand resting on the table (25 px of drift, from the hips).
    if "hold" in p:
        names = [b.name.split(":")[-1] for b in arm.pose.bones]
        _calm(arm, 0.0, {n for n in names if n.startswith(tuple(p["hold"]))})
    if "head_up" in p:
        _nod(arm, p["head_up"])
    # `scale`: painters cheat, and some painted patrons are burlier than any
    # model at their depth (the back-table man is ~1.3x broad, ~1.13x tall).
    # A list is [width, depth, height] in the body's frame: the bald man is
    # painted narrower but longer-backed than any model.
    s = p.get("scale", 1.0)
    root.scale = s if isinstance(s, list) else (s, s, s)
    # pitch: leans the whole body towards the camera, from the feet.
    root.rotation_euler = (math.radians(p.get("pitch", 0.0)), 0.0, math.radians(p["yaw"]))
    if "head" in p:            # a bust (feet hidden): the crown is the anchor
        _anchor_head(root, arm, *p["head"])
    else:                       # seated: the hips, feet on the floor
        root.location = (plate_to_world_x(p["hip_px"], p["hip_depth"]), p["hip_depth"],
                         -feet * root.scale.z)
    _aim(sc, arm, p.get("aim", {}))
    act = bpy.data.actions[0]
    return root, body, act


def _lights(sc, target, mix):
    """The room's key/fill/rim, placed around `target`. `mix` scales their
    energies per patron ({key, fill, rim}): the room's light isn't uniform,
    and the bartender's corner is soft and lit from the glowing counter."""
    world = bpy.data.worlds.new("Bar")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (*AMBIENT, 1.0)
    sc.world = world

    def aim(obj):
        obj.rotation_euler = (target - obj.location).to_track_quat('-Z', 'Y').to_euler()

    aim(stage.light(sc, "Key", 'AREA', target + Vector((-2.0, -1.5, 2.2)), KEY,
                    KEY_W * mix.get("key", 1.0), size=KEY_SIZE))
    aim(stage.light(sc, "Fill", 'AREA', target + Vector((1.5, -2.0, -0.2)), FILL,
                    FILL_W * mix.get("fill", 1.0), size=2.0))
    aim(stage.light(sc, "Rim", 'AREA', target + Vector((1.0, 1.0, 1.8)), KEY,
                    RIM_W * mix.get("rim", 1.0), size=1.0))


def build(samples, name=next(iter(PATRONS))):
    p = PATRONS[name]
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.render.use_motion_blur = False           # an idle: nothing moves fast
    sc.render.film_transparent = True
    x0, y0, x1, y1 = p["crop"]
    sc.render.use_border, sc.render.use_crop_to_border = True, False
    sc.render.border_min_x, sc.render.border_max_x = x0 / stage.PLATE_W, x1 / stage.PLATE_W
    sc.render.border_min_y = 1.0 - y1 / stage.PLATE_H
    sc.render.border_max_y = 1.0 - y0 / stage.PLATE_H
    _camera(sc)
    root, body, act = _patron(sc, p)
    _set(sc, root, body, p)
    _props(sc, p)
    _lights(sc, Vector((root.location.x, root.location.y, 0.8)), p.get("light", {}))
    f0, f1 = (int(v) for v in act.frame_range)
    sc.frame_start, sc.frame_end = f0, f1
    return sc, f0, f1


def render(name, frames, samples, save_blend=None):
    sc, f0, f1 = build(samples, name)
    if save_blend:
        bpy.ops.wm.save_as_mainfile(filepath=str(Path(save_blend).resolve()))
    out = BUILD / name
    out.mkdir(parents=True, exist_ok=True)
    if frames:
        first, last, *step = (int(v) for v in frames.split(":"))
        todo = range(first, last + 1, step[0] if step else 1)
    else:
        for old in out.glob("*.png"):
            old.unlink()
        todo = range(f0, f1 + 1)
    for f in todo:
        sc.frame_set(f)
        sc.render.filepath = str(out / f"{f:04d}.png")
        bpy.ops.render.render(write_still=True)
    (out / "pass.json").write_text(json.dumps(
        {"frames": f1 - f0 + 1, "first": f0, "fps": sc.render.fps,
         "crop": list(PATRONS[name]["crop"])}) + "\n")
    print(f"[render_bar] {name} done", flush=True)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--patron", action="append", choices=sorted(PATRONS),
                    help="repeatable; default: every patron")
    ap.add_argument("--frames", help="first:last[:step]")
    ap.add_argument("--samples", type=int, default=64)
    ap.add_argument("--save-blend", help="also save the scene for inspection (one patron)")
    args = ap.parse_args(argv)
    for name in args.patron or PATRONS:
        render(name, args.frames, args.samples, args.save_blend)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

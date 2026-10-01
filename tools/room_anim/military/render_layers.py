"""Render the raw passes for the animated Military concourse (#588).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/military/render_layers.py -- --check
    ... -- --layer all
    ... -- --layer munitions_train --frames 200:210 --samples 16    # quick look

--check renders glowing floor guides from the matched camera over the plate
(build/room_anim/military/check.png): the lane's kerb, dashes and ramp kerb,
the emblems' rings, and the train's route. If the match is right they hug
the painted lines.

Layers write their passes (see ../render.py) to build/room_anim/military/<layer>/;
bake_layer.py --base military then turns them into plate-aware RGBA sprites.
"""
import argparse
import sys
from pathlib import Path

import bpy

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import actors  # noqa: E402
import render  # noqa: E402
import scene as hall  # noqa: E402
from base import paths  # noqa: E402
from stage import overlay_on_plate  # noqa: E402

MILITARY = paths("military")
TRAIN_SPEED = 4.5          # m/s, a loaded ordnance tug
# Out from behind the armoured ramp's far end, round the bend, then straight
# down the lane at the camera, leaving past the bottom edge.
TRAIN_ROUTE = [(hall.RAMP_X + 9.0, hall.WINDOW_Y - 10.0), (hall.LANE_X, hall.WINDOW_Y - 10.0),
               (hall.LANE_X, 10.0)]
TRAIN_FILLET = 5.0         # m
# The painted middle emblem (centre px, ring width px) for --check.
EMBLEMS = [((587.0, 808.0), 325.0)]


def _route():
    return actors.Route(TRAIN_ROUTE, TRAIN_FILLET)


def _munitions_train(sc):
    tug, trailer, beacon = actors.build_munitions_train()
    route = _route()
    trail = actors.TUG_LENGTH + actors.TRAILER_LENGTH + actors.COUPLING_GAP
    frames = round((route.length + trail) / TRAIN_SPEED * sc.render.fps)
    actors.animate_train(tug, trailer, beacon, route, TRAIN_SPEED, 1, frames, sc.render.fps)
    return [tug, trailer], (1, frames)


# name -> builder(scene) -> (actor roots, (first frame, last frame))
LAYERS = {"munitions_train": _munitions_train}


def _build_hall(samples=48):
    sc = hall.reset()
    hall.setup_render(sc, samples)
    cam = hall.add_plate_camera(sc)
    hall.attach_plate_reference(cam, str(MILITARY.plate))
    decks = hall.add_deck(sc)
    hall.add_occluders(sc)
    hall.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_hall()
    roots, frames = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = frames
    return sc, decks, roots


def _ring(name, centre, radius, mat, sc, width=0.12):
    """A flat glowing ring on the floor."""
    import bmesh
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    for r in (radius - width / 2, radius + width / 2):
        bmesh.ops.create_circle(bm, cap_ends=False, radius=r, segments=96)
    bmesh.ops.bridge_loops(bm, edges=bm.edges[:])
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    obj.location = (*centre, 0.01)
    sc.collection.objects.link(obj)


def check(out_png):
    """Floor guides from the matched camera, overlaid on the plate."""
    sc = hall.reset()
    hall.setup_render(sc, samples=4)
    sc.view_settings.exposure = 0.0
    sc.render.film_transparent = True
    sc.render.use_motion_blur = False
    hall.add_plate_camera(sc)
    glow = {c: hall.material(f"Guide{c}", (0, 0, 0), emission=rgb, strength=0.9)
            for c, rgb in (("lane", (1.0, 0.2, 0.2)), ("ring", (0.2, 0.7, 1.0)),
                           ("route", (0.3, 1.0, 0.3)))}
    for x in (hall.KERB_X, hall.DASH_X, hall.RAMP_X):
        hall.box_object(f"Lane{x:.1f}", (0.08, 120.0, 0.01), (x, 62.0, 0.0), glow["lane"],
                        sc.collection)
    f, h = hall.FOCAL_PX, hall.EYE_HEIGHT
    for (px, py), width in EMBLEMS:
        depth = f * h / (py - hall.VANISHING_POINT[1])
        centre = ((px - hall.VANISHING_POINT[0]) * depth / f, depth)
        _ring(f"Ring{py:.0f}", centre, width * depth / f / 2, glow["ring"], sc)
    route = _route()
    s = 0.0
    while s < route.length:
        x, y, _ = route.at(s)
        hall.box_object(f"Route{s:.0f}", (0.15, 0.15, 0.01), (x, y, 0.0), glow["route"],
                        sc.collection)
        s += 1.0
    tmp = MILITARY.build / "check_guides.png"
    sc.render.filepath = str(tmp)
    bpy.ops.render.render(write_still=True)
    overlay_on_plate(MILITARY.plate, tmp, out_png)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="floor-guide overlay on the plate")
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last override, e.g. 200:210")
    ap.add_argument("--samples", type=int, help="override the beauty samples")
    args = ap.parse_args(argv)
    MILITARY.build.mkdir(parents=True, exist_ok=True)
    if args.check:
        check(MILITARY.build / "check.png")
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        if args.samples:
            sc.cycles.samples = args.samples
        render.render_passes(sc, decks, roots, MILITARY.build / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

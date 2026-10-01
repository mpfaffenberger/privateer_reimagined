"""Render the raw passes for the animated Refinery concourse's floor (#584).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/refinery/render_layers.py -- --check
    ... -- --layer all
    ... -- --layer ore_train --frames 1:48 --samples 8     # quick look

--check renders glowing guides from the matched camera over the plate:
build/room_anim/refinery/check.png. The wall ring (red) should hug the foot
of the shopfronts, the lamp ring (green) run through the floor lamps, the
garden ring (blue) skirt the planters, the ore train's route (yellow) keep
to open floor and pass through the cargo bay's door, and the 1.75 m poles
(magenta) stand people-sized next to the painted drums and doors.

Layers write their passes (see ../render.py) to build/room_anim/refinery/<layer>/;
bake_layer.py --base refinery turns them into plate-aware RGBA sprites.
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

REFINERY = paths("refinery")
TRAIN_SPEED = 3.0          # m/s, a loaded ore tug's crawl (as on the mining base)
TRAIN_RING = 15.0          # m from the axis: mid-floor, clear of lamps and shopfronts


def _arc(radius, a0, a1, step=3.0):
    n = max(1, round(abs(a1 - a0) / step))
    return [hall.floor_xy(radius, a0 + (a1 - a0) * i / n) for i in range(n + 1)]


def ore_train_route():
    """In past the bottom-left corner, clockwise round the ring floor to the
    cargo bay, out through its door and left inside, behind the wall."""
    door = sum(hall.BAY_DOOR) / 2
    inside = hall.WALL_R + 2.5
    return actors.FloorPath(
        _arc(TRAIN_RING, -150.0, door - 9.0) +
        [hall.floor_xy(TRAIN_RING + 1.5, door - 4.0), hall.floor_xy(TRAIN_RING + 3.5, door - 1.5),
         hall.floor_xy(hall.WALL_R, door), hall.floor_xy(inside - 1.0, door - 1.0)] +
        _arc(inside, door - 4.0, door - 25.0))


def _ore_train(sc):
    return actors.ore_train_along(ore_train_route(), TRAIN_SPEED, sc.render.fps)


# name -> builder(scene) -> (actor roots, frames)
LAYERS = {"ore_train": _ore_train}


def _build_atrium():
    sc = hall.reset()
    hall.setup_render(sc)
    cam = hall.add_plate_camera(sc)
    hall.attach_plate_reference(cam, str(REFINERY.plate))
    decks = hall.add_deck(sc)
    hall.add_occluders(sc)
    hall.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_atrium()
    roots, frames = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = 1, frames
    return sc, decks, roots


def _glow(name, rgb):
    return hall.material(name, (0, 0, 0), emission=rgb, strength=1.0)


def check(out_png):
    """Guides from the matched camera, alpha-over the plate."""
    sc = hall.reset()
    hall.setup_render(sc, samples=4)
    sc.view_settings.exposure = 0.0
    sc.render.film_transparent = True
    sc.render.use_motion_blur = False
    hall.add_plate_camera(sc)
    for name, r, rgb in (("Wall", hall.WALL_R, (1.0, 0.2, 0.2)),
                         ("Lamps", hall.LAMP_R, (0.2, 1.0, 0.2)),
                         ("Garden", hall.GARDEN_R, (0.2, 0.5, 1.0))):
        hall.disc(name, r + 0.08, 0.01, _glow(name, rgb), sc.collection, hole=r - 0.08)
    route = ore_train_route()
    dot = _glow("Route", (1.0, 1.0, 0.1))
    for i in range(int(route.length)):
        x, y, _ = route.at(float(i))
        hall.box_object(f"Route{i}", (0.25, 0.25, 0.02), (x, y, 0.01), dot, sc.collection)
    pole = _glow("Pole", (1.0, 0.2, 1.0))
    for a in range(-150, 181, 15):
        x, y = hall.floor_xy(0.85 * hall.WALL_R, float(a))
        hall.box_object(f"Pole{a}", (0.12, 0.12, 1.75), (x, y, 0.875), pole, sc.collection)
    tmp = REFINERY.build / "check_guides.png"
    sc.render.filepath = str(tmp)
    bpy.ops.render.render(write_still=True)
    overlay_on_plate(REFINERY.plate, tmp, out_png)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="guide overlay on the plate")
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--samples", type=int, help="override the beauty samples")
    args = ap.parse_args(argv)
    REFINERY.build.mkdir(parents=True, exist_ok=True)
    if args.check:
        check(REFINERY.build / "check.png")
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        if args.samples:
            sc.cycles.samples = args.samples
        render.render_passes(sc, decks, roots, REFINERY.build / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

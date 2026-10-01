"""Render the raw passes for the animated Refinery concourse's floor (#584,
#623).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/refinery/render_layers.py -- --check
    ... -- --layer all
    ... -- --layer ore_train --frames 1:48 --samples 8     # quick look

--check renders glowing guides from the matched camera over the plate:
build/room_anim/refinery/check.png. The wall ring (red) should hug the foot
of the shopfronts, the lamp ring (green) run through the floor lamps, the
garden ring (blue) skirt the planters, the ore train's route (yellow) keep
to open floor and pass through the cargo bay's door, the walkers' routes
(cyan, a 1.75 m pole every few metres) keep off the painted drums and out
of the garden, and the 1.75 m poles (magenta) stand people-sized next to
the painted drums and doors.

Layers write their passes (see ../render.py) to build/room_anim/refinery/<layer>/;
bake_layer.py --base refinery turns them into plate-aware RGBA sprites.
"""
import argparse
import math
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
from walkers import animate_walk_path, build_walker  # noqa: E402

REFINERY = paths("refinery")
TRAIN_SPEED = 3.0          # m/s, a loaded ore tug's crawl (as on the mining base)
TRAIN_RING = 15.0          # m from the axis: mid-floor, clear of lamps and shopfronts
WALK_FPS = 12              # plenty for a 50-70 px figure, half the atlas of 24 fps
WALK_SPEED = 1.3           # m/s
# Inside the cargo bay, from just past its door round to behind its right
# wall (BayWallR starts at -16 deg): the walkers' way in and out. The train
# turns left in there, so they keep right.
BAY_INSIDE = [(22.5, -23.0), (23.5, -16.0), (24.0, -6.0)]     # (r m, floor_xy deg)


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


def _polar(points):
    return [hall.floor_xy(r, a) for r, a in points]


def ring_walk_route():
    """A refinery hand in past the bottom edge along the inner ring, just
    outside the floor lamps (r ~11), then across the train's ring and into
    the cargo bay, right behind its wall."""
    return (_arc(12.5, -125.0, -55.0) +
            _polar([(14.0, -42.0), (17.0, -32.0), (20.5, -27.0)] + BAY_INSIDE))


def bay_walk_route():
    """A clerk out of the cargo bay from behind its right wall, across the
    train's ring and along the shopfronts, inside the painted drums (r ~17),
    off past the bottom-left corner."""
    return (_polar(BAY_INSIDE[::-1] + [(19.5, -30.0), (17.3, -38.0)]) +
            _arc(16.3, -50.0, -125.0))


WALK_ROUTES = {"walker_ring": ring_walk_route, "walker_bay": bay_walk_route}


def _ore_train(sc):
    return actors.ore_train_along(ore_train_route(), TRAIN_SPEED, sc.render.fps)


def _walker(name):
    """-> builder(scene): the walker `name` (its look in actors.LOOKS) on its
    route, keyed at WALK_FPS."""
    def build_layer(sc):
        sc.render.fps = WALK_FPS
        coat, trousers, skin, head = actors.LOOKS[name]
        root, limbs = build_walker(name, coat, trousers, skin_rgb=skin, head_rgb=head)
        return [root], animate_walk_path(root, limbs, WALK_ROUTES[name](), 0.0, WALK_FPS,
                                         speed=WALK_SPEED)
    return build_layer


# name -> builder(scene) -> (actor roots, frames)
LAYERS = {"ore_train": _ore_train, **{name: _walker(name) for name in WALK_ROUTES}}


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


def _every(points, step):
    """Points every `step` m along the polyline `points`, as walkers walk it."""
    out = []
    for (x0, y0), (x1, y1) in zip(points, points[1:]):
        n = max(1, round(math.hypot(x1 - x0, y1 - y0) / step))
        out += [(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n) for i in range(n)]
    return out + [points[-1]]


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
    walk = _glow("Walk", (0.1, 1.0, 1.0))
    for name, walk_route in WALK_ROUTES.items():
        for i, (x, y) in enumerate(_every(walk_route(), 1.0)):
            size = (0.12, 0.12, 1.75) if i % 4 == 0 else (0.2, 0.2, 0.02)
            hall.box_object(f"{name}{i}", size, (x, y, size[2] / 2), walk, sc.collection)
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

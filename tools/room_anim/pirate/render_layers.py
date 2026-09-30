"""Render the raw passes for the animated pirate concourse (#586).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/pirate/render_layers.py -- --check
    ... -- --layer all
    ... -- --layer pirate_to_bay --frames 1:24 --samples 16     # quick look

--check renders glowing floor guides (the floor's painted edges, the far
arch's legs, the pillar's foot, cross lines every 5 m) from the matched
camera over the plate: build/room_anim/pirate/check.png.

Layers write their passes (see ../render.py) to build/room_anim/pirate/<layer>/;
bake_layer.py --base pirate turns them into plate-aware RGBA sprites.
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
from stage import animate_path, overlay_on_plate  # noqa: E402
from walkers import animate_walk_path  # noqa: E402

PIRATE = paths("pirate")
POD_SPEED = 3.0            # m/s, an unhurried grav pod
POD_HOVER = 0.45           # m, underside above the deck
WALK_FPS = 12              # plenty for a 50-100 px figure, half the atlas of 24 fps


def _pod(sc):
    """Out of the side bay from behind the rock pillar, across the tunnel and
    off round the far arch's left leg into the fog-filled hall. It stays
    20 m+ out: closer, the game mesh's facets show."""
    pod, strobe = actors.build_pod()
    last = animate_path(pod, [(12.0, 25.5), (3.0, 26.5), (0.5, 33.0), (-8.0, 39.0)], POD_HOVER,
                        sc.render.fps, POD_SPEED, turn_m=4.0,
                        lift=lambda f: 0.03 * math.sin(f * 0.21))
    actors.blink(strobe, (1, last), round(sc.render.fps * 1.2))
    return [pod], (1, last)


def _pirate(sc, name, look, path, speed):
    sc.render.fps = WALK_FPS
    root, limbs = actors.build_pirate(name, look)
    last = animate_walk_path(root, limbs, path, 0.0, WALK_FPS, speed=speed)
    return [root], (1, last)


def _pirate_to_bay(sc):
    """Out from behind the far arch's left leg, across the hall mouth, down
    the tunnel and right into the side bay behind the rock pillar."""
    return _pirate(sc, "PirateToBay", "bandana",
                   [(-7.0, 39.0), (-1.0, 37.5), (1.5, 31.0), (3.5, 27.5), (11.0, 27.0)],
                   speed=1.25)


def _pirate_from_bay(sc):
    """Out of the side bay, then off down the tunnel and round the far
    arch's right leg into the hall."""
    return _pirate(sc, "PirateFromBay", "longcoat",
                   [(11.0, 25.0), (2.5, 25.5), (0.5, 31.0), (1.5, 37.0), (7.5, 39.0)],
                   speed=1.1)


# name -> builder(scene) -> (actor roots, (first frame, last frame))
LAYERS = {"cargo_pod": _pod, "pirate_to_bay": _pirate_to_bay,
          "pirate_from_bay": _pirate_from_bay}


def _build_tunnel():
    sc = hall.reset()
    hall.setup_render(sc)
    cam = hall.add_plate_camera(sc)
    hall.attach_plate_reference(cam, str(PIRATE.plate))
    decks = hall.add_deck(sc)
    hall.add_occluders(sc)
    hall.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_tunnel()
    roots, frames = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = frames
    return sc, decks, roots


def check(out_png):
    """Floor guides from the matched camera, overlaid on the plate."""
    sc = hall.reset()
    hall.setup_render(sc, samples=4)
    sc.view_settings.exposure = 0.0
    sc.render.film_transparent = True
    sc.render.use_motion_blur = False
    hall.add_plate_camera(sc)
    glow = {c: hall.material(f"Guide{c}", (0, 0, 0), emission=rgb, strength=0.9)
            for c, rgb in (("edge", (1.0, 0.2, 0.2)), ("cross", (0.2, 0.7, 1.0)),
                           ("post", (0.3, 1.0, 0.3)))}
    for x in (hall.LEFT_WALL_X, 0.0, hall.KERB_X):                 # along the tunnel
        hall.box_object(f"Along{x}", (0.06, 60.0, 0.01), (x, 30.0, 0.0), glow["edge"],
                        sc.collection)
    for y in range(5, 45, 5):                                      # cross lines
        hall.box_object(f"Cross{y}", (12.0, 0.05, 0.01), (0.8, float(y), 0.0), glow["cross"],
                        sc.collection)
    for name, x, y in (("ArchL", hall.ARCH_LEFT_X, hall.ARCH_Y),   # 2 m posts
                       ("ArchR", hall.ARCH_RIGHT_X, hall.ARCH_Y),
                       ("Pillar", hall.PILLAR_X, hall.PILLAR_Y), ("Drums", 5.6, 19.0)):
        hall.box_object(name, (0.15, 0.15, 2.0), (x, y, 1.0), glow["post"], sc.collection)
    tmp = PIRATE.build / "check_guides.png"
    sc.render.filepath = str(tmp)
    bpy.ops.render.render(write_still=True)
    overlay_on_plate(PIRATE.plate, tmp, out_png)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="floor-guide overlay on the plate")
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--samples", type=int, help="override the beauty samples")
    args = ap.parse_args(argv)
    PIRATE.build.mkdir(parents=True, exist_ok=True)
    if args.check:
        check(PIRATE.build / "check.png")
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        if args.samples:
            sc.cycles.samples = args.samples
        render.render_passes(sc, decks, roots, PIRATE.build / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

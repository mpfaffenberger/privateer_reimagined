"""Render the raw passes for the animated New Detroit concourse (#590).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/newdetroit/render_layers.py -- --check
    ... -- --layer all
    ... -- --layer walker_door --frames 1:12        # quick look

--check renders the holdouts (pillars, the platform's back wall, the left
block), the kerb and the walkers' paths from the matched camera over the
plate: build/room_anim/newdetroit/check.png.

Layers write their passes (see ../render.py) to build/room_anim/newdetroit/<layer>/;
bake_layer.py --base newdetroit then turns them into plate-aware RGBA sprites.
"""
import argparse
import sys
from itertools import pairwise
from pathlib import Path

import bpy

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import render  # noqa: E402
import scene as hall  # noqa: E402
import walkers  # noqa: E402
from base import paths  # noqa: E402
from stage import overlay_on_plate  # noqa: E402

NEWDETROIT = paths("newdetroit")
WALK_FPS = 12      # enough for a 90-160 px figure, and half the atlas of 24 fps

# Walker paths, (x, y) waypoints in metres (see scene.py for the landmarks).
# Along the platform from off-frame right, between the pillars (hidden behind
# the near one, then the far one), then out across the plaza, stepping down
# the kerb, into the side passage behind the left building's corner.
PLATFORM_PATH = [(13.3, 11.9), (13.3, 20.3), (-3.5, 24.5)]
# Out of the platform's lit doorway (from inside, behind the wall), then
# toward the camera and off-frame right.
DOOR_PATH = [(hall.BACK_WALL_X + 1.2, hall.DOOR_Y), (14.4, hall.DOOR_Y), (14.4, 11.3)]


def _walker(sc, name, model, points, speed):
    sc.render.fps = WALK_FPS
    root, gait = walkers.build_rigged_walker(name, model)
    last = walkers.animate_rigged_walk(root, gait, points, hall.floor_height, WALK_FPS, speed)
    return [root], (1, last)


def _walker_platform(sc):
    return _walker(sc, "WalkerPlatform", "blue_jacket_worker_idle_3.glb", PLATFORM_PATH,
                   speed=1.3)


def _walker_door(sc):
    return _walker(sc, "WalkerDoor", "ironclad_wanderer_chair_sit_idle_f.glb", DOOR_PATH,
                   speed=1.2)


# name -> builder(scene) -> (actor roots, (first frame, last frame))
LAYERS = {
    "walker_platform": _walker_platform,
    "walker_door": _walker_door,
}


def _build_plaza():
    sc = hall.reset()
    hall.setup_render(sc)
    cam = hall.add_plate_camera(sc)
    hall.attach_plate_reference(cam, str(NEWDETROIT.plate))
    decks = hall.add_deck(sc)
    hall.add_occluders(sc)
    hall.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_plaza()
    roots, frames = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = frames
    return sc, decks, roots


def check(out_png):
    """Holdouts, kerb and walker paths from the matched camera, over the plate."""
    sc = hall.reset()
    hall.setup_render(sc, samples=4)
    sc.view_settings.exposure = 0.0
    sc.render.film_transparent = True
    sc.render.use_motion_blur = False
    hall.add_plate_camera(sc)
    glow = {c: hall.material(f"Guide{c}", (0, 0, 0), emission=rgb, strength=0.6)
            for c, rgb in (("hold", (1.0, 0.2, 0.8)), ("kerb", (1.0, 0.2, 0.2)),
                           ("path", (0.2, 1.0, 0.3)))}
    for obj in hall.add_occluders(sc):
        obj.is_holdout = False
        obj.data.materials[0] = glow["hold"]
    hall.box_object("Kerb", (0.08, 60.0, 0.01), (hall.KERB_X, 30.0, hall.PLATFORM_Z),
                    glow["kerb"], sc.collection)
    for i, points in enumerate((PLATFORM_PATH, DOOR_PATH)):
        for j, ((x0, y0), (x1, y1)) in enumerate(pairwise(points)):
            n = 24
            for k in range(n + 1):
                x, y = x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n
                hall.box_object(f"Path{i}_{j}_{k}", (0.12, 0.12, 0.02),
                                (x, y, hall.floor_height(x, y) + 0.01), glow["path"],
                                sc.collection)
    guides = out_png.with_name("check_guides.png")
    sc.render.filepath = str(guides)
    bpy.ops.render.render(write_still=True)
    overlay_on_plate(NEWDETROIT.plate, guides, out_png)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="holdout/path overlay on the plate")
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--samples", type=int, help="override the beauty samples")
    args = ap.parse_args(argv)
    NEWDETROIT.build.mkdir(parents=True, exist_ok=True)
    if args.check:
        check(NEWDETROIT.build / "check.png")
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        if args.samples:
            sc.cycles.samples = args.samples
        render.render_passes(sc, decks, roots, NEWDETROIT.build / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

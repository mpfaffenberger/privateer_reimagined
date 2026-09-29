"""Render the raw passes for one animated New Con concourse layer (#515).

Run inside Blender (headless):
    blender --background --factory-startup \
        --python tools/room_anim/newcon/render_layers.py -- --layer all
    ... -- --layer walker_toward --frames 1:24     # quick look
    ... -- --review-blend                          # regenerate the review .blend

Writes each layer's passes (see ../render.py) to build/room_anim/newcon/<layer>/;
bake_layer.py --base newcon then turns them into plate-aware RGBA sprites.
"""
import argparse
import os
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
from base import REPO, paths  # noqa: E402

NEWCON = paths("newcon")
PLATE = NEWCON.plate
BUILD = NEWCON.build
REVIEW_BLEND = HERE / "newcon_concourse.blend"


def _car_receding(sc):
    car = actors.build_hover_car("CarReceding")
    actors.animate_straight_pass(car, x=-9.0, y_start=11.5, y_end=150.0, hover_z=0.9,
                                 frame_start=1, frame_end=240)
    return [car], (1, 240)


def _car_approaching(sc):
    """Kerb lane between the walkway (X -25) and the red stripe (X -21.6),
    coming toward the camera headlights-first and leaving frame-left (fully
    off-screen by Y~20 m; the cart holdout covers its last metres)."""
    car = actors.build_hover_car("CarApproaching", hull_rgb=(0.34, 0.29, 0.24))
    actors.animate_straight_pass(car, x=-23.2, y_start=150.0, y_end=19.0, hover_z=1.0,
                                 frame_start=1, frame_end=256, heading_deg=180.0)
    return [car], (1, 256)


WALK_FPS = 12      # plenty for a 30-60 px figure, and half the atlas of 24 fps


def _walker(sc, name, coat_rgb, start, end, speed):
    sc.render.fps = WALK_FPS
    # Walkers only touch the walkway. The glossy lane deck would mirror them
    # past the kerb, where the painted kerb and railings would block it.
    sc.objects["Deck"].hide_render = True
    root, limbs = actors.build_walker(name, coat_rgb)
    last = actors.animate_walk(root, limbs, start, end, hall.PROMENADE_Z, WALK_FPS,
                               speed=speed)
    return [root], (1, last)


def _walker_toward(sc):
    """Steps out from behind the kiosk pod, drifts toward the middle of the
    walkway and walks at the camera, leaving frame-left."""
    return _walker(sc, "WalkerToward", (0.16, 0.10, 0.06), start=(-30.5, 72.0),
                   end=(-28.0, 23.0), speed=1.3)


def _walker_away(sc):
    """Enters frame-left near the kerb and strolls away, easing over to the
    shopfronts and disappearing behind the kiosk pod."""
    return _walker(sc, "WalkerAway", (0.07, 0.09, 0.12), start=(-26.8, 23.0),
                   end=(-30.5, 72.0), speed=1.15)


# name -> builder(scene) -> (actor roots, (first frame, last frame))
LAYERS = {
    "car_receding": _car_receding,
    "car_approaching": _car_approaching,
    "walker_toward": _walker_toward,
    "walker_away": _walker_away,
}


def _build_hall():
    sc = hall.reset()
    hall.setup_render(sc)
    cam = hall.add_plate_camera(sc)
    hall.attach_plate_reference(cam, str(PLATE))
    decks = hall.add_deck(sc)
    hall.add_occluders(sc)
    hall.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_hall()
    roots, frames = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = frames
    return sc, decks, roots


def save_review_blend(path):
    """One scene with every layer's actors (a collection per layer) over the
    camera-matched hall, for inspecting the setup in Blender. Walker layers
    are keyed at their own 12 fps, so they move at half speed in this 24 fps
    scene. Renders always come from build(), never from this file."""
    sc, _ = _build_hall()
    last = 1
    for name, builder in LAYERS.items():
        before = set(bpy.data.objects)
        _, (_, end) = builder(sc)
        last = max(last, end)
        coll = bpy.data.collections.new(name)
        sc.collection.children.link(coll)
        for obj in set(bpy.data.objects) - before:
            for owner in obj.users_collection:
                owner.objects.unlink(obj)
            coll.objects.link(obj)
    sc.objects["Deck"].hide_render = False       # walker builders hide it
    sc.render.fps, sc.frame_start, sc.frame_end = 24, 1, last
    # Forward-slash relative plate path so the file opens on macOS too.
    rel = Path(os.path.relpath(PLATE, Path(path).parent)).as_posix()
    for img in bpy.data.images:
        img.filepath = f"//{rel}"
    bpy.ops.wm.save_as_mainfile(filepath=str(path), relative_remap=False, compress=True)
    Path(f"{path}1").unlink(missing_ok=True)   # generated file: no .blend1 backup


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--review-blend", action="store_true",
                    help=f"write {REVIEW_BLEND.relative_to(REPO)} (no rendering)")
    args = ap.parse_args(argv)
    if args.review_blend:
        save_review_blend(REVIEW_BLEND)
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        render.render_passes(sc, decks, roots, BUILD / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

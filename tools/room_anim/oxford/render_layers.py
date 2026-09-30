"""Render the raw passes for the animated Oxford concourse (#592).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/oxford/render_layers.py -- --check
    ... -- --layer all
    ... -- --layer aircar_avenue --frames 1:24 --samples 16     # quick look

--check asserts that the Blender camera projects exactly like
camera_match.py, which check_camera.py draws over the plate.

Every route in routes.json is one layer: an air-car or, #610, a pedestrian
(its "kind"). Passes (see ../render.py) go to build/room_anim/oxford/<layer>/; bake_layer.py --base oxford then turns
them into plate-aware RGBA sprites.
"""
import argparse
import json
import sys
from pathlib import Path

import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Vector

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import actors  # noqa: E402
import camera_match as cm  # noqa: E402
import render  # noqa: E402
import scene as town  # noqa: E402
from base import paths  # noqa: E402
from walkers import animate_walk_path  # noqa: E402

OXFORD = paths("oxford")
WALK_FPS = 12              # plenty for a ~40 px figure, half the atlas of 24 fps
ROUTES = {k: v for k, v in json.loads((HERE / "routes.json").read_text()).items()
          if not k.startswith("_")}


def _aircar(sc, name):
    route = ROUTES[name]
    car = actors.build_aircar(name, actors.PAINTS[route["paint"]])
    last = actors.animate_drive(car, route["uv"][0], route["uv"][-1], route["hover"],
                                route["speed"], sc.render.fps)
    return [car], (1, last)


def _walker(sc, name):
    route = ROUTES[name]
    sc.render.fps = WALK_FPS
    root, limbs = actors.build_pedestrian(name, route["look"])
    last = animate_walk_path(root, limbs, [cm.to_world(*p) for p in route["uv"]], 0.0,
                             WALK_FPS, speed=route["speed"])
    return [root], (1, last)


KINDS = {"aircar": _aircar, "walker": _walker}   # routes.json "kind" -> builder


def _build_town():
    sc = town.reset()
    town.setup_render(sc)
    cam = town.add_plate_camera(sc)
    town.attach_plate_reference(cam, str(OXFORD.plate))
    decks = town.add_deck(sc)
    town.add_occluders(sc)
    town.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_town()
    roots, frames = KINDS[ROUTES[layer]["kind"]](sc, layer)
    sc.frame_start, sc.frame_end = frames
    return sc, decks, roots


def check(tolerance_px=0.5):
    """The Blender camera must agree with camera_match.project()."""
    sc = town.reset()
    town.setup_render(sc, samples=1)
    town.add_plate_camera(sc)
    worst = 0.0
    for u in range(-60, 61, 20):
        for v in range(-20, 81, 20):
            for z in (0.0, 10.0):
                x, y = cm.to_world(u, v)
                px, py = cm.project(x, y, z)
                if not (0 <= px < cm.PLATE_W and 0 <= py < cm.PLATE_H):
                    continue
                ndc = world_to_camera_view(sc, sc.camera, Vector((x, y, z)))
                bx, by = ndc.x * cm.PLATE_W, (1.0 - ndc.y) * cm.PLATE_H
                worst = max(worst, abs(bx - px), abs(by - py))
    print(f"[render_layers] camera check: worst {worst:.3f} px", flush=True)
    if worst > tolerance_px:
        raise SystemExit(f"Blender camera disagrees with camera_match by {worst:.2f} px")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="Blender camera vs camera_match")
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(ROUTES)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--samples", type=int, help="override the beauty samples")
    args = ap.parse_args(argv)
    if args.check:
        check()
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(ROUTES) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        if args.samples:
            sc.cycles.samples = args.samples
        render.render_passes(sc, decks, roots, OXFORD.build / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

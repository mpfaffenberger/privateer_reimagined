"""Render the raw passes for the Pleasure concourse walkers (#598).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/pleasure/render_layers.py -- --check
    ... -- --layer all
    ... -- --layer walker_ranger --frames 60:60        # quick look

--check renders the holdouts (couches, tables, the planter, the arch jamb)
and the walkers' lanes from the matched camera over the plate:
build/room_anim/pleasure/check.png.

Layers write their passes (see ../render.py) to build/room_anim/pleasure/<layer>/;
bake_layer.py --base pleasure then turns them into plate-aware RGBA sprites.
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

PLEASURE = paths("pleasure")
WALK_FPS = 12      # plenty for a ~175 px figure at walking pace (New Detroit's too)

# Lanes, as the plate pixel under the walker's feet (scene.floor_point maps
# them to the floor). Out from behind the bar arch's left jamb, through the
# opening, then along the carpet behind the couches (feet at y 814), easing
# forward past the landing-pad door (its threshold is nearer than the back
# wall) and off behind the planter and pillar.
RANGER_LANE = [(300, 790), (415, 800), (455, 814), (1000, 814), (1300, 830), (1620, 830)]
# The other way on a lane farther back (y 806), so it's drawn first. In
# through the arch and off behind its jamb. The two never meet (layers.json).
ENFORCER_LANE = [(1620, 825), (1330, 825), (1040, 806), (470, 806), (400, 797), (290, 789)]


def _floor(lane):
    return [hall.floor_point(px, py) for px, py in lane]


def _walker(sc, name, model, lane, speed):
    sc.render.fps = WALK_FPS
    root, gait = walkers.build_rigged_walker(name, model)
    last = walkers.animate_rigged_walk(root, gait, _floor(lane), 0.0, WALK_FPS, speed)
    return [root], (1, last)


def _walker_ranger(sc):
    """A pilot in an orange flight suit on shore leave."""
    return _walker(sc, "WalkerRanger", "rustbound_ranger_sit_cross_legged.glb", RANGER_LANE,
                   speed=1.25)


def _walker_enforcer(sc):
    """A bald heavy in a grey leather jacket, in no hurry."""
    return _walker(sc, "WalkerEnforcer", "weathered_enforcer_sitting_answering_questions.glb",
                   ENFORCER_LANE, speed=1.1)


# name -> builder(scene) -> (actor roots, (first frame, last frame))
LAYERS = {
    "walker_ranger": _walker_ranger,
    "walker_enforcer": _walker_enforcer,
}


def _build_hall():
    sc = hall.reset()
    hall.setup_render(sc)
    cam = hall.add_plate_camera(sc)
    hall.attach_plate_reference(cam, str(PLEASURE.plate))
    decks = hall.add_deck(sc)
    hall.add_occluders(sc)
    hall.add_lights(sc)
    return sc, decks


def build(layer):
    sc, decks = _build_hall()
    roots, frames = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = frames
    return sc, decks, roots


def check(out_png):
    """Holdouts and lanes from the matched camera, over the plate."""
    sc = hall.reset()
    hall.setup_render(sc, samples=4)
    sc.view_settings.exposure = 0.0
    sc.render.film_transparent = True
    sc.render.use_motion_blur = False
    hall.add_plate_camera(sc)
    glow = {c: hall.material(f"Guide{c}", (0, 0, 0), emission=rgb, strength=0.6)
            for c, rgb in (("hold", (1.0, 0.2, 0.8)), ("lane", (0.2, 1.0, 0.3)))}
    for obj in hall.add_occluders(sc):
        obj.is_holdout = False
        obj.data.materials[0] = glow["hold"]
    for i, lane in enumerate((RANGER_LANE, ENFORCER_LANE)):
        for j, ((x0, y0), (x1, y1)) in enumerate(pairwise(_floor(lane))):
            n = 24
            for k in range(n + 1):
                x, y = x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n
                hall.box_object(f"Lane{i}_{j}_{k}", (0.1, 0.1, 0.02), (x, y, 0.01),
                                glow["lane"], sc.collection)
    # A 1.75 m post at each lane's middle waypoints: the walkers' height.
    for i, lane in enumerate((RANGER_LANE, ENFORCER_LANE)):
        for j, (x, y) in enumerate(_floor(lane)[1:-1]):
            hall.box_object(f"Height{i}_{j}", (0.06, 0.06, 1.75), (x, y, 0.875), glow["lane"],
                            sc.collection)
    guides = out_png.with_name("check_guides.png")
    sc.render.filepath = str(guides)
    bpy.ops.render.render(write_still=True)
    overlay_on_plate(PLEASURE.plate, guides, out_png)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="holdout/lane overlay on the plate")
    ap.add_argument("--layer", nargs="+", default=[], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last override, e.g. 1:24")
    ap.add_argument("--samples", type=int, help="override the beauty samples")
    args = ap.parse_args(argv)
    PLEASURE.build.mkdir(parents=True, exist_ok=True)
    if args.check:
        check(PLEASURE.build / "check.png")
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, decks, roots = build(layer)
        if args.samples:
            sc.cycles.samples = args.samples
        render.render_passes(sc, decks, roots, PLEASURE.build / layer, frames)
        print(f"[render_layers] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

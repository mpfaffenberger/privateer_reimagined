"""Render a pair of Confed Stilettos crossing beyond the Military concourse's window (#588).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/military/render_flyby.py -- --frames 1:150:25 --samples 8
    ... --                                  # the full pass

The original game flew a fighter pair past this window (the legacy
concourse_sh0 overlay). Out in space there is no floor to encode against, so
this is a plain straight-alpha pass from the matched plate camera
(scene.add_plate_camera). bake_sky.py packs it as an "under" layer, drawn
between the stars and the plate, so the window's lattice and pillar occlude
the ships exactly where the painting says.

The path is solved from screen targets (_path): the leader's centre enters
off the left edge at y ~320 and leaves past the pillar and the slot beside
it at y ~390, through the middle row of panes, closing from 230 m to 190 m,
so it descends and grows a little. The wingman trails
back and above, the legacy overlay's echelon.

Writes build/room_anim/military/fighter_pair/:
    NNNN.png   straight-alpha RGBA, rendered inside a border around the pair
    pass.json  {"frames", "fps"}
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

import render  # noqa: E402
import scene as hall  # noqa: E402
import ships  # noqa: E402
import stage  # noqa: E402
from base import paths  # noqa: E402

MILITARY = paths("military")
LAYER = "fighter_pair"
FPS = 24
PASS_SECONDS = 6.5

LENGTH = 20.0                       # m; ~160-200 px long at these depths
# Leader's screen path: (plate x, plate y, depth m) at the start and the end.
# Through the middle row of panes (transoms at y ~203 and ~450), the wingman
# ~60 px above the leader, clear of both.
ENTER = (-220.0, 320.0, 230.0)
LEAVE = (1750.0, 400.0, 190.0)   # off the right edge: the trailing wingman
                                  # must clear the slot beside the pillar
WINGMAN = Vector((-14.0, -34.0, 7.0))   # m, in the leader's frame (right, fwd, up)
BANK = 12.0                         # deg, rolled toward the camera: undersides show

SUN = (1.0, 0.95, 0.88)
STATION_STEEL = (0.22, 0.26, 0.32)  # bounce off the base's gunmetal hull
ENGINE = (0.6, 0.8, 1.0)
NAV_RED, NAV_GREEN = (1.0, 0.1, 0.05), (0.1, 1.0, 0.3)


def _world_point(px, py, depth):
    """World point that the plate camera sees at pixel (px, py), `depth` m out."""
    vx, vy = hall.VANISHING_POINT
    return Vector(((px - vx) * depth / hall.FOCAL_PX, depth,
                   hall.EYE_HEIGHT - (py - vy) * depth / hall.FOCAL_PX))


def _path():
    return _world_point(*ENTER), _world_point(*LEAVE)


def _lights(sc):
    world = bpy.data.worlds.new("Space")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (*STATION_STEEL, 1.0)
    sc.world = world
    # Sun high, ahead and to the left, so the side facing the window is lit.
    stage.light(sc, "Sun", 'SUN', (0.0, 0.0, 0.0), SUN, 8.0,
                rot=(math.radians(-50.0), math.radians(-30.0), 0.0))


def _fighter(name):
    ship = ships.import_ship("stiletto", LENGTH, name)
    w, length, h = ship["size"]
    for side in (-1, 1):
        stage.emitter(f"{name}Engine{side}", ship, (side * w * 0.1, -length / 2, 0.0), 0.45,
                      ENGINE, 40.0)
    stage.emitter(f"{name}NavPort", ship, (-w / 2, -length * 0.2, 0.0), 0.3, NAV_RED, 30.0)
    stage.emitter(f"{name}NavStarboard", ship, (w / 2, -length * 0.2, 0.0), 0.3, NAV_GREEN, 30.0)
    ship.rotation_mode = 'QUATERNION'
    return ship


def build(samples):
    sc = hall.reset()
    hall.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    frames = round(PASS_SECONDS * FPS)
    sc.frame_start, sc.frame_end = 1, frames
    hall.add_plate_camera(sc)
    _lights(sc)
    start, end = _path()
    travel = (end - start).normalized()
    # Nose (+Y) along the travel, top (+Z) up, then banked about the nose.
    attitude = (Quaternion(travel, math.radians(BANK)) @ travel.to_track_quat('Y', 'Z'))
    leader, wingman = _fighter("Leader"), _fighter("Wingman")
    for f in range(1, frames + 1):
        t = (f - 1) / (frames - 1)
        pos = start.lerp(end, t)
        for ship, offset in ((leader, Vector()), (wingman, WINGMAN)):
            ship.location = pos + attitude @ offset
            ship.rotation_quaternion = attitude
            ship.keyframe_insert("location", frame=f)
            ship.keyframe_insert("rotation_quaternion", frame=f)
    return sc, [leader, wingman], frames


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", help="first:last[:step], e.g. 1:150:25 for a quick look")
    ap.add_argument("--samples", type=int, default=32)
    args = ap.parse_args(argv)
    sc, roots, frames = build(args.samples)
    out = MILITARY.build / LAYER
    out.mkdir(parents=True, exist_ok=True)
    if args.frames:
        first, last, *step = (int(v) for v in args.frames.split(":"))
        todo = range(first, last + 1, step[0] if step else 1)
    else:
        for old in out.glob("*.png"):           # off-screen frames write nothing,
            old.unlink()                        # so stale ones must not survive
        todo = range(1, frames + 1)
    for f in todo:
        sc.frame_set(f)
        if not render.set_border(sc, roots, margin=0.15, footprint=False):
            continue
        sc.render.filepath = str(out / f"{f:04d}.png")
        bpy.ops.render.render(write_still=True)
    (out / "pass.json").write_text(json.dumps({"frames": frames, "fps": FPS}) + "\n")
    print(f"[render_flyby] {LAYER} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

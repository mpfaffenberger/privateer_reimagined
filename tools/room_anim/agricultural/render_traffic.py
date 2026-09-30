"""Render the aircraft seen through the Agricultural concourse windows (#582).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/agricultural/render_traffic.py -- --layer all
    ... -- --layer aircar_crossing --frames 1:160:20 --samples 8    # quick look

The windows look out over farmland at dusk: a glass farm dome, a lit
settlement, far ridges. Nothing outside is near enough to match in 3D, so the
camera is only matched to the horizon (flight.py): level, f = FOCAL_PX, the
far ridges at plate y HORIZON_Y, and each pass is solved from screen
targets.
The farmland lies EYE_HEIGHT below the camera: at that height the farm dome
(its base at y ~255, 500 px wide) is ~370 m across and ~45 m tall.

Lighting is the painting's dusk: the sun just down behind the dome, so hulls
are backlit, warm along their top edges, and lit from the front only by the
dim sky. Nav lights keep them readable.

Writes build/room_anim/agricultural/traffic/<layer>/:
    NNNN.png   straight-alpha RGBA inside a border around the craft
    pass.json  {"frames", "fps", "depth": [m per frame]}; bake_traffic.py
               hazes each frame by its depth and clips it to the glass.
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

import flight  # noqa: E402
import ships  # noqa: E402
import stage  # noqa: E402
from base import paths  # noqa: E402

AGRI = paths("agricultural")
BUILD = AGRI.build / "traffic"
FPS = 24
FOCAL_PX = 1000.0
HORIZON_Y = 172.0                   # plate px: the far ridges
EYE_HEIGHT = 60.0                   # m above the farmland

SUNSET = (1.0, 0.62, 0.36)          # the glow behind the dome
DUSK_SKY = (0.075, 0.07, 0.068)     # what lights the camera-facing sides
NAV_RED, NAV_GREEN, STROBE = (1.0, 0.1, 0.05), (0.1, 1.0, 0.3), (1.0, 1.0, 1.0)
ENGINE = (1.0, 0.72, 0.42)
HEADLAMP = (1.0, 0.9, 0.72)


def _lights(sc):
    world = bpy.data.worlds.new("Dusk")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (*DUSK_SKY, 1.0)
    sc.world = world
    # Just below the horizon, dead ahead: -Z (the light's direction) points
    # back at the camera and a touch down, so only upward and far-side
    # faces catch it. Grazing, so it's strong.
    stage.light(sc, "Sunset", 'SUN', (0.0, 0.0, 0.0), SUNSET, 6.0,
                rot=(math.radians(-86.0), 0.0, math.radians(-5.0)))


def _solve(screen, depth):
    return flight.solve(screen, depth, FOCAL_PX, HORIZON_Y)


def _freighter_departure(sc):
    """A Galaxy freighter, laden with the harvest, climbs out from the
    spaceport hidden right of the windows and heads off to the left, away
    and up, shrinking into the dusk (111 px long -> 55 px)."""
    frames = round(22.0 * FPS)
    ship = ships.import_ship("mrchship", 100.0, "Galaxy")
    w, length, h = ship["size"]
    for i, x in enumerate((-w / 4, w / 4)):
        stage.emitter(f"Engine{i}", ship, (x, -length / 2, 0.0), 2.6, ENGINE, 40.0)
    stage.emitter("NavPort", ship, (-w / 2, 0.0, 0.0), 1.4, NAV_RED, 40.0)
    stage.emitter("NavStarboard", ship, (w / 2, 0.0, 0.0), 1.4, NAV_GREEN, 40.0)
    flight.strobe(stage.emitter("Strobe", ship, (0.0, 0.0, h / 2), 1.6, STROBE, 80.0),
                  frames, FPS, 1.5)
    depths = flight.fly(ship, _solve((1510.0, 170.0), 900.0), _solve((560.0, 95.0), 1900.0),
                        frames, ease_in=0.45, bank_deg=-8.0)
    return ship, frames, depths


def _aircar_crossing(sc):
    """A farmhand's yellow aircar skims left to right across the fields in
    front of the dome, dipping toward the settlement, headlamps on."""
    frames = round(7.0 * FPS)
    car = ships.import_ship("aircar", 7.0, "Aircar")
    w, length, h = car["size"]
    for i, x in enumerate((-w / 5, w / 5)):
        stage.emitter(f"Headlamp{i}", car, (x, length / 2, 0.0), 0.28, HEADLAMP, 60.0)
    stage.emitter("Tail", car, (0.0, -length / 2, 0.0), 0.3, NAV_RED, 30.0)
    flight.strobe(stage.emitter("Beacon", car, (0.0, 0.0, h / 2), 0.3, (1.0, 0.55, 0.1), 60.0),
                  frames, FPS, 0.8, on_s=0.15)
    depths = flight.fly(car, _solve((560.0, 196.0), 160.0), _solve((1530.0, 204.0), 160.0),
                        frames, bank_deg=6.0)
    return car, frames, depths


# name -> builder(scene) -> (craft root, frames, depth per frame)
LAYERS = {"freighter_departure": _freighter_departure, "aircar_crossing": _aircar_crossing}


def build(layer, samples):
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    flight.add_camera(sc, "WindowCam", FOCAL_PX, HORIZON_Y)
    _lights(sc)
    root, frames, depths = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = 1, frames
    return sc, root, frames, depths


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", nargs="+", required=True, choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last[:step], e.g. 1:160:20 for a quick look")
    ap.add_argument("--samples", type=int, default=32)
    args = ap.parse_args(argv)
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, root, frames, depths = build(layer, args.samples)
        flight.render_pass(sc, root, BUILD / layer, frames, FPS, args.frames, depth=depths)
        print(f"[render_traffic] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

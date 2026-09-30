"""Render the aircraft over the Agricultural landing pad (#583).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/agricultural/render_landing.py -- --layer all
    ... -- --layer transport_crossing --frames 1:432:48 --samples 8    # quick look

The landing pad is 18 differently framed composites (bake_landing.py), so
nothing is matched to one painting. The passes are rendered for a CANONICAL
framing, tarsus, and the engine slides every frame by the big moon's offset
on each composite (anchors.json; translation only, see bake_landing.py).
The camera is level with f = 1024 px and tarsus's far shore on the horizon
(flight.py); paths are solved from tarsus screen targets:

* transport_crossing: a transport (`transprt`) crosses from the right,
  behind the right pylon, the brick tower and the left pylon, sinking a
  little toward a far pad.
* freighter_arrival: a Galaxy (`mrchship`) comes in high on the right and
  descends away from us, sliding down behind the tower's top.

Both stay clear of the moons, whose painted discs are in the sky fill.

Writes build/room_anim/agricultural/landing/<layer>/:
    NNNN.png   straight-alpha RGBA, rendered inside a border around the ship
    pass.json  {"frames", "fps", "anchor"}
"""
import argparse
import json
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
BUILD = AGRI.build / "landing"
FPS = 24
FOCAL_PX = 1024.0
CANONICAL = "tarsus"                # composite the passes are rendered against
HORIZON_Y = 505.0                   # tarsus: the dark far shore across the lake

# Dusk: the sun is down, the sky above is a bright blue dome and the horizon
# glows pink. Hulls are lit mostly by the sky, with a pink low key.
SKY_DOME = (0.09, 0.08, 0.22)
HORIZON_GLOW = (1.0, 0.55, 0.75)
NAV_RED, NAV_GREEN, STROBE = (1.0, 0.1, 0.05), (0.1, 1.0, 0.3), (1.0, 1.0, 1.0)
ENGINE = (0.75, 0.8, 1.0)
LANDING_LAMP = (1.0, 0.92, 0.8)


def _lights(sc):
    world = bpy.data.worlds.new("DuskDome")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (*SKY_DOME, 1.0)
    sc.world = world
    # Low, from behind and to the left: grazes the hulls' far and upper sides.
    stage.light(sc, "HorizonGlow", 'SUN', (0.0, 0.0, 0.0), HORIZON_GLOW, 2.5,
                rot=(math.radians(-80.0), 0.0, math.radians(30.0)))


def _nav_lights(ship, frames, strobe_every):
    w, _, h = ship["size"]
    stage.emitter("NavPort", ship, (-w / 2, 0.0, 0.0), w * 0.03, NAV_RED, 40.0)
    stage.emitter("NavStarboard", ship, (w / 2, 0.0, 0.0), w * 0.03, NAV_GREEN, 40.0)
    flight.strobe(stage.emitter("Strobe", ship, (0.0, 0.0, -h / 2), w * 0.035, STROBE, 80.0),
                  frames, FPS, strobe_every)


def _path(screen_from, screen_to, depths):
    """Tarsus screen targets at `depths` m -> world start and end."""
    return [flight.solve(s, d, FOCAL_PX, HORIZON_Y)
            for s, d in zip((screen_from, screen_to), depths)]


def _transport_crossing():
    frames = round(18.0 * FPS)
    ship = ships.import_ship("transprt", 80.0, "Transport")
    _, length, _ = ship["size"]
    stage.emitter("Engine", ship, (0.0, -length / 2, 0.0), 2.2, ENGINE, 30.0)
    _nav_lights(ship, frames, 1.2)
    start, end = _path((1620.0, 285.0), (-90.0, 330.0), (1000.0, 1000.0))
    flight.fly(ship, start, end, frames, bank_deg=4.0)
    return ship, frames


def _freighter_arrival():
    frames = round(20.0 * FPS)
    ship = ships.import_ship("mrchship", 100.0, "Galaxy")
    w, length, h = ship["size"]
    for i, x in enumerate((-w / 4, w / 4)):
        stage.emitter(f"Engine{i}", ship, (x, -length / 2, 0.0), 2.4, ENGINE, 25.0)
    stage.emitter("LandingLamp", ship, (0.0, length / 3, -h / 2), 2.0, LANDING_LAMP, 60.0)
    _nav_lights(ship, frames, 1.5)
    start, end = _path((1600.0, 70.0), (1000.0, 250.0), (1700.0, 3600.0))
    flight.fly(ship, start, end, frames, ease_in=-0.4)       # slowing on approach
    return ship, frames


# name -> builder() -> (ship root, frames)
LAYERS = {"transport_crossing": _transport_crossing, "freighter_arrival": _freighter_arrival}


def build(layer, samples):
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    flight.add_camera(sc, "PadCam", FOCAL_PX, HORIZON_Y)
    _lights(sc)
    ship, frames = LAYERS[layer]()
    sc.frame_start, sc.frame_end = 1, frames
    return sc, ship, frames


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", nargs="+", required=True, choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last[:step], e.g. 1:432:48 for a quick look")
    ap.add_argument("--samples", type=int, default=32)
    args = ap.parse_args(argv)
    anchor = json.loads((AGRI.anim / "landing" / "anchors.json").read_text())[CANONICAL]
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, ship, frames = build(layer, args.samples)
        flight.render_pass(sc, ship, BUILD / layer, frames, FPS, args.frames, anchor=anchor)
        print(f"[render_landing] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

"""Render ships outside the Refinery landing pad's hangar door (#585).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/refinery/render_landing.py -- --layer all
    ... -- --layer tug_liftoff --frames 1:432:48 --samples 8     # quick look

The landing pad is 18 differently framed composites (bake_landing.py), so
nothing is matched to one painting: the passes are rendered for a CANONICAL
framing, tarsus (flyover.CANONICAL), whose door anchor [cx, cy, r] is read
from bake_landing.py's anchors.json (run that first), and the engine moves
and scales every frame onto each composite's own door. Paths are planned in door units, u across
(-1 left lamp, +1 right lamp) and v down from the lamps in half-widths, so
they land in the doorway: beyond it is a dark city under the stars, whose
skyline tops out at v ~0.7 on tarsus.

The stage, render loop and pass.json are flyover.py's (#587): its level
f = 1024 px camera, run() and, through `lights=`, this pad's own night
lighting.

    transport_pass   a transport crossing high and far, side on (the legacy
                     overlay's drifting lights were the only life out there)
    tug_liftoff      a heavy tug rising from behind the skyline, then
                     climbing away to the right, engines glowing

Writes build/room_anim/refinery/landing/<layer>/ (flyover.render_frames()).
"""
import argparse
import math
import sys
from pathlib import Path

import bpy

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import flyover  # noqa: E402
import ships  # noqa: E402
import stage  # noqa: E402

CITY_GLOW = (1.0, 0.72, 0.45)       # sodium light from the city below
STARLIGHT = (0.6, 0.72, 1.0)        # cool rim from the sky
ENGINE = (0.55, 0.75, 1.0)          # the tug's exhausts


def _at(u, v, depth):
    """World point `depth` m out that lands on the canonical door's coords
    (u, v): flyover's level camera, f = FOCAL_PX."""
    cx, cy, r = flyover.anchor("refinery")
    px, py = cx + u * r, cy + v * r
    return ((px - stage.PLATE_W / 2) * depth / flyover.FOCAL_PX, depth,
            (stage.PLATE_H / 2 - py) * depth / flyover.FOCAL_PX)


def _smooth(t):
    return t * t * (3.0 - 2.0 * t)


def _key_path(ship, points, frames, yaw_deg, pitch_deg=(0.0, 0.0)):
    """Fly through `points` [(t, (u, v, depth)), ...] (t from 0 to 1),
    easing in and out of each, with the pitch blending across the pass."""
    ship.rotation_mode = 'XYZ'
    for f in range(1, frames + 1):
        t = (f - 1) / (frames - 1)
        i = max(k for k, (tk, _) in enumerate(points[:-1]) if tk <= t)
        (t0, p0), (t1, p1) = points[i], points[i + 1]
        s = _smooth((t - t0) / (t1 - t0))
        ship.location = _at(*(a + (b - a) * s for a, b in zip(p0, p1)))
        pitch = pitch_deg[0] + (pitch_deg[1] - pitch_deg[0]) * _smooth(t)
        ship.rotation_euler = (math.radians(pitch), 0.0, math.radians(yaw_deg))
        ship.keyframe_insert("location", frame=f)
        ship.keyframe_insert("rotation_euler", frame=f)


def _transport_pass(frames, rim_y):
    """Left to right across the top of the doorway, 1.2 km out: ~70 px long."""
    ship = ships.import_ship("transprt", 70.0, "Transport")
    flyover.hull_nav_lights(ship, frames, every_s=2.0)
    _key_path(ship, [(0.0, (-1.4, 0.24, 1200.0)), (1.0, (1.4, 0.16, 1200.0))], frames,
              yaw_deg=-90.0)
    return [ship]


def _tug_liftoff(frames, rim_y):
    """Straight up from behind the city (hidden below the skyline), a
    hover, then nose up and away past the right-hand beam, shrinking."""
    ship = ships.import_ship("dd_tug", 30.0, "Tug")
    w, length, h = ship["size"]
    flyover.hull_nav_lights(ship, frames, every_s=1.2, phase_s=0.5)
    for side in (-1, 1):
        for inner in (0.18, 0.4):
            stage.emitter(f"Exhaust{side}{inner}", ship, (side * w * inner, -length / 2, 0.0),
                          0.06 * length, ENGINE, 40.0)
    _key_path(ship, [(0.0, (0.28, 1.05, 520.0)), (0.4, (0.3, 0.45, 540.0)),
                     (1.0, (1.35, 0.02, 1150.0))], frames,
              yaw_deg=-55.0, pitch_deg=(0.0, 14.0))
    return [ship]


# name -> (pass seconds, flyover build_ships(frames, rim_y) -> roots)
LAYERS = {"transport_pass": (21.0, _transport_pass), "tug_liftoff": (18.0, _tug_liftoff)}


def _lights(sc):
    """Night over the city, for flyover.build(lights=)."""
    world = bpy.data.worlds.new("Night")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.02, 0.022, 0.03, 1.0)
    sc.world = world
    stage.light(sc, "CityGlow", 'SUN', (0.0, 0.0, 0.0), CITY_GLOW, 1.4,
                rot=(math.radians(150.0), 0.0, math.radians(20.0)))     # from below, front
    stage.light(sc, "Starlight", 'SUN', (0.0, 0.0, 0.0), STARLIGHT, 2.0,
                rot=(math.radians(-30.0), 0.0, math.radians(160.0)))    # from above, behind


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", nargs="+", default=["all"], choices=["all", *sorted(LAYERS)])
    args, rest = ap.parse_known_args(argv)          # --frames / --samples: flyover.run()
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        seconds, build_ships = LAYERS[layer]
        flyover.run(rest, "refinery", layer, seconds, build_ships, __doc__.splitlines()[0],
                    lights=_lights)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

"""Render a ship lifting off beyond the Oxford landing pad (#593).

Run inside Blender (headless), after bake_landing.py (it reads anchors.json):
    blender --background --factory-startup \\
        --python tools/room_anim/oxford/render_landing.py -- --frames 1:240:24 --samples 8
    ... --                                  # the full pass

The stage, camera (level, f = 1024 px), nav lights and render loop are the
crater pads' (../flyover.py). What's Oxford's: the dusk light, and the path.

The landing pad is 18 differently framed composites, so nothing is matched
to one painting: the pass is rendered for flyover's CANONICAL framing
(tarsus) and anchored on its big moon, which bake_landing.py finds in every
composite. The engine moves and scales each frame from that moon onto each
composite's own, so the ship keeps its place against the moons everywhere,
and everything nearer (hangars, hills, the castle) hides it through the sky
mask.

The ship is the original game's Oxford ship (ships_wcnews/oxship.obj). It
climbs away in a straight line from behind the hangar roofs to off the left
edge, left of and below the moons, shrinking as it goes: the beat of a
departure seen from the pad. Unlike flyover.fly_straight() it climbs, so the
path is a 3D line solved from screen targets (PATH_PX) at two depths; a 3D
line projects to a straight screen path, so the whole climb stays clear of
the moons if its ends do (see _path).
"""
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

LAYER = "oxship_departure"
LENGTH = 40.0                       # m
# Screen targets on tarsus (moon at 815, 180, r 141), px: the ship's centre
# starts under the hangar roof line (hidden until it climbs past it) and
# leaves off the left edge over the castle, well clear of the moons.
# From 2 km out it was a speck that read as a bird; and receding 5x in
# depth front-loads the screen motion (70% of it in the first 30% of the
# pass: a zip, then gone). Doubling the depth keeps it even, ~43 m/s.
PATH_PX = ((600.0, 560.0), (-120.0, 150.0))
DEPTH = (200.0, 420.0)              # m: ~200 px long at the start, ~95 at the end
PASS_SECONDS = 10.0

# Dusk: the sun has just set ahead of the camera (the sky is orange low
# down), so the ship is rim-lit from beyond and filled by the purple sky.
SUN = (1.0, 0.55, 0.3)
DUSK_SKY = (0.10, 0.08, 0.16)
ENGINE = (1.0, 0.55, 0.25)


def _dusk(sc):
    world = bpy.data.worlds.new("Dusk")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (*DUSK_SKY, 1.0)
    sc.world = world
    # Low (8 deg), from ahead and to the right: a warm rim on the climbing hull.
    stage.light(sc, "Sun", 'SUN', (0.0, 0.0, 0.0), SUN, 4.0,
                rot=(math.radians(-82.0), 0.0, math.radians(200.0)))


def _path():
    """-> (start, end) world points putting the ship's centre on PATH_PX at
    DEPTH: x = (px - 768) d / f, z = (512 - py) d / f."""
    f = flyover.FOCAL_PX
    return [((px - stage.PLATE_W / 2) * d / f, d, (stage.PLATE_H / 2 - py) * d / f)
            for (px, py), d in zip(PATH_PX, DEPTH)]


def _ship(frames, _anchor_cy):
    """The departing ship (the path is fixed in tarsus's pixels, so the
    anchor height flyover passes in isn't needed)."""
    ship = ships.import_ship("oxship", LENGTH, "OxShip")
    w, length, _ = ship["size"]
    flyover.nav_lights(ship, frames, 0.5, 30.0, 0.5, 60.0, strobe_every_s=1.2)
    # The engines face the camera as it climbs away: they're what reads as
    # it shrinks, so they glow hot.
    for side in (-1, 1):
        stage.emitter(f"Engine{side}", ship, (side * w * 0.12, -length / 2, 0.0), 0.9,
                      ENGINE, 80.0)
    (x0, y0, z0), (x1, y1, z1) = _path()
    dx, dy, dz = x1 - x0, y1 - y0, z1 - z0
    # Nose along the travel: yaw toward it, then pitch up by the climb angle.
    ship.rotation_euler = (math.atan2(dz, math.hypot(dx, dy)), 0.0, math.atan2(-dx, dy))
    for f in range(1, frames + 1):
        t = (f - 1) / (frames - 1)
        ship.location = (x0 + dx * t, y0 + dy * t, z0 + dz * t)
        ship.keyframe_insert("location", frame=f)
    return [ship]


if __name__ == "__main__":
    flyover.run(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [],
                "oxford", LAYER, PASS_SECONDS, _ship, doc=__doc__.splitlines()[0],
                lights=_dusk)

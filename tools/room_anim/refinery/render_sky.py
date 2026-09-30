"""Render the ships crossing the Refinery concourse's sky (#584).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/refinery/render_sky.py -- --layer all
    ... -- --layer hauler_pass --frames 1:720:90 --samples 8     # quick look

The original concourse had small ships crossing the dome (its legacy sh0/sh1
overlays). Two passes, straight alpha from the plate camera (scene.py), drawn
*under* the plate by bake_sky.py --layers-only so the arches, the refinery's
towers and the hanging cable occlude them:

    hauler_pass    a Galaxy freighter far out, crossing the whole dome left
                   to right above the refinery, backlit by the painted sun
    shuttle_in     a Demon dropping in from the top right and down behind the
                   refinery's towers, as if landing there

Paths are planned in plate pixels at a distance (scene.plate_point), so they
land where the windows are. Writes build/room_anim/refinery/sky/<layer>/.
"""
import argparse
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import flyover  # noqa: E402
import scene as hall  # noqa: E402
import ships  # noqa: E402
import stage  # noqa: E402
from base import paths  # noqa: E402

REFINERY = paths("refinery")
BUILD = REFINERY.build / "sky"
FPS = flyover.FPS

SUN_PX = (75.0, 200.0)              # the painted sun
SUN = (1.0, 0.86, 0.66)             # its warm white-gold glare
REFINERY_GLOW = (1.0, 0.7, 0.42)    # sodium light from the plant below


def _fly(ship, start, end, frames, pitch_deg=0.0):
    """Constant-speed straight flight, nose along the travel."""
    (x0, y0, z0), (x1, y1, z1) = start, end
    heading = math.atan2(-(x1 - x0), y1 - y0)
    ship.rotation_euler = (math.radians(pitch_deg), 0.0, heading)
    for f in range(1, frames + 1):
        t = (f - 1) / (frames - 1)
        ship.location = (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, z0 + (z1 - z0) * t)
        ship.keyframe_insert("location", frame=f)


def _hauler_pass(sc):
    """~46 px long at 2.4 km, rising gently across the whole width: behind
    the near arch, through the middle window, behind the far arch and the
    refinery's tall towers, and out the right edge."""
    frames = 30 * FPS
    ship = ships.import_ship("mrchship", 100.0, "Galaxy")
    flyover.hull_nav_lights(ship, frames)
    _fly(ship, hall.plate_point(-140.0, 100.0, 2400.0), hall.plate_point(1700.0, 50.0, 2400.0),
         frames)
    return ship, frames


def _shuttle_in(sc):
    """In from above the top-right corner, sinking away toward the plant,
    and gone behind its towers before it lands."""
    frames = 14 * FPS
    ship = ships.import_ship("demon", 26.0, "Shuttle")
    flyover.hull_nav_lights(ship, frames, every_s=1.1, phase_s=0.4)
    _fly(ship, hall.plate_point(1600.0, -30.0, 700.0), hall.plate_point(1040.0, 190.0, 1300.0),
         frames, pitch_deg=-6.0)
    return ship, frames


# name -> builder(scene) -> (ship root, frames)
LAYERS = {"hauler_pass": _hauler_pass, "shuttle_in": _shuttle_in}


def _lights(sc):
    world = bpy.data.worlds.new("Space")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.03, 0.028, 0.03, 1.0)
    sc.world = world
    toward_sun = Vector(hall.plate_ray(*SUN_PX))
    sun = stage.light(sc, "Sun", 'SUN', (0.0, 0.0, 0.0), SUN, 4.0)
    sun.rotation_euler = (-toward_sun).to_track_quat('-Z', 'Y').to_euler()
    # The plant's lights from below and behind the camera: the lit side we see.
    glow = stage.light(sc, "RefineryGlow", 'SUN', (0.0, 0.0, 0.0), REFINERY_GLOW, 1.2)
    glow.rotation_euler = Vector((0.2, 1.0, 0.45)).to_track_quat('-Z', 'Y').to_euler()


def build(layer, samples):
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    sc.render.fps = FPS
    cam = hall.add_plate_camera(sc)
    cam.data.clip_start, cam.data.clip_end = 1.0, 20000.0
    _lights(sc)
    ship, frames = LAYERS[layer](sc)
    sc.frame_start, sc.frame_end = 1, frames
    return sc, ship, frames


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", nargs="+", default=["all"], choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last[:step], e.g. 1:720:90 for a quick look")
    ap.add_argument("--samples", type=int, default=32)
    args = ap.parse_args(argv)
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        sc, ship, frames = build(layer, args.samples)
        flyover.render_frames(sc, [ship], BUILD / layer, frames,
                              flyover.frame_range(args.frames) if args.frames else None)
        print(f"[render_sky] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

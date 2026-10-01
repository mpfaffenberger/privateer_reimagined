"""Render the ad on the Pleasure concourse's ship-rental billboard (#599).

Run inside Blender (headless), after billboard.py has fitted the screen:
    blender --background --factory-startup \\
        --python tools/room_anim/pleasure/render_billboard.py
    ... -- --frames 1:384:24 --samples 8                    # quick look

The ad is a flat video: it's rendered on billboard.py's ad canvas (two
panels side by side, read from build/room_anim/pleasure/billboard/screen.json)
and billboard.py --ad warps it onto the plate. The camera is level at the
origin looking down +Y with f = FOCAL_PX canvas px, centred on the mullion.
Shots are planned in canvas px at a depth (_canvas_point).

One loop, LOOP_SECONDS long, two spots and a beat of empty starfield:
    a Demon banks across the wall from the plate's right edge, over the
    mullion and out the left, then a Galaxy cruises back the other way,
    turning slowly to show off its hull.
The ship that isn't on is parked behind the camera, where nothing renders.

Writes build/room_anim/pleasure/billboard/billboard_ad/:
    NNNN.png   straight-alpha RGBA, canvas-sized, rendered inside a border
    pass.json  {"frames", "fps"}
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Euler, Matrix, Vector

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import flyover  # noqa: E402
import ships  # noqa: E402
import stage  # noqa: E402
from base import paths  # noqa: E402

PLEASURE = paths("pleasure")
BUILD = PLEASURE.build / "billboard"
LAYER = "billboard_ad"
FPS = flyover.FPS
FOCAL_PX = 700.0                    # canvas px; the canvas is 480 px tall
LOOP_SECONDS = 16.0
PARKED = (0.0, -500.0, 0.0)         # behind the camera: off the air
TILT = 25.0                         # deg: ships tipped toward the lens

# Studio light for an ad: a warm key from the upper left in front, a cold
# blue rim from behind on the right, a dim blue world (the starfield).
KEY, RIM, WORLD = (1.0, 0.95, 0.88), (0.45, 0.65, 1.0), (0.08, 0.11, 0.22)


def _canvas():
    return json.loads((BUILD / "screen.json").read_text())["canvas"]


def _canvas_point(px, py, depth):
    """World point that lands on canvas px (px, py) at `depth` m."""
    w, h = _canvas()
    return Vector(((px - w / 2) * depth / FOCAL_PX, depth, -(py - h / 2) * depth / FOCAL_PX))


def _camera(sc):
    w, h = _canvas()
    sc.render.resolution_x, sc.render.resolution_y = w, h
    data = bpy.data.cameras.new("AdCam")
    data.sensor_fit, data.sensor_width = 'HORIZONTAL', 36.0
    data.lens = FOCAL_PX * 36.0 / w
    data.clip_start, data.clip_end = 1.0, 5000.0
    cam = bpy.data.objects.new("AdCam", data)
    sc.collection.objects.link(cam)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)      # level, looking +Y
    sc.camera = cam


def _lights(sc):
    world = bpy.data.worlds.new("Studio")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (*WORLD, 1.0)
    sc.world = world
    for name, rgb, energy, travel in (("Key", KEY, 6.0, (0.6, 1.0, -0.7)),
                                      ("Rim", RIM, 6.0, (-0.5, -1.0, -0.4))):
        sun = stage.light(sc, name, 'SUN', (0.0, 0.0, 0.0), rgb, energy)
        sun.rotation_euler = Vector(travel).to_track_quat('-Z', 'Y').to_euler()


def _spot(ship, first, last, start, end, yaw, roll):
    """Fly `ship` from `start` to `end` (eased) over frames first..last;
    `yaw(t)` turns its nose toward the camera off the line of travel,
    `roll(t)` banks it (degrees, t 0..1), and the whole ship is tipped TILT
    toward the lens so the ad shows its back, not a level profile. Parked
    the rest of the loop.

    Getting on and off the air must not be motion-blurred into a streak
    from behind the camera: the shutter opens on the frame (START) and the
    keys out of the spot and in the car park hold (CONSTANT), so no
    sub-frame ever falls between parked and flying."""
    edit = bpy.context.preferences.edit
    heading = math.atan2(-(end.x - start.x), end.y - start.y)
    tilt = Matrix.Rotation(math.radians(TILT), 3, 'X')
    for f in range(1, round(LOOP_SECONDS * FPS) + 1):
        edit.keyframe_new_interpolation_type = 'LINEAR' if first <= f < last else 'CONSTANT'
        if first <= f <= last:
            t = (f - first) / (last - first)
            s = t + 0.08 * math.sin(2.0 * math.pi * t)        # lingers mid-wall
            ship.location = start.lerp(end, s)
            pose = Euler((0.0, math.radians(roll(t)), heading + math.radians(yaw(t))), 'XYZ')
            ship.rotation_euler = (tilt @ pose.to_matrix()).to_euler('XYZ', ship.rotation_euler)
        else:
            ship.location, ship.rotation_euler = PARKED, (0.0, 0.0, 0.0)
        ship.keyframe_insert("location", frame=f)
        ship.keyframe_insert("rotation_euler", frame=f)


def _ships():
    frames = round(LOOP_SECONDS * FPS)
    demon = ships.import_ship("demon", 26.0, "Demon")
    flyover.hull_nav_lights(demon, frames, every_s=1.2)
    _spot(demon, 1, round(5.5 * FPS),
          _canvas_point(1650.0, 190.0, 48.0), _canvas_point(-380.0, 290.0, 40.0),
          yaw=lambda t: -25.0, roll=lambda t: 30.0 * math.sin(math.pi * t) - 10.0)
    galaxy = ships.import_ship("mrchship", 100.0, "Galaxy")
    flyover.hull_nav_lights(galaxy, frames, every_s=1.5, phase_s=0.7)
    _spot(galaxy, round(7.0 * FPS), round(15.0 * FPS),
          _canvas_point(-420.0, 260.0, 220.0), _canvas_point(1700.0, 210.0, 200.0),
          yaw=lambda t: 35.0 - 70.0 * t, roll=lambda t: 8.0 * math.sin(math.pi * t))
    return [demon, galaxy], frames


def build(samples):
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    sc.render.fps = FPS
    sc.render.motion_blur_position = 'START'    # see _spot()
    _camera(sc)
    _lights(sc)
    roots, frames = _ships()
    sc.frame_start, sc.frame_end = 1, frames
    return sc, roots, frames


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--frames", help="first:last[:step], e.g. 1:384:24 for a quick look")
    ap.add_argument("--samples", type=int, default=32)
    args = ap.parse_args(argv)
    sc, roots, frames = build(args.samples)
    flyover.render_frames(sc, roots, BUILD / LAYER, frames,
                          flyover.frame_range(args.frames) if args.frames else None)
    print(f"[render_billboard] {LAYER} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

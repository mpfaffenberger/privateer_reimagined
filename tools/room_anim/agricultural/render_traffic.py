"""Render the aircraft seen through the Agricultural concourse windows (#582).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/agricultural/render_traffic.py -- --layer all
    ... -- --layer aircar_crossing --frames 1:160:20 --samples 8    # quick look

The windows look out over farmland at dusk: a glass farm dome, a lit
settlement, far ridges. Nothing outside is near enough to match in 3D, so the
camera is only matched to the horizon: level at the origin looking down +Y,
f = FOCAL_PX, the horizon (the far ridges) at plate y HORIZON_Y. A point
(x, depth, z) lands at (768 + f x / depth, HORIZON_Y - f z / depth), and each
pass is solved from screen targets (_solve), like mining/render_landing.py.
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
import json
import math
import sys
from pathlib import Path

import bpy

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import render  # noqa: E402
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


def _camera(sc):
    data = bpy.data.cameras.new("WindowCam")
    data.sensor_fit, data.sensor_width = 'HORIZONTAL', 36.0
    data.lens = FOCAL_PX * 36.0 / stage.PLATE_W
    data.shift_y = (HORIZON_Y - stage.PLATE_H / 2) / stage.PLATE_W    # frame up: horizon high
    data.clip_start, data.clip_end = 1.0, 20000.0
    cam = bpy.data.objects.new("WindowCam", data)
    sc.collection.objects.link(cam)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)      # level, looking +Y
    sc.camera = cam
    return cam


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
    """Screen target (px, py) at `depth` m -> world (x, depth, z)."""
    px, py = screen
    return ((px - stage.PLATE_W / 2) * depth / FOCAL_PX, depth,
            (HORIZON_Y - py) * depth / FOCAL_PX)


def _strobe(obj, frames, every_s, on_s=0.1, phase_s=0.0):
    for f in range(1, frames + 1):
        t = (f - 1) / FPS + phase_s
        obj.hide_render = (t % every_s) >= on_s
        obj.keyframe_insert("hide_render", frame=f)


def _fly(root, start, end, frames, ease_in=0.0, bank_deg=0.0):
    """Straight flight from `start` to `end` (world points), nose along the
    track, pitched to the climb. `ease_in` 0..1 makes it accelerate (a
    departure). Returns the depth per frame."""
    dx, dy, dz = (e - s for s, e in zip(start, end))
    heading = math.atan2(-dx, dy)
    pitch = math.atan2(dz, math.hypot(dx, dy))
    root.rotation_euler = (pitch, math.radians(bank_deg), heading)
    depths = []
    for f in range(1, frames + 1):
        t = (f - 1) / (frames - 1)
        s = (1.0 - ease_in) * t + ease_in * t * t
        root.location = tuple(a + (b - a) * s for a, b in zip(start, end))
        root.keyframe_insert("location", frame=f)
        depths.append(round(root.location.y, 1))
    return depths


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
    _strobe(stage.emitter("Strobe", ship, (0.0, 0.0, h / 2), 1.6, STROBE, 80.0), frames, 1.5)
    depths = _fly(ship, _solve((1510.0, 170.0), 900.0), _solve((560.0, 95.0), 1900.0), frames,
                  ease_in=0.45, bank_deg=-8.0)
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
    _strobe(stage.emitter("Beacon", car, (0.0, 0.0, h / 2), 0.3, (1.0, 0.55, 0.1), 60.0),
            frames, 0.8, on_s=0.15)
    depths = _fly(car, _solve((560.0, 196.0), 160.0), _solve((1530.0, 204.0), 160.0), frames,
                  bank_deg=6.0)
    return car, frames, depths


# name -> builder(scene) -> (craft root, frames, depth per frame)
LAYERS = {"freighter_departure": _freighter_departure, "aircar_crossing": _aircar_crossing}


def build(layer, samples):
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    _camera(sc)
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
        out = BUILD / layer
        out.mkdir(parents=True, exist_ok=True)
        if args.frames:
            first, last, *step = (int(v) for v in args.frames.split(":"))
            todo = range(first, min(last, frames) + 1, step[0] if step else 1)
        else:
            for old in out.glob("*.png"):       # off-screen frames write nothing,
                old.unlink()                    # so stale ones must not survive
            todo = range(1, frames + 1)
        for f in todo:
            sc.frame_set(f)
            if not render.set_border(sc, [root], margin=0.15, footprint=False):
                continue
            sc.render.filepath = str(out / f"{f:04d}.png")
            bpy.ops.render.render(write_still=True)
        (out / "pass.json").write_text(json.dumps(
            {"frames": frames, "fps": FPS, "depth": depths}) + "\n")
        print(f"[render_traffic] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

"""Render the Galaxy freighter passing over the mining landing pad (#561).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/mining/render_landing.py -- --frames 1:384:48 --samples 8
    ... --                                  # the full pass

The landing pad is 18 differently framed composites (bake_landing.py), so
nothing is matched to one painting. The scene is built for a CANONICAL
framing instead: tarsus, whose rim height is read from bake_landing.py's
anchors.json (run that first). The engine slides every frame from that
anchor onto each composite's own rim height.

The camera is level at the origin looking down +Y. f is exactly 1024 px
(24 mm on 36 mm over the 1536 px canvas), so a point (x, depth, z) lands at
(768 + f x / depth, 512 - f z / depth). The sky is a thin strip (~103 px
on tarsus, ~61 px on the tightest composites), so the path is solved from
screen targets relative to the rim (_path): the ship's centre rises from
rim + 10 (emerging from behind the rim, off the right edge) to rim - 30 (off
the left edge). At a constant ALTITUDE that means closing on the camera,
so it also grows ~10% as it climbs: the original game's beat (the legacy
landing_shp overlay). The climb stays shallow so the ~50 px ship still
clears the top edge on the tightest skies.

Writes build/room_anim/mining/landing/galaxy_flyover/:
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
for path in (HERE, HERE.parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import render  # noqa: E402
import ships  # noqa: E402
import stage  # noqa: E402
from base import paths  # noqa: E402

MINING = paths("mining")
BUILD = MINING.build / "landing"
LAYER = "galaxy_flyover"
FPS = 24
FOCAL_PX = 1024.0
CANONICAL = "tarsus"                # composite the pass is rendered against

LENGTH = 100.0                      # m; ~110 px long, ~50 px tall on screen
ALTITUDE = 400.0                    # m above the camera
RISE = (10.0, -30.0)                # ship centre relative to the rim, px: start, end
SCREEN_X = (1616.0, -80.0)          # off the right edge -> off the left edge
PASS_SECONDS = 16.0

SUN = (1.0, 0.93, 0.82)             # warm, like the crater's lit walls
# World: the sunlit crater below. It must be bright: the hull is dark grey
# metal seen from below, and metal is mostly lit by what it reflects. At
# 0.12 the mid-hull matched the black sky and the ship read as two pieces.
CRATER_BOUNCE = (0.35, 0.26, 0.17)
NAV_RED, NAV_GREEN, STROBE = (1.0, 0.1, 0.05), (0.1, 1.0, 0.3), (1.0, 1.0, 1.0)


def _camera(sc):
    data = bpy.data.cameras.new("SkyCam")
    data.sensor_fit, data.sensor_width = 'HORIZONTAL', 36.0
    data.lens = FOCAL_PX * 36.0 / stage.PLATE_W
    data.clip_start, data.clip_end = 1.0, 20000.0
    cam = bpy.data.objects.new("SkyCam", data)
    sc.collection.objects.link(cam)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)      # level, looking +Y
    sc.camera = cam


def _lights(sc):
    world = bpy.data.worlds.new("Space")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (*CRATER_BOUNCE, 1.0)
    sc.world = world
    stage.light(sc, "Sun", 'SUN', (0.0, 0.0, 0.0), SUN, 5.0,
                rot=(math.radians(35.0), math.radians(-25.0), 0.0))


def _anchor():
    return json.loads((MINING.anim / "landing" / "anchors.json").read_text())[CANONICAL]


def _path(rim_y):
    """-> ((x, depth) start, (x, depth) end) in m, putting the ship centre on
    the RISE / SCREEN_X targets: depth = f H / (512 - y), x = (px - 768) depth / f."""
    ends = []
    for px, rise in zip(SCREEN_X, RISE):
        depth = FOCAL_PX * ALTITUDE / (stage.PLATE_H / 2 - (rim_y + rise))
        ends.append(((px - stage.PLATE_W / 2) * depth / FOCAL_PX, depth))
    return ends


def _freighter(frames, rim_y):
    ship = ships.import_ship("mrchship", LENGTH, "Galaxy")
    w, _, h = ship["size"]
    lights = [stage.emitter("NavPort", ship, (-w / 2, 0.0, 0.0), 1.3, NAV_RED, 30.0),
              stage.emitter("NavStarboard", ship, (w / 2, 0.0, 0.0), 1.3, NAV_GREEN, 30.0)]
    strobe = stage.emitter("Strobe", ship, (0.0, 0.0, -h / 2), 1.5, STROBE, 60.0)
    # A 0.1 s flash every 1.5 s: key the strobe's visibility.
    for f in range(1, frames + 1):
        on = (f % round(1.5 * FPS)) < max(1, round(0.1 * FPS))
        strobe.hide_render = not on
        strobe.keyframe_insert("hide_render", frame=f)
    (x0, y0), (x1, y1) = _path(rim_y)
    heading = math.degrees(math.atan2(-(x1 - x0), y1 - y0))     # nose along the travel
    ship.rotation_euler = (0.0, 0.0, math.radians(heading))
    for f in range(1, frames + 1):
        t = (f - 1) / (frames - 1)
        ship.location = (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, ALTITUDE)
        ship.keyframe_insert("location", frame=f)
    return ship, lights


def build(samples):
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    frames = round(PASS_SECONDS * FPS)
    sc.frame_start, sc.frame_end = 1, frames
    _camera(sc)
    _lights(sc)
    ship, _ = _freighter(frames, _anchor()[1])
    return sc, ship, frames


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", help="first:last[:step], e.g. 1:384:48 for a quick look")
    ap.add_argument("--samples", type=int, default=32)
    args = ap.parse_args(argv)
    sc, ship, frames = build(args.samples)
    out = BUILD / LAYER
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
        if not render.set_border(sc, [ship], margin=0.15, footprint=False):
            continue
        sc.render.filepath = str(out / f"{f:04d}.png")
        bpy.ops.render.render(write_still=True)
    (out / "pass.json").write_text(json.dumps(
        {"frames": frames, "fps": FPS, "anchor": _anchor()}) + "\n")
    print(f"[render_landing] {LAYER} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

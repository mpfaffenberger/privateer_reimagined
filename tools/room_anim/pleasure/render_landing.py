"""Render a transport coming in over the sea at the Pleasure landing pad (#595).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/pleasure/render_landing.py -- --frames 1:432:48 --samples 8
    ... --                                  # the full pass

The landing pad is 18 differently framed composites (bake_landing.py), so
nothing is matched to one painting. As on the mining pad, the scene is built
for a CANONICAL framing (tarsus, its horizon read from anchors.json: run
bake_landing.py first) and the engine slides every frame from that anchor
onto each composite's own horizon.

The camera is level at the origin looking down +Y with f = 1024 px, so a
point (x, depth, z) lands at (768 + f x / depth, horizon - f z / depth). The
transport emerges from behind the tower block, then cruises left and away at
a constant ALTITUDE toward a spaceport beyond the left tower. Receding at a
constant height, it sinks toward the horizon and shrinks (~95 px to ~45 px). The path is solved from screen targets
relative to the horizon (_path). The lowest target stays 110 px above the
anchor: bake_landing.py's horizon can sit up to ~70 px below the painted one,
never above it, so the ship always clears the sea.

It's drawn under the plate through each composite's sky mask, so the block,
the towers and anything else painted occlude it.

Writes build/room_anim/pleasure/landing/transport_arrival/:
    NNNN.png   straight-alpha RGBA, rendered inside a border around the ship
    pass.json  {"frames", "fps", "anchor"}
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import render  # noqa: E402
import ships  # noqa: E402
import stage  # noqa: E402
from base import paths  # noqa: E402

PLEASURE = paths("pleasure")
BUILD = PLEASURE.build / "landing"
LAYER = "transport_arrival"
FPS = 24
FOCAL_PX = 1024.0
CANONICAL = "tarsus"                # composite the pass is rendered against

LENGTH = 300.0                      # m, a liner-sized transport
ALTITUDE = 500.0                    # m above the camera
ABOVE_HORIZON = (230.0, 110.0)      # ship centre above the horizon, px: start, end
SCREEN_X = (900.0, 250.0)           # behind the block -> behind the left tower
PASS_SECONDS = 18.0

# Dusk: the sun is down behind the far sea, a little left, so the ship is lit
# from beyond it (a warm rim) and filled by the purple sky.
SUNSET = (1.0, 0.55, 0.45)
SKY = (0.22, 0.14, 0.42)
NAV_RED, NAV_GREEN, STROBE = (1.0, 0.1, 0.05), (0.1, 1.0, 0.3), (1.0, 1.0, 1.0)
NAV_INSET = 0.8                     # of the half-width


def _camera(sc):
    data = bpy.data.cameras.new("SkyCam")
    data.sensor_fit, data.sensor_width = 'HORIZONTAL', 36.0
    data.lens = FOCAL_PX * 36.0 / stage.PLATE_W
    data.clip_start, data.clip_end = 1.0, 50000.0
    cam = bpy.data.objects.new("SkyCam", data)
    sc.collection.objects.link(cam)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)      # level, looking +Y
    sc.camera = cam
    return cam


def _horizon_shift(cam, horizon_y):
    """Lens shift that puts the level camera's horizon on `horizon_y`."""
    cam.data.shift_y = (horizon_y - stage.PLATE_H / 2) / stage.PLATE_W


def _lights(sc):
    world = bpy.data.worlds.new("Dusk")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (*SKY, 1.0)
    sc.world = world
    # A sun just under the horizon, ahead and a little left: it rims the hull.
    stage.light(sc, "Sunset", 'SUN', (0.0, 0.0, 0.0), SUNSET, 3.0,
                rot=(math.radians(-85.0), 0.0, math.radians(20.0)))


def _anchor():
    return json.loads((PLEASURE.anim / "landing" / "anchors.json").read_text())[CANONICAL]


def _path():
    """-> ((x, depth) start, (x, depth) end) in m, putting the ship centre on
    the SCREEN_X / ABOVE_HORIZON targets: depth = f H / above, x = (px - 768) depth / f."""
    ends = []
    for px, above in zip(SCREEN_X, ABOVE_HORIZON):
        depth = FOCAL_PX * ALTITUDE / above
        ends.append(((px - stage.PLATE_W / 2) * depth / FOCAL_PX, depth))
    return ends


def _transport(frames):
    ship = ships.import_ship("transprt", LENGTH, "Transport")
    w, length, h = ship["size"]
    # Nav lights on the hull's flanks: its widest point isn't amidships, so at
    # a full half-width they float off the side.
    for name, side, rgb in (("NavPort", -1, NAV_RED), ("NavStarboard", 1, NAV_GREEN)):
        stage.emitter(name, ship, (side * NAV_INSET * w / 2, 0.0, 0.0), 0.02 * length, rgb, 12.0)
    strobe = stage.emitter("Strobe", ship, (0.0, 0.0, h / 2), 0.02 * length, STROBE, 60.0)
    for f in range(1, frames + 1):                          # a 0.1 s flash every 1.5 s
        strobe.hide_render = (f % round(1.5 * FPS)) >= max(1, round(0.1 * FPS))
        strobe.keyframe_insert("hide_render", frame=f)
    (x0, y0), (x1, y1) = _path()
    heading = math.degrees(math.atan2(-(x1 - x0), y1 - y0))     # nose along the travel
    ship.rotation_euler = (0.0, 0.0, math.radians(heading))
    for f in range(1, frames + 1):
        t = (f - 1) / (frames - 1)
        ship.location = Vector((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, ALTITUDE))
        ship.keyframe_insert("location", frame=f)
    return ship


def build(samples):
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    frames = round(PASS_SECONDS * FPS)
    sc.frame_start, sc.frame_end = 1, frames
    _horizon_shift(_camera(sc), _anchor()[1])
    _lights(sc)
    return sc, _transport(frames), frames


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", help="first:last[:step], e.g. 1:432:48 for a quick look")
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

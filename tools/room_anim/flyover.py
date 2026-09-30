"""Ships crossing a crater landing pad's strip of sky (#561; shared in #587 by
the mining and pirate bases; Oxford's dusk pad, #593, brings its own
`lights=`). Runs inside Blender.

The landing pad is 18 differently framed composites (bake_crater.py), so
nothing is matched to one painting. The scene is built for a CANONICAL
framing instead, whose rim height is read from bake_crater.py's
anchors.json (run that first). The engine slides every frame from that
anchor onto each composite's own rim height.

The camera is level at the origin looking down +Y. f is exactly 1024 px
(24 mm on 36 mm over the 1536 px canvas), so a point (x, depth, z) lands at
(768 + f x / depth, 512 - f z / depth). The sky is a thin strip (~61 px on
the tightest composites), so paths are solved from screen targets relative
to the rim (screen_path()).

A base's render_landing.py builds its ships and calls run(), which writes
build/room_anim/<base>/landing/<layer>/ (render_frames()):
    NNNN.png   straight-alpha RGBA, rendered inside a border around the ships
    pass.json  {"frames", "fps", "anchor"}
bake_crater.py --layers-only then packs them. render_frames() and
nav_lights() also serve other straight-alpha sky passes (the Refinery
concourse's dome, #584).
"""
import argparse
import json
import math

import bpy

import render
import stage
from base import paths

FPS = 24
FOCAL_PX = 1024.0
CANONICAL = "tarsus"                # composite the passes are rendered against

SUN = (1.0, 0.93, 0.82)             # warm, like the crater's lit walls
# World: the sunlit crater below. It must be bright: hulls are dark metal
# seen from below, and metal is mostly lit by what it reflects. At 0.12 the
# Galaxy's mid-hull matched the black sky and it read as two pieces.
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


def anchor(base):
    """The canonical composite's [cx, cy, r]; cy is its rim height."""
    return json.loads((paths(base).anim / "landing" / "anchors.json").read_text())[CANONICAL]


def screen_path(rim_y, altitude, rise, screen_x):
    """-> [(x, depth) start, (x, depth) end] in m, putting a point at
    `altitude` above the camera on screen x `screen_x` and y rim + `rise`
    (px; pairs of start, end): depth = f H / (512 - y), x = (px - 768) depth / f."""
    ends = []
    for px, dy in zip(screen_x, rise):
        depth = FOCAL_PX * altitude / (stage.PLATE_H / 2 - (rim_y + dy))
        ends.append(((px - stage.PLATE_W / 2) * depth / FOCAL_PX, depth))
    return ends


def fly_straight(ship, start, end, altitude, frames):
    """Constant speed from `start` to `end` (x, depth) at `altitude`, nose
    along the travel."""
    (x0, y0), (x1, y1) = start, end
    ship.rotation_euler = (0.0, 0.0, math.atan2(-(x1 - x0), y1 - y0))
    for f in range(1, frames + 1):
        t = (f - 1) / (frames - 1)
        ship.location = (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, altitude)
        ship.keyframe_insert("location", frame=f)


def nav_lights(ship, frames, radius, strength, strobe_radius, strobe_strength,
               strobe_every_s=1.5, strobe_phase=0):
    """Red/green wingtip lights and a belly strobe: a 0.1 s flash every
    `strobe_every_s`, keyed through its visibility."""
    w, _, h = ship["size"]
    stage.emitter("NavPort", ship, (-w / 2, 0.0, 0.0), radius, NAV_RED, strength)
    stage.emitter("NavStarboard", ship, (w / 2, 0.0, 0.0), radius, NAV_GREEN, strength)
    strobe = stage.emitter("Strobe", ship, (0.0, 0.0, -h / 2), strobe_radius, STROBE,
                           strobe_strength)
    for f in range(1, frames + 1):
        on = (f + strobe_phase) % round(strobe_every_s * FPS) < max(1, round(0.1 * FPS))
        strobe.hide_render = not on
        strobe.keyframe_insert("hide_render", frame=f)


def hull_nav_lights(ship, frames, every_s=1.5, phase_s=0.0):
    """nav_lights() sized for the hull: a 100 m ship gets the crater
    freighter's 1.3 m lamps (#561), a strobe every `every_s` from `phase_s`."""
    k = ship["size"][1] / 100.0
    nav_lights(ship, frames, 1.3 * k, 30.0, 1.5 * k, 60.0,
               strobe_every_s=every_s, strobe_phase=round(phase_s * FPS))


def frame_range(spec):
    """"first:last[:step]" -> the frame numbers, for a quick look."""
    first, last, *step = (int(v) for v in spec.split(":"))
    return range(first, last + 1, step[0] if step else 1)


def render_frames(sc, roots, out, frames, todo=None, info=None):
    """Straight-alpha NNNN.png frames into `out`, each rendered inside a
    border around `roots` (off-screen frames write nothing), and pass.json
    {"frames", "fps", **info}. `todo` renders just those frames; a full pass
    first clears stale ones."""
    out.mkdir(parents=True, exist_ok=True)
    if todo is None:
        for old in out.glob("*.png"):           # off-screen frames write nothing,
            old.unlink()                        # so stale ones must not survive
        todo = range(1, frames + 1)
    for f in todo:
        sc.frame_set(f)
        if not render.set_border(sc, roots, margin=0.15, footprint=False):
            continue
        sc.render.filepath = str(out / f"{f:04d}.png")
        bpy.ops.render.render(write_still=True)
    (out / "pass.json").write_text(json.dumps(
        {"frames": frames, "fps": FPS, **(info or {})}) + "\n")


def build(samples, frames, lights=None):
    """The empty sky stage: camera, then `lights(sc)`: by default the sun
    and crater bounce."""
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    sc.frame_start, sc.frame_end = 1, frames
    _camera(sc)
    (lights or _lights)(sc)
    return sc


def run(argv, base, layer, seconds, build_ships, doc=None, lights=None):
    """Parse --frames/--samples, build the stage (`lights`: see build()), let
    `build_ships(frames, rim_y)` add the ships (-> their root objects) and
    render the pass. rim_y is the canonical anchor's cy."""
    ap = argparse.ArgumentParser(description=doc)
    ap.add_argument("--frames", help="first:last[:step], e.g. 1:384:48 for a quick look")
    ap.add_argument("--samples", type=int, default=32)
    args = ap.parse_args(argv)
    frames = round(seconds * FPS)
    sc = build(args.samples, frames, lights)
    rim = anchor(base)
    roots = build_ships(frames, rim[1])
    render_frames(sc, roots, paths(base).build / "landing" / layer, frames,
                  frame_range(args.frames) if args.frames else None, {"anchor": rim})
    print(f"[render_landing] {layer} done", flush=True)

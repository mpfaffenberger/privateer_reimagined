"""Render the aircars flying past the New Detroit landing pad (#591).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/newdetroit/render_landing.py -- --frames 1:200:25 --samples 8
    ... -- --layer all                        # every pass

The landing pad is 18 composites (landing_ships/<hull>.png), and they are not
one painting reframed: the pad was rendered from a different camera for each
hull and the city around it painted afresh. So nothing here goes *behind*
anything painted. The aircars (the original game's New Detroit aircar,
ships_wcnews/nd_airca.obj) fly between the camera and the pad, 20-25 m above
its deck: they cover the plate and are never covered, so one pass fits every
composite and is drawn unanchored, over the plate.

The camera looks down PITCH_DEG like the composites do, f exactly FOCAL_PX.
Paths are solved from screen targets: a point (px, py) at `depth` metres
along the camera's view axis (_screen_point).

Writes build/room_anim/newdetroit/landing/<layer>/:
    NNNN.png   straight-alpha RGBA, rendered inside a border around the car
    pass.json  {"frames", "fps"}
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Euler, Vector

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import render  # noqa: E402
import ships  # noqa: E402
import stage  # noqa: E402
from base import paths  # noqa: E402

BUILD = paths("newdetroit").build / "landing"
FPS = 24
FOCAL_PX = 1100.0
PITCH_DEG = 38.0                  # camera looking down, like the composites

CAR_LENGTH = 5.0                  # m
MOON = (0.62, 0.72, 1.0)          # cool light from above
CITY_GLOW = (1.0, 0.62, 0.35)     # warm light from the streets below
HEADLAMP, TAIL, STROBE = (1.0, 0.92, 0.75), (1.0, 0.08, 0.04), (1.0, 0.15, 0.1)

# name -> screen targets ((px, py, depth m) start, end) and speed (m/s). Both
# start and end off-frame. Depths keep the cars 20-25 m above the pad's
# deck (~70 m out): in front of everything painted.
PASSES = {
    # Low and close, right to left over the drop in front of the pad's near
    # rim (nearer and higher than the rim): ~150 px long.
    "aircar_low": (((1720.0, 940.0, 34.0), (-180.0, 870.0, 40.0)), 15.0),
    # High, left to right over the pad's far side and the hangar: ~100 px.
    "aircar_high": (((-150.0, 250.0, 60.0), (1700.0, 160.0, 52.0)), 12.0),
}


def _view():
    return Euler((math.radians(90.0 - PITCH_DEG), 0.0, 0.0)).to_matrix()


def _camera(sc):
    data = bpy.data.cameras.new("PadCam")
    data.sensor_fit, data.sensor_width = 'HORIZONTAL', 36.0
    data.lens = FOCAL_PX * 36.0 / stage.PLATE_W
    data.clip_start, data.clip_end = 0.5, 5000.0
    cam = bpy.data.objects.new("PadCam", data)
    sc.collection.objects.link(cam)
    cam.rotation_euler = _view().to_euler()
    sc.camera = cam


def _screen_point(px, py, depth):
    """World point seen at plate pixel (px, py), `depth` m along the view."""
    local = Vector(((px - stage.PLATE_W / 2) * depth / FOCAL_PX,
                    (stage.PLATE_H / 2 - py) * depth / FOCAL_PX, -depth))
    return _view() @ local


def _lights(sc):
    world = bpy.data.worlds.new("NightCity")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.03, 0.035, 0.05, 1)
    sc.world = world
    stage.light(sc, "Moon", 'SUN', (0.0, 0.0, 0.0), MOON, 1.5,
                rot=(math.radians(20.0), math.radians(-30.0), 0.0))
    stage.light(sc, "CityGlow", 'SUN', (0.0, 0.0, 0.0), CITY_GLOW, 0.8,
                rot=(math.radians(160.0), 0.0, 0.0))                  # shining up


def _glow(car):
    """The mesh's GLOW_* materials are its drive glow: make them emit."""
    for obj in car.children:
        for slot in obj.material_slots:
            mat = slot.material
            if mat is None or "GLOW" not in mat.name:
                continue
            bsdf = mat.node_tree.nodes["Principled BSDF"]
            src = bsdf.inputs["Base Color"].links[0].from_socket
            mat.node_tree.links.new(src, bsdf.inputs["Emission Color"])
            bsdf.inputs["Emission Strength"].default_value = 1.5


def _aircar(name, frames, targets):
    car = ships.import_ship("nd_airca", CAR_LENGTH, name)
    _glow(car)
    w, length, h = car["size"]
    for side in (-1, 1):
        stage.emitter(f"{name}Head{side}", car, (side * w * 0.22, length * 0.5, 0.0), 0.07,
                      HEADLAMP, 25.0)
        stage.emitter(f"{name}Tail{side}", car, (side * w * 0.3, -length * 0.5, 0.05), 0.06,
                      TAIL, 12.0)
    strobe = stage.emitter(f"{name}Strobe", car, (0.0, -length * 0.1, -h / 2), 0.08, STROBE,
                           40.0)
    for f in range(1, frames + 1):            # a 0.1 s flash every second
        strobe.hide_render = (f % FPS) >= max(1, round(0.1 * FPS))
        strobe.keyframe_insert("hide_render", frame=f)
    a, b = (_screen_point(*t) for t in targets)
    travel = b - a
    car.rotation_euler = (math.atan2(travel.z, travel.xy.length), 0.0,
                          math.atan2(-travel.x, travel.y))           # nose along the travel
    for f in range(1, frames + 1):
        car.location = a.lerp(b, (f - 1) / (frames - 1))
        car.keyframe_insert("location", frame=f)
    return car


def build(layer, samples):
    sc = stage.reset()
    stage.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0             # plain straight alpha: no plate encode
    sc.render.film_transparent = True
    targets, speed = PASSES[layer]
    distance = (_screen_point(*targets[1]) - _screen_point(*targets[0])).length
    frames = round(distance / speed * FPS)
    sc.frame_start, sc.frame_end = 1, frames
    _camera(sc)
    _lights(sc)
    return sc, _aircar(layer, frames, targets), frames


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", nargs="+", default=["all"], choices=["all", *sorted(PASSES)])
    ap.add_argument("--frames", help="first:last[:step], e.g. 1:200:25 for a quick look")
    ap.add_argument("--samples", type=int, default=32)
    args = ap.parse_args(argv)
    for layer in (sorted(PASSES) if "all" in args.layer else args.layer):
        sc, car, frames = build(layer, args.samples)
        out = BUILD / layer
        out.mkdir(parents=True, exist_ok=True)
        if args.frames:
            first, last, *step = (int(v) for v in args.frames.split(":"))
            todo = range(first, last + 1, step[0] if step else 1)
        else:
            for old in out.glob("*.png"):       # off-screen frames write nothing,
                old.unlink()                    # so stale ones must not survive
            todo = range(1, frames + 1)
        for f in todo:
            sc.frame_set(f)
            if not render.set_border(sc, [car], margin=0.15, footprint=False):
                continue
            sc.render.filepath = str(out / f"{f:04d}.png")
            bpy.ops.render.render(write_still=True)
        (out / "pass.json").write_text(json.dumps({"frames": frames, "fps": FPS}) + "\n")
        print(f"[render_landing] {layer} done ({frames} frames)", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

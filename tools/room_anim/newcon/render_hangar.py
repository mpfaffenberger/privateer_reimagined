"""Render ship traffic for the New Con hangar (#553). Runs inside Blender:

    blender --background --factory-startup \\
        --python tools/room_anim/newcon/render_hangar.py -- --layer all
    ... -- --layer ship_depart --frames 1:48 --samples 8     # quick look

The hangar is 18 differently framed composites, so nothing is camera-matched
to one painting. Instead the scene is built around a CANONICAL mouth: the
camera sits at the origin looking down +Y (24 mm on a 36 mm sensor, so the
focal length is exactly 1024 px on the 1536x1024 canvas), and the hangar
mouth is a disc at depth MOUTH_DEPTH that projects to ANCHOR = (cx, cy, r)
px. The engine maps every frame from ANCHOR onto each composite's detected
mouth (bake_hangar.py -> anchors.json).

Beyond the mouth plane a ship is out in space: it is drawn UNDER the plate,
so the tunnel rim and the parked ship occlude it. Inside the tunnel it is
drawn OVER the plate; those paths stay in the band above the parked hulls
(bake_traffic.py checks this).

Writes build/room_anim/newcon/hangar/<layer>/:
    NNNN.png   straight-alpha RGBA ship, rendered inside a border around it
    pass.json  {"frames", "fps", "anchor", "over": [frame numbers in the tunnel]}
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import render  # noqa: E402  (set_border)
import scene as hall  # noqa: E402  (reset, setup_render, canvas size)
import ships  # noqa: E402
from base import paths  # noqa: E402

BUILD = paths("newcon").build / "hangar"

FOCAL_PX = 1024.0
ANCHOR = (768.0, 360.0, 200.0)          # canonical mouth on the canvas, px
MOUTH_DEPTH = 220.0                     # m from the camera to the mouth plane
FPS = 24


def screen_to_world(px, py, depth):
    """Canvas pixel at `depth` metres in front of the camera -> world point."""
    return Vector(((px - hall.PLATE_W / 2) * depth / FOCAL_PX, depth,
                   (hall.PLATE_H / 2 - py) * depth / FOCAL_PX))


MOUTH_CENTRE = screen_to_world(ANCHOR[0], ANCHOR[1], MOUTH_DEPTH)
MOUTH_RADIUS = ANCHOR[2] * MOUTH_DEPTH / FOCAL_PX


# ---- flight paths: u in [0, 1] over the pass -> world position ------------------

def _thrust(t, gate, end):
    """Distance covered after t seconds under steadily rising thrust,
    s = v0*t + c*t^3, fitted through the (seconds, metres) points `gate`
    and `end`. Reversing time gives a braking approach."""
    (tg, dg), (te, de) = gate, end
    c = (de * tg - dg * te) / (te ** 3 * tg - tg ** 3 * te)
    return (dg - c * tg ** 3) / tg * t + c * t ** 3


DEPART_S, ARRIVE_S = 10.0, 9.0          # pass lengths, seconds


def _depart_path(u):
    """Launch: in from above/behind the camera, out through the upper mouth
    after 4.5 s, then accelerating away along the same line."""
    start = Vector((4.0, 20.0, 19.0))
    gate = MOUTH_CENTRE + Vector((-4.0, 0.0, 0.45 * MOUTH_RADIUS))
    s = _thrust(u * DEPART_S, (4.5, (gate - start).length), (DEPART_S, 1400.0))
    return start + (gate - start).normalized() * s


def _arrive_path(u):
    """Approach: out of the middle of the mouth from 1.4 km, braking, in
    through the mouth after 5.5 s, then up through the tunnel over the
    camera."""
    depth = 25.0 + _thrust((1.0 - u) * ARRIVE_S, (ARRIVE_S - 5.5, MOUTH_DEPTH - 25.0),
                           (ARRIVE_S, 1375.0))
    return Vector((8.0 + 0.02 * (depth - 25.0), depth, 24.0 + 0.16 * (depth - 25.0)))


def _orientation(path, u, du, bank_deg):
    """Ship rotation on the path at u: nose along the velocity, top toward
    +Z, rolled by `bank_deg` about the nose."""
    here = path(u)
    fwd = (path(u + du) - here) if u + du <= 1.0 else (here - path(u - du))
    fwd.normalize()
    right = fwd.cross(Vector((0, 0, 1))).normalized()
    basis = Matrix((right, fwd, right.cross(fwd))).transposed()  # ship X, Y nose, Z top
    return Matrix.Rotation(math.radians(bank_deg), 3, fwd) @ basis


def _key_path(root, path, frames, bank_deg):
    """A key every frame (dense enough that motion blur follows the curve);
    eulers stay continuous so blur never spins through a 2*pi wrap."""
    du, prev = 1.0 / (frames - 1), None
    for f in range(1, frames + 1):
        u = (f - 1) * du
        rot = _orientation(path, u, du, bank_deg * math.sin(math.pi * u))
        root.location = path(u)
        root.rotation_euler = rot.to_euler('XYZ', prev) if prev else rot.to_euler('XYZ')
        prev = root.rotation_euler.copy()
        root.keyframe_insert("location", frame=f)
        root.keyframe_insert("rotation_euler", frame=f)


# ---- ship dressing ---------------------------------------------------------------

def _emitter(name, parent, local, radius, rgb, strength, squash=(1, 1, 1)):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=radius, location=(0, 0, 0))
    glow = bpy.context.active_object
    glow.name = name
    mat = bpy.data.materials.new(name)
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0, 0, 0, 1)
    bsdf.inputs["Emission Color"].default_value = (*rgb, 1)
    bsdf.inputs["Emission Strength"].default_value = strength
    glow.data.materials.append(mat)
    glow.parent = parent
    glow.location = local
    glow.scale = squash
    return glow


def _demon(length):
    root = ships.import_ship("demon", length, "ShipDepart")
    for side in (-1, 1):                          # twin nozzles, from the front lookdev
        _emitter(f"Nozzle{side}", root, (side * 0.08 * length, -0.49 * length, 0.0),
                 0.035 * length, (0.3, 0.5, 1.0), 6.0, squash=(1, 0.3, 1))
    exhaust = bpy.data.objects.new("Exhaust", bpy.data.lights.new("Exhaust", 'POINT'))
    exhaust.data.color, exhaust.data.energy = (0.45, 0.65, 1.0), 900.0
    exhaust.data.shadow_soft_size = 1.0
    bpy.context.scene.collection.objects.link(exhaust)
    exhaust.parent, exhaust.location = root, (0, -0.62 * length, 0)
    return root


def _talon(length):
    root = ships.import_ship("talon5", length, "ShipArrive")
    _emitter("Landing", root, (0, 0.47 * length, -0.05 * length), 0.03 * length,
             (1.0, 0.8, 0.5), 30.0)               # warm landing light on the nose
    for side, rgb in ((-1, (1.0, 0.1, 0.05)), (1, (0.1, 1.0, 0.25))):
        _emitter(f"Nav{side}", root, (side * 0.3 * length, -0.2 * length, 0.02 * length),
                 0.02 * length, rgb, 30.0)        # port red / starboard green
    return root


LAYERS = {
    # name: (builder, path, frames, bank degrees at mid-flight)
    "ship_depart": (lambda: _demon(18.0), _depart_path, int(DEPART_S * FPS), 12.0),
    "ship_arrive": (lambda: _talon(16.0), _arrive_path, int(ARRIVE_S * FPS), -10.0),
}


# ---- scene -------------------------------------------------------------------------

def _light(name, kind, loc, rgb, energy, size=1.0, aim=None):
    obj = bpy.data.objects.new(name, bpy.data.lights.new(name, kind))
    obj.data.color, obj.data.energy = rgb, energy
    if kind == 'AREA':
        obj.data.size = size
    elif kind in ('POINT', 'SPOT'):
        obj.data.shadow_soft_size = size
    obj.location = loc
    if aim is not None:
        obj.rotation_euler = (Vector(aim) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()
    bpy.context.scene.collection.objects.link(obj)
    return obj


def build_hangar(samples):
    """Camera + lighting matching the painting: dim cool starlight out in
    space; inside the tunnel cool steel floods and warm amber spill from the
    hangar lamps near the camera."""
    sc = hall.reset()
    hall.setup_render(sc, samples=samples)
    sc.view_settings.exposure = 0.0               # plain sprites: no plate-aware headroom
    sc.render.film_transparent = True
    sc.render.fps = FPS
    cam = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
    cam.data.sensor_fit, cam.data.sensor_width, cam.data.lens = 'HORIZONTAL', 36.0, 24.0
    cam.data.clip_start, cam.data.clip_end = 1.0, 10000.0
    cam.rotation_euler = (math.radians(90), 0, 0)  # look down +Y, +Z up
    sc.collection.objects.link(cam)
    sc.camera = cam

    world = bpy.data.worlds.new("Space")
    world.color = (0.004, 0.005, 0.009)
    sc.world = world
    star = _light("Starlight", 'SUN', (0, 0, 0), (0.75, 0.82, 1.0), 1.6)
    star.rotation_euler = (math.radians(55), math.radians(-25), math.radians(200))
    # Tunnel floods: a ring of cool panels just inside the mouth, lighting the
    # tunnel's axis; they fall off with distance, so ships light up as they
    # enter. Amber: the hangar's warm lamps near the camera.
    for i, angle in enumerate((30, 150, 270)):
        a = math.radians(angle)
        loc = MOUTH_CENTRE + Vector((math.cos(a) * MOUTH_RADIUS * 0.9, -40.0,
                                     math.sin(a) * MOUTH_RADIUS * 0.9))
        _light(f"Flood{i}", 'AREA', loc, (0.78, 0.86, 1.0), 25000.0, size=12.0,
               aim=MOUTH_CENTRE + Vector((0, -120.0, 0)))
    _light("Amber", 'AREA', (0.0, 30.0, 40.0), (1.0, 0.6, 0.28), 12000.0, size=15.0,
           aim=(0.0, 80.0, 30.0))
    return sc


def build(layer, samples):
    builder, path, frames, bank = LAYERS[layer]
    sc = build_hangar(samples)
    root = builder()
    _key_path(root, path, frames, bank)
    sc.frame_start, sc.frame_end = 1, frames
    return sc, root, frames


def render(layer, frame_range=None, samples=32):
    sc, root, frames = build(layer, samples)
    out = BUILD / layer
    out.mkdir(parents=True, exist_ok=True)
    if frame_range is None:
        # Off-screen frames write nothing, so a stale PNG from an earlier
        # render would survive and get baked. A full render starts clean.
        for old in out.glob("*.png"):
            old.unlink()
    over = []
    first, last = frame_range or (1, frames)
    for f in range(first, last + 1):
        sc.frame_set(f)
        if root.matrix_world.translation.y < MOUTH_DEPTH:
            over.append(f)
        if not render.set_border(sc, [root], margin=0.25, footprint=False):
            continue                              # off-screen: a blank timeline slot
        sc.render.filepath = str(out / f"{f:04d}.png")
        bpy.ops.render.render(write_still=True)
    (out / "pass.json").write_text(json.dumps(
        {"frames": frames, "fps": FPS, "anchor": list(ANCHOR), "over": over}) + "\n")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", nargs="+", required=True, choices=["all", *sorted(LAYERS)])
    ap.add_argument("--frames", help="first:last, e.g. 1:48")
    ap.add_argument("--samples", type=int, default=32)
    args = ap.parse_args(argv)
    frames = tuple(int(v) for v in args.frames.split(":")) if args.frames else None
    for layer in (sorted(LAYERS) if "all" in args.layer else args.layer):
        render(layer, frames, args.samples)
        print(f"[render_hangar] {layer} done", flush=True)


if __name__ == "__main__":
    main(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])

"""Aircraft passes against a horizon-matched camera, for the Agricultural
base's sky layers (#582 concourse windows, #583 landing pad). Runs inside
Blender.

The camera is level at the origin looking down +Y with focal length
`focal_px` (plate px) and the horizon at plate row `horizon_y`, so a point
(x, depth, z) lands at (768 + f x / depth, horizon_y - f z / depth). Passes
are solved from screen targets (solve()), like mining/render_landing.py.
"""
import json
import math

import bpy

import render
import stage


def add_camera(sc, name, focal_px, horizon_y):
    data = bpy.data.cameras.new(name)
    data.sensor_fit, data.sensor_width = 'HORIZONTAL', 36.0
    data.lens = focal_px * 36.0 / stage.PLATE_W
    data.shift_y = (horizon_y - stage.PLATE_H / 2) / stage.PLATE_W   # frame up: horizon high
    data.clip_start, data.clip_end = 1.0, 20000.0
    cam = bpy.data.objects.new(name, data)
    sc.collection.objects.link(cam)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)             # level, looking +Y
    sc.camera = cam
    return cam


def solve(screen, depth, focal_px, horizon_y):
    """Screen target (px, py) at `depth` m -> world (x, depth, z)."""
    px, py = screen
    return ((px - stage.PLATE_W / 2) * depth / focal_px, depth,
            (horizon_y - py) * depth / focal_px)


def strobe(obj, frames, fps, every_s, on_s=0.1):
    """Flash `obj` for `on_s` every `every_s` seconds (keyed visibility)."""
    for f in range(1, frames + 1):
        obj.hide_render = ((f - 1) / fps % every_s) >= on_s
        obj.keyframe_insert("hide_render", frame=f)


def fly(root, start, end, frames, ease_in=0.0, bank_deg=0.0):
    """Straight flight from `start` to `end` (world points), nose along the
    track, pitched to the climb. `ease_in` 0..1 makes it accelerate (a
    departure; negative decelerates, an arrival). Returns the depth per
    frame."""
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


def render_pass(sc, root, out, frames, fps, quick=None, **info):
    """Render `root`'s frames as straight-alpha PNGs inside a border around
    it (off-screen frames write nothing), then pass.json with `frames`,
    `fps` and `info`. `quick` = "first:last[:step]" renders a sample."""
    out.mkdir(parents=True, exist_ok=True)
    if quick:
        first, last, *step = (int(v) for v in quick.split(":"))
        todo = range(first, min(last, frames) + 1, step[0] if step else 1)
    else:
        for old in out.glob("*.png"):           # stale frames must not survive
            old.unlink()
        todo = range(1, frames + 1)
    for f in todo:
        sc.frame_set(f)
        if not render.set_border(sc, [root], margin=0.15, footprint=False):
            continue
        sc.render.filepath = str(out / f"{f:04d}.png")
        bpy.ops.render.render(write_still=True)
    (out / "pass.json").write_text(json.dumps({"frames": frames, "fps": fps, **info}) + "\n")

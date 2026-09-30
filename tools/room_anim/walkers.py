"""Procedural pedestrian proxies for any camera-matched room (#515; shared in
#586). Runs inside Blender.

Deliberately low-detail: walkers are 30-100 px tall on screen, so
silhouette, swing and rim light sell them, not modelling detail.
"""
import math

import bpy

from stage import animate_path, box_object, material


def shaped_box(name, size, loc, mat, bevel=0.0, parent=None, nose_taper=1.0, nose_drop=0.0,
               base_flare=1.0):
    """Box with an optional wedge nose (+Y verts narrowed by `nose_taper`, top
    +Y verts lowered by `nose_drop` x height) and `base_flare` (bottom verts
    widened in X and Y, e.g. a coat hem)."""
    obj = box_object(name, size, loc, mat, bpy.context.scene.collection)
    for v in obj.data.vertices:
        if v.co.y > 0.0:
            v.co.x *= nose_taper
            if v.co.z > 0.0:
                v.co.z -= nose_drop * size[2]
        if v.co.z < 0.0:
            v.co.x *= base_flare
            v.co.y *= base_flare
    if bevel > 0.0:
        mod = obj.modifiers.new("Bevel", 'BEVEL')
        mod.width, mod.segments = bevel, 3
    for poly in obj.data.polygons:
        poly.use_smooth = True
    if parent is not None:
        obj.parent = parent
    return obj


def _pivot(name, loc, parent):
    empty = bpy.data.objects.new(name, None)
    empty.location, empty.parent = loc, parent
    bpy.context.scene.collection.objects.link(empty)
    return empty


def build_walker(name, coat_rgb, trousers_rgb=(0.05, 0.05, 0.06), skin_rgb=(0.45, 0.32, 0.25),
                 head_rgb=None):
    """A ~1.75 m pedestrian proxy facing +Y, origin at the feet. Returns
    (root, limbs) where limbs are the four swing pivots. `head_rgb` covers
    the head (a bandana, a hood); by default it's bare skin."""
    root = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(root)
    coat = material(f"{name}Coat", coat_rgb, roughness=0.7)
    legs = material(f"{name}Legs", trousers_rgb, roughness=0.8)
    skin = material(f"{name}Skin", skin_rgb, roughness=0.6)
    head = material(f"{name}Head", head_rgb, roughness=0.7) if head_rgb else skin

    # Overcoat from shoulders to knees, narrower at the waist than the
    # shoulders and flaring to the hem: reads as a person, not a crate.
    shaped_box("Chest", (0.44, 0.25, 0.36), (0, 0, 1.36), coat, bevel=0.07, parent=root,
               base_flare=0.82)
    shaped_box("Coat", (0.36, 0.22, 0.72), (0, 0, 0.84), coat, bevel=0.05, parent=root,
               base_flare=1.3)
    shaped_box("Head", (0.18, 0.2, 0.23), (0, 0.01, 1.68), head, bevel=0.085, parent=root)
    limbs = []
    for side, x in (("L", -0.09), ("R", 0.09)):
        hip = _pivot(f"{name}Hip{side}", (x, 0, 0.93), root)
        shaped_box(f"Leg{side}", (0.12, 0.14, 0.9), (0, 0, -0.45), legs, bevel=0.04, parent=hip)
        shoulder = _pivot(f"{name}Shoulder{side}", (x * 2.4, 0, 1.5), root)
        shaped_box(f"Arm{side}", (0.1, 0.11, 0.6), (0, 0, -0.3), coat, bevel=0.03,
                   parent=shoulder)
        limbs.append((hip, shoulder))
    return root, limbs


def animate_walk_path(root, limbs, points, z, fps, speed=1.3, step_m=0.72, swing_deg=24.0):
    """Walk the polyline `points` [(x, y), ...] on a floor at height `z`,
    beginning at frame 1 and turning smoothly through corners. Duration and
    gait both follow from `speed`, so the feet never slide. Returns the last
    frame."""
    stride_hz = speed / (2.0 * step_m)                      # one cycle = two steps

    def phase(f):
        return 2.0 * math.pi * stride_hz * (f - 1) / fps

    frame_end = animate_path(root, points, z, fps, speed,
                             lift=lambda f: 0.025 * abs(math.sin(phase(f))))
    for f in range(1, frame_end + 1):
        for i, (hip, shoulder) in enumerate(limbs):
            swing = math.radians(swing_deg) * math.sin(phase(f) + math.pi * i)
            hip.rotation_euler = (swing, 0.0, 0.0)
            shoulder.rotation_euler = (-0.8 * swing, 0.0, 0.0)
            hip.keyframe_insert("rotation_euler", frame=f)
            shoulder.keyframe_insert("rotation_euler", frame=f)
    return frame_end


def animate_walk(root, limbs, start, end, z, fps, speed=1.3, step_m=0.72, swing_deg=24.0):
    """Walk in a straight line from `start` to `end` (x, y). Returns the last
    frame."""
    return animate_walk_path(root, limbs, [start, end], z, fps, speed, step_m, swing_deg)

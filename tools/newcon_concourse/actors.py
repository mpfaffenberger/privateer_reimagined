"""Procedural actors for the New Con concourse layers (#515).

Deliberately low-detail proxies: the largest a hover-car ever gets on screen
is ~250 px wide and it spends most of its pass far smaller, so silhouette,
lights and floor interaction sell it, not modelling detail.
"""
import math

import bpy

from scene import AMBER, box_object, material


def _box(name, size, loc, mat, bevel=0.0, parent=None, nose_taper=1.0, nose_drop=0.0,
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


def build_hover_car(name="HoverCar", hull_rgb=(0.42, 0.44, 0.47)):
    """A ~4.6 m hover-car facing +Y, origin at its hover centre."""
    root = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(root)

    hull = material(f"{name}Hull", hull_rgb, metallic=0.85, roughness=0.32)
    glass = material(f"{name}Glass", (0.02, 0.03, 0.05), metallic=0.0, roughness=0.05)
    trim = material(f"{name}Trim", (0.06, 0.06, 0.07), metallic=0.9, roughness=0.45)
    head = material(f"{name}Head", (1.0, 0.9, 0.7), emission=(1.0, 0.85, 0.6), strength=20.0)
    tail = material(f"{name}Tail", (0.8, 0.05, 0.03), emission=(1.0, 0.06, 0.02), strength=6.0)
    glow = material(f"{name}Glow", AMBER, emission=AMBER, strength=4.0)

    _box("Hull", (2.0, 4.6, 0.6), (0, 0, 0), hull, bevel=0.22, parent=root,
         nose_taper=0.72, nose_drop=0.45)
    _box("Skirt", (2.1, 4.3, 0.16), (0, 0, -0.34), trim, bevel=0.06, parent=root,
         nose_taper=0.75)
    _box("Canopy", (1.4, 1.9, 0.48), (0, -0.4, 0.46), glass, bevel=0.2, parent=root,
         nose_taper=0.6, nose_drop=0.55)
    _box("Fin", (1.9, 0.35, 0.08), (0, -2.1, 0.62), trim, bevel=0.03, parent=root)
    _box("HeadL", (0.4, 0.06, 0.08), (-0.5, 2.3, -0.05), head, parent=root)
    _box("HeadR", (0.4, 0.06, 0.08), (0.5, 2.3, -0.05), head, parent=root)
    _box("TailStrip", (1.6, 0.06, 0.07), (0, -2.31, 0.1), tail, parent=root)
    _box("Underglow", (1.4, 3.2, 0.04), (0, 0, -0.44), glow, parent=root)
    # Warm light pooling on the deck under the car: this is what reads as
    # "hovering" in the reflection, even when the car is a few pixels tall.
    pool = bpy.data.lights.new(f"{name}Pool", 'AREA')
    pool.color, pool.energy, pool.size = AMBER, 70.0, 2.5
    pool_obj = bpy.data.objects.new(f"{name}Pool", pool)
    pool_obj.location, pool_obj.parent = (0, 0, -0.5), root
    bpy.context.scene.collection.objects.link(pool_obj)
    return root


def _pivot(name, loc, parent):
    empty = bpy.data.objects.new(name, None)
    empty.location, empty.parent = loc, parent
    bpy.context.scene.collection.objects.link(empty)
    return empty


def build_walker(name, coat_rgb, trousers_rgb=(0.05, 0.05, 0.06)):
    """A ~1.75 m pedestrian proxy facing +Y, origin at the feet. On screen
    they are 30-60 px tall: silhouette, swing and rim light are what count.
    Returns (root, limbs) where limbs are the four swing pivots."""
    root = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(root)
    coat = material(f"{name}Coat", coat_rgb, roughness=0.7)
    legs = material(f"{name}Legs", trousers_rgb, roughness=0.8)
    skin = material(f"{name}Skin", (0.45, 0.32, 0.25), roughness=0.6)

    # Overcoat from shoulders to knees, narrower at the waist than the
    # shoulders and flaring to the hem: reads as a person, not a crate.
    _box("Chest", (0.44, 0.25, 0.36), (0, 0, 1.36), coat, bevel=0.07, parent=root,
         base_flare=0.82)
    _box("Coat", (0.36, 0.22, 0.72), (0, 0, 0.84), coat, bevel=0.05, parent=root,
         base_flare=1.3)
    _box("Head", (0.18, 0.2, 0.23), (0, 0.01, 1.68), skin, bevel=0.085, parent=root)
    limbs = []
    for side, x in (("L", -0.09), ("R", 0.09)):
        hip = _pivot(f"{name}Hip{side}", (x, 0, 0.93), root)
        _box(f"Leg{side}", (0.12, 0.14, 0.9), (0, 0, -0.45), legs, bevel=0.04, parent=hip)
        shoulder = _pivot(f"{name}Shoulder{side}", (x * 2.4, 0, 1.5), root)
        _box(f"Arm{side}", (0.1, 0.11, 0.6), (0, 0, -0.3), coat, bevel=0.03, parent=shoulder)
        limbs.append((hip, shoulder))
    return root, limbs


def animate_walk(root, limbs, start, end, z, fps, speed=1.3, step_m=0.72, swing_deg=24.0):
    """Walk in a straight line from `start` to `end` (x, y) on a floor at
    height `z`, beginning at frame 1. Duration and gait both follow from
    `speed`, so the feet never slide. Returns the last frame."""
    dx, dy = end[0] - start[0], end[1] - start[1]
    root.rotation_euler = (0.0, 0.0, math.atan2(-dx, dy))   # +Y faces the direction
    frame_end = 1 + round(math.hypot(dx, dy) / speed * fps)
    stride_hz = speed / (2.0 * step_m)                      # one cycle = two steps
    span = frame_end - 1
    for f in range(1, frame_end + 1):
        t = (f - 1) / span
        phase = 2.0 * math.pi * stride_hz * (f - 1) / fps
        root.location = (start[0] + dx * t, start[1] + dy * t,
                         z + 0.025 * abs(math.sin(phase)))
        root.keyframe_insert("location", frame=f)
        for i, (hip, shoulder) in enumerate(limbs):
            swing = math.radians(swing_deg) * math.sin(phase + math.pi * i)
            hip.rotation_euler = (swing, 0.0, 0.0)
            shoulder.rotation_euler = (-0.8 * swing, 0.0, 0.0)
            hip.keyframe_insert("rotation_euler", frame=f)
            shoulder.keyframe_insert("rotation_euler", frame=f)
    return frame_end


def animate_straight_pass(obj, x, y_start, y_end, hover_z, frame_start, frame_end,
                          bob=0.06, heading_deg=0.0):
    """Constant-speed pass down the lane with a gentle hover bob. Every frame
    is keyed, so interpolation mode is irrelevant."""
    obj.rotation_euler = (0.0, 0.0, math.radians(heading_deg))
    span = frame_end - frame_start
    for f in range(frame_start, frame_end + 1):
        t = (f - frame_start) / span
        obj.location = (x, y_start + (y_end - y_start) * t,
                        hover_z + bob * math.sin(f * 0.35))
        obj.keyframe_insert("location", frame=f)

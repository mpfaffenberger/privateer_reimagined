"""Camera-matched Blender scene for the mining base concourse (#558).

The painting (assets/concourse/mining/concourse_bg.png) is a rock tunnel in
two-point perspective: the camera is level but yawed ~16 deg right of the
tunnel axis, so the cross-floor grates tilt toward a vanishing point far off
to the right.

Measured on the plate (tools/room_anim/README.md, "Mining"):
* tunnel-axis vanishing point (475, 557): the plated guide strip on the floor
  and the tunnel walls converge on the far lift doors,
* cross vanishing point (~4500, 565): Hough fit of the floor grates,
* so the horizon is y = 557, and f^2 = (768 - 475) * (4500 - 768), f ~ 1050 px
  (a 24.6 mm lens on 36 mm), yaw = atan((768 - 475) / f) ~ 15.6 deg.
Check: floor_point() puts two guide-strip points 14 m apart in depth at
X = 3.71 and 3.76 m, i.e. on one line along the axis, as a strip must be.

World: metres, +Y down the tunnel toward the far doors, +X to the right, Z up,
floor at Z = 0. The camera sits at the origin, EYE_HEIGHT up.

Pure bpy. Imported by mining/render_layers.py (run inside Blender).
"""
import math

import bpy

from stage import (EXPOSURE_EV, PLATE_H, PLATE_W, attach_plate_reference,  # noqa: F401
                   box_object, light, material, reset, setup_render)

HORIZON_Y = 557.0                   # plate px
AXIS_VP_X = 475.0                   # plate px, tunnel axis vanishing point
FOCAL_PX = 1050.0
SENSOR_MM = 36.0
LENS_MM = FOCAL_PX * SENSOR_MM / PLATE_W
YAW = math.atan((PLATE_W / 2 - AXIS_VP_X) / FOCAL_PX)   # radians, camera turned right
EYE_HEIGHT = 2.6                    # m; sets the scale (the wall terminal reads ~1.9 m)

GUIDE_X = 3.73                      # m, centre of the plated guide strip (floor_point)
GUIDE_WIDTH = 1.1                   # m

WARM_ROCK = (1.0, 0.55, 0.28)
LAMP_TEAL = (0.62, 0.86, 1.0)       # the cool wall lamps


def floor_point(px, py):
    """World (x, y) of the floor under plate pixel (px, py). Pure math, so
    constants derived from the painting stay traceable."""
    u = (px - PLATE_W / 2) / FOCAL_PX           # right, per unit depth
    v = (py - HORIZON_Y) / FOCAL_PX             # down from the horizon
    depth = EYE_HEIGHT / v
    fx, fy = math.sin(YAW), math.cos(YAW)       # camera forward on the floor
    rx, ry = math.cos(YAW), -math.sin(YAW)      # camera right
    return (depth * fx + u * depth * rx, depth * fy + u * depth * ry)


def add_plate_camera(scene):
    data = bpy.data.cameras.new("PlateCam")
    data.sensor_fit = 'HORIZONTAL'
    data.sensor_width = SENSOR_MM
    data.lens = LENS_MM
    # Level camera; the horizon sits below centre, so shift the frame (Blender
    # shift is in units of the larger sensor dimension, the width).
    data.shift_y = (HORIZON_Y - PLATE_H / 2) / PLATE_W
    data.clip_start, data.clip_end = 0.1, 500.0
    cam = bpy.data.objects.new("PlateCam", data)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, EYE_HEIGHT)
    cam.rotation_euler = (math.radians(90.0), 0.0, -YAW)   # level, yawed right
    scene.camera = cam
    return cam


def add_deck(scene):
    """Floor proxy: stone floor with a smoother metal guide strip. Visible in
    the beauty pass, holdout in the mask pass; only the change an actor makes
    to it is transferred onto the painting."""
    stone = material("StoneFloor", (0.08, 0.075, 0.07), roughness=0.6)
    steel = material("GuideStrip", (0.09, 0.09, 0.095), metallic=0.7, roughness=0.3)
    floor = box_object("Deck", (40.0, 200.0, 0.02), (0.0, 100.0, -0.01), stone,
                       scene.collection)
    strip = box_object("Guide", (GUIDE_WIDTH, 200.0, 0.02), (GUIDE_X, 100.0, -0.005), steel,
                       scene.collection)
    return [floor, strip]


def add_lights(scene):
    """The painting: warm orange light washing the rock from the right-hand
    shop alcoves and ceiling, cool teal wall lamps down both walls, dim warm
    ambience."""
    world = bpy.data.worlds.new("TunnelAir")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.03, 0.022, 0.018, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world
    # Warm fill from the right side (lit alcoves, orange rock bounce).
    light(scene, "RockBounce", 'AREA', (14.0, 30.0, 8.0), WARM_ROCK, 9000.0, size=30.0,
          rot=(math.radians(60.0), math.radians(40.0), 0.0))
    # Cool wall lamps high on both walls, every ~10 m down the tunnel.
    for i, y in enumerate(range(8, 90, 10)):
        for side, x in (("L", -6.0), ("R", 12.0)):
            light(scene, f"Lamp{side}{i:02d}", 'POINT', (x, float(y), 5.5), LAMP_TEAL,
                  900.0, size=0.4)

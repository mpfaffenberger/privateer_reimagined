"""Camera-matched Blender scene for the New Detroit concourse (#590).

The painting (assets/concourse/newdetroit/concourse_bg.png) is a wet, dark
plaza between towers: a bar under a red awning on the left, stairs up to a
second level at the back, and on the right a raised platform under the
hangar building, its pillars lit by blue kerb lamps and a lit doorway.

It's loose one-point perspective with vertical verticals, so the camera is
level with lens shift. Measured on the plate (tools/room_anim/README.md,
"New Detroit"):
* horizon y 555: the left wall's lamp strip, the floor grate channel and the
  platform kerb converge on it (they scatter in x from ~190 to ~430, so the
  vanishing point x 330 is their middle),
* eye 2.8 m: puts the bar stools' seats at 0.9 m and the platform door at
  2.0 m, with the landing at the top of the stairs ~2.4 m up,
* f 1100 px (a 25.8 mm lens): the painting has no second vanishing point, so
  this is a normal wide lens that keeps the pillars round (~1.5 m across).
floor_point() maps plate pixels to the floor, and every landmark below is
written as the plate pixel it was measured at.

World: metres, +Y away from the camera, +X right, Z up, plaza at Z 0. The
camera sits at the origin, EYE_HEIGHT up.

Pure bpy. Imported by newdetroit/render_layers.py (run inside Blender).
"""
import math

import bmesh
import bpy

from stage import (EXPOSURE_EV, PLATE_H, PLATE_W, attach_plate_reference,  # noqa: F401
                   box_object, light, material, reset, setup_render)

HORIZON_Y = 555.0                   # plate px
VP_X = 330.0                        # plate px, where depth lines converge
FOCAL_PX = 1100.0
SENSOR_MM = 36.0
LENS_MM = FOCAL_PX * SENSOR_MM / PLATE_W
EYE_HEIGHT = 2.8                    # m

PLATFORM_Z = 0.2                    # m, the raised platform under the hangar
WARM = (1.0, 0.55, 0.3)             # bar lamps, back-hall windows
BAR_RED = (1.0, 0.3, 0.16)          # the awning's glow
KERB_BLUE = (0.35, 0.62, 1.0)       # the platform's kerb lamps and panel
DOOR_WARM = (1.0, 0.78, 0.5)        # the platform's lit doorway


def floor_point(px, py, z=0.0):
    """World (x, y) of the point at height `z` seen at plate pixel (px, py)."""
    depth = FOCAL_PX * (EYE_HEIGHT - z) / (py - HORIZON_Y)
    return ((px - VP_X) * depth / FOCAL_PX, depth)


# Landmarks (plate px -> world), see the module docstring.
KERB_X = floor_point(950, 730)[0]                   # platform front edge, ~9.9 m
PILLARS = [                                         # (x, y, radius): base centre, shaft
    (*floor_point(1055, 735, PLATFORM_Z), 0.72),    # far pillar, shaft x 1000-1100
    (*floor_point(1310, 780, PLATFORM_Z), 0.78),    # near pillar, shaft x 1240-1380
]
BACK_WALL_X = floor_point(1415, 740, PLATFORM_Z)[0]  # the platform's back wall, ~15 m
DOOR_Y = floor_point(1415, 740, PLATFORM_Z)[1]       # the lit doorway's centre, ~15.5 m
DOOR_HALF_WIDTH = 0.42                               # x 1385-1445
CORNER = floor_point(290, 690)                       # left building's corner at the passage


def floor_height(x, y):
    """The plaza, and the platform one short step up past the kerb."""
    t = min(1.0, max(0.0, (x - KERB_X + 0.25) / 0.5))
    return PLATFORM_Z * t


def add_plate_camera(scene):
    data = bpy.data.cameras.new("PlateCam")
    data.sensor_fit = 'HORIZONTAL'
    data.sensor_width = SENSOR_MM
    data.lens = LENS_MM
    # Blender shift is in units of the larger sensor dimension (the width).
    data.shift_x = -(VP_X - PLATE_W / 2) / PLATE_W
    data.shift_y = (HORIZON_Y - PLATE_H / 2) / PLATE_W
    data.clip_start, data.clip_end = 0.1, 500.0
    cam = bpy.data.objects.new("PlateCam", data)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, EYE_HEIGHT)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)   # level, facing +Y
    scene.camera = cam
    return cam


def add_deck(scene):
    """Floor proxies: the wet plaza and the raised platform. Visible in the
    beauty pass, holdout in the mask pass; only the change an actor makes to
    them (reflection, shadow) is transferred onto the painting."""
    wet = material("WetPlaza", (0.05, 0.055, 0.06), metallic=0.6, roughness=0.12)
    plaza = box_object("Deck", (60.0, 120.0, 0.02), (0.0, 60.0, -0.01), wet,
                       scene.collection)
    width = BACK_WALL_X - KERB_X
    platform = box_object("Platform", (width, 120.0, PLATFORM_Z),
                          (KERB_X + width / 2, 60.0, PLATFORM_Z / 2), wet, scene.collection)
    return [plaza, platform]


def _cylinder(name, x, y, radius, z0, z1, mat, scene):
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=24, radius1=radius, radius2=radius,
                          depth=z1 - z0)
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    obj.location = (x, y, (z0 + z1) / 2)
    scene.collection.objects.link(obj)
    return obj


def add_occluders(scene):
    """Holdouts for painted things in front of the walkers: alpha 0 in every
    pass, so the bake leaves the painting untouched there."""
    hold = material("Holdout", (0.0, 0.0, 0.0))
    occ = [_cylinder(f"Pillar{i}", x, y, r, PLATFORM_Z, 12.0, hold, scene)
           for i, (x, y, r) in enumerate(PILLARS)]
    # The platform's back wall either side of the lit doorway, up to the
    # building's belly (the door walker starts inside, behind it).
    near, far = DOOR_Y - DOOR_HALF_WIDTH, DOOR_Y + DOOR_HALF_WIDTH
    for name, y0, y1 in (("WallNear", 8.0, near), ("WallFar", far, 22.0)):
        occ.append(box_object(name, (0.3, y1 - y0, 3.0),
                              (BACK_WALL_X + 0.15, (y0 + y1) / 2, 1.5), hold, scene.collection))
    # The left building's block, up to its corner at the side passage.
    cx, cy = CORNER
    occ.append(box_object("LeftBlock", (10.0, cy - 2.0, 12.0), (cx - 5.0, (cy + 2.0) / 2, 6.0),
                          hold, scene.collection))
    for obj in occ:
        obj.is_holdout = True
    return occ


def add_lights(scene):
    """The painting: a cold dark plaza lit warm from the bar and the back
    hall's windows, blue from the platform's kerb lamps and panel. The lights
    light the walkers but are never seen: not by the camera (a lamp behind a
    walker clipped to white and haloed him) and not mirrored in the proxy
    decks (static highlights cancel in the bake anyway; what's left is their
    noise, printed as coloured specks). The painting has its own reflections."""
    world = bpy.data.worlds.new("PlazaAir")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.02, 0.025, 0.035, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world
    # Area lights shine down their local -Z: +-90 deg about Y aims them at
    # -X / +X, -80 deg about X at the camera and a little down.
    # The bar's red-orange glow under the awning, facing into the plaza.
    lamps = [light(scene, "BarGlow", 'AREA', (-1.5, 11.0, 2.4), BAR_RED, 1500.0, size=4.0,
                   rot=(0.0, math.radians(-90.0), 0.0))]
    # Back hall: lit windows and stair lamps beyond the plaza, a warm back light.
    lamps.append(light(scene, "BackHall", 'AREA', (4.0, 40.0, 6.0), WARM, 8000.0, size=14.0,
                       rot=(math.radians(-80.0), 0.0, 0.0)))
    # Platform: blue kerb lamps at the pillar bases, the blue panel between
    # the pillars, and the lit doorway on the back wall.
    for i, (px, py) in enumerate(((1050, 740), (1240, 785))):     # on the kerb face
        y = floor_point(px, py, PLATFORM_Z)[1]
        lamps.append(light(scene, f"KerbLamp{i}", 'POINT', (KERB_X - 0.15, y, 0.1), KERB_BLUE,
                           60.0, size=0.2))
    panel_y = floor_point(1195, 680, PLATFORM_Z)[1]
    lamps.append(light(scene, "BluePanel", 'AREA', (BACK_WALL_X - 0.1, panel_y, 1.3),
                       KERB_BLUE, 120.0, size=1.6, rot=(0.0, math.radians(90.0), 0.0)))
    lamps.append(light(scene, "DoorSpill", 'AREA', (BACK_WALL_X + 1.5, DOOR_Y, 1.3),
                       DOOR_WARM, 150.0, size=0.8, rot=(0.0, math.radians(90.0), 0.0)))
    # Cool fill under the hangar building's belly, over the platform.
    lamps.append(light(scene, "PlatformFill", 'AREA', (12.5, 16.0, 2.3), (0.6, 0.75, 1.0),
                       40.0, size=4.0))
    for lamp in lamps:
        lamp.visible_camera = lamp.visible_glossy = False

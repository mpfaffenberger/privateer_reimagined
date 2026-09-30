"""Camera-matched Blender scene for the pirate base concourse (#586).

The painting (assets/concourse/pirate/concourse_bg.png) is a smugglers' rock
tunnel in one-point perspective, looking down a metal-plated floor toward a
fog-filled far hall through a rock arch. A side bay with rope barriers opens
off the right wall behind a rock pillar.

Measured on the plate (tools/room_anim/README.md, "Pirate concourse"):
* vanishing point (540, 430): the left wall's foot and the right kerb (the
  floor's lit edge strip) meet there. The ceiling pipes converge further
  right (~630, 397); the painter's tunnel bends, the floor is what we match.
* eye height 2.7 m: the painted drums (0.9 m) stand ~a third of their
  below-horizon distance tall (blue drum, bottom left: 110 px over 335 px;
  green drums by the bay: 55 over 145).
* focal 1024 px (a 24 mm lens on 36 mm). With a level camera, floor
  positions and upright sizes don't depend on the focal length at all
  (X = (px - vx) * eye / (py - hy), height = eye * h / (py - hy)); it only
  sets depth, i.e. how long things take to cross the floor.
So the floor runs from the left wall's foot at X -3.0 to the right kerb at
X 4.7; the far arch stands ~32.5 m out, and its hall floor ends ~42 m out.

World: metres, +Y down the tunnel, +X right, Z up, floor at Z = 0, camera at
the origin EYE_HEIGHT up. Pure bpy. Imported by pirate/render_layers.py.
"""
import math

import bpy

from stage import (EXPOSURE_EV, PLATE_H, PLATE_W, attach_plate_reference,  # noqa: F401
                   box_object, light, material, reset, setup_render)

VANISHING_POINT = (540.0, 430.0)    # plate px
FOCAL_PX = 1024.0
SENSOR_MM = 36.0
LENS_MM = FOCAL_PX * SENSOR_MM / PLATE_W
EYE_HEIGHT = 2.7                    # m

LEFT_WALL_X, KERB_X = -3.0, 4.7     # m, the floor's painted edges near the camera
ARCH_Y = 32.5                       # m, the far arch; its legs stand at X -3.8 and 3.5
ARCH_LEFT_X, ARCH_RIGHT_X = -3.8, 3.5
PILLAR_Y, PILLAR_X = 16.3, 5.1      # m, the rock pillar in front of the side bay

LANTERN = (1.0, 0.55, 0.25)         # the warm wall lanterns
COLD = (0.62, 0.8, 1.0)             # the blue-white wash on the floor
ROCK_AIR = (0.035, 0.026, 0.02)


def floor_point(px, py):
    """World (x, y) of the floor under plate pixel (px, py)."""
    depth = FOCAL_PX * EYE_HEIGHT / (py - VANISHING_POINT[1])
    return (px - VANISHING_POINT[0]) * depth / FOCAL_PX, depth


def add_plate_camera(scene):
    data = bpy.data.cameras.new("PlateCam")
    data.sensor_fit = 'HORIZONTAL'
    data.sensor_width = SENSOR_MM
    data.lens = LENS_MM
    # Level camera; Blender shift is in units of the larger sensor side.
    data.shift_x = -(VANISHING_POINT[0] - PLATE_W / 2) / PLATE_W
    data.shift_y = (VANISHING_POINT[1] - PLATE_H / 2) / PLATE_W
    data.clip_start, data.clip_end = 0.1, 500.0
    cam = bpy.data.objects.new("PlateCam", data)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, EYE_HEIGHT)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)    # level, facing +Y
    scene.camera = cam
    return cam


def add_deck(scene):
    """Floor proxy: the tunnel's worn, faintly glossy metal plates, out to
    the far hall. Visible in the beauty pass, holdout in the mask pass; only
    the change an actor makes to it is transferred onto the painting."""
    plates = material("DeckPlates", (0.07, 0.068, 0.066), metallic=0.5, roughness=0.32)
    return [box_object("Deck", (12.0, 60.0, 0.02), (0.8, 30.0, -0.01), plates,
                       scene.collection)]


def add_occluders(scene):
    """Holdouts for painted things that stand in front of the actors."""
    hold = material("Holdout", (0.0, 0.0, 0.0))
    boxes = [
        # The far arch's legs: walkers leave the far hall round them.
        box_object("ArchLeftHoldout", (8.0, 1.0, 9.0), (ARCH_LEFT_X - 4.0, ARCH_Y, 4.5), hold,
                   scene.collection),
        box_object("ArchRightHoldout", (8.0, 1.0, 9.0), (ARCH_RIGHT_X + 4.0, ARCH_Y, 4.5), hold,
                   scene.collection),
        # The rock pillar hiding the side bay's right half.
        box_object("PillarHoldout", (8.0, 1.0, 12.0), (PILLAR_X + 4.0, PILLAR_Y, 6.0), hold,
                   scene.collection),
        # The green drums by the bay (plate x 810-850, base y 575).
        box_object("DrumsHoldout", (1.2, 0.8, 0.95), (5.6, 19.0, 0.475), hold,
                   scene.collection),
    ]
    for obj in boxes:
        obj.is_holdout = True
    return boxes


def add_lights(scene):
    """The painting: warm lanterns down the left wall, a cold blue-white wash
    across the middle of the floor from the ceiling lamps on the right, and
    dim brown rock ambience."""
    world = bpy.data.worlds.new("TunnelAir")
    world.node_tree.nodes["Background"].inputs["Color"].default_value = (*ROCK_AIR, 1.0)
    scene.world = world
    light(scene, "ColdWash", 'AREA', (4.0, 14.0, 7.0), COLD, 2500.0, size=6.0,
          rot=(math.radians(-15.0), math.radians(25.0), 0.0))
    for i, y in enumerate(range(6, 40, 8)):
        light(scene, f"Lantern{i:02d}", 'POINT', (LEFT_WALL_X + 0.4, float(y), 3.2), LANTERN,
              400.0, size=0.2)
    light(scene, "ArchLantern", 'POINT', (0.0, ARCH_Y - 0.5, 5.0), LANTERN, 600.0, size=0.3)

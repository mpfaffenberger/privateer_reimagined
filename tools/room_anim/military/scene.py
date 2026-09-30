"""Camera-matched Blender scene for the Military concourse (#588).

The painting (assets/concourse/military/concourse_bg.png, 2564x2016, the
only concourse plate at this size) is a one-point view down a walkway to a
huge latticed window, with a vehicle lane (yellow dashes) along its right
between the walkway kerb and the teal ramps.

Measured on the plate (tools/room_anim/README.md, "Military concourse"):
* the lane's kerb line (x = 1.025 y - 41.6) and dash line (x = 1.140 y - 142.8),
  both least-squares fits, meet at the vanishing point (860, 880). The
  horizon lands on the window's lower transom, as it should for a level eye.
* the window's lattice is square to the frame (level, unyawed camera), so the
  off-centre vanishing point is lens shift, not yaw.
* the painted emblems' rings foreshorten to ~590 x 125 px at y ~1582, which
  for circles gives f = (1582 - 880) * 590 / 125 ~ 3300 px; their equal-armed
  stars say ~3060. f = 3100 px (a 43.5 mm lens on 36 mm).
* a floor line at lateral X satisfies (x - 860) / (y - 880) = X / EYE_HEIGHT:
  kerb 1.025, dashes 1.14, right lane kerb (the grey plinth under the teal
  ramps, far and near) ~1.36. EYE_HEIGHT sets the scale: 8 m makes the lane
  2.7 m wide, 1.8 m of it right of the dashes: room for a 1.5 m tug.

World: metres, +Y down the hall toward the window, +X to the right, Z up,
floor at Z = 0. The camera sits at the origin, EYE_HEIGHT up.

Pure bpy. Imported by military/render_layers.py and render_flyby.py.
"""
import math

import bpy

from stage import (EXPOSURE_EV, attach_plate_reference, box_object, light,  # noqa: F401
                   material, reset, setup_render)

PLATE_W, PLATE_H = 2564, 2016       # this plate, not stage's 1536x1024
VANISHING_POINT = (860.0, 880.0)    # plate px
FOCAL_PX = 3100.0
SENSOR_MM = 36.0
EYE_HEIGHT = 8.0                    # m

KERB_X = 1.025 * EYE_HEIGHT         # walkway | lane
DASH_X = 1.14 * EYE_HEIGHT          # the yellow dashes
RAMP_X = 1.36 * EYE_HEIGHT          # lane | teal ramps
LANE_X = (DASH_X + RAMP_X) / 2      # the traffic lane right of the dashes
WINDOW_Y = FOCAL_PX * EYE_HEIGHT / (1215.0 - VANISHING_POINT[1])  # sill at y 1215: ~74 m

TEAL = (0.45, 0.95, 0.9)            # the hall's teal walls, as bounce light
CEILING = (0.92, 0.96, 1.0)


def plate_px(x, y, z=0.0):
    """Plate pixel of world point (x, y, z). Pure math, for path planning."""
    vx, vy = VANISHING_POINT
    return vx + FOCAL_PX * x / y, vy + FOCAL_PX * (EYE_HEIGHT - z) / y


def setup(scene, samples=48):
    """stage.setup_render at this plate's size."""
    setup_render(scene, samples=samples)
    scene.render.resolution_x, scene.render.resolution_y = PLATE_W, PLATE_H


def add_plate_camera(scene):
    data = bpy.data.cameras.new("PlateCam")
    data.sensor_fit = 'HORIZONTAL'
    data.sensor_width = SENSOR_MM
    data.lens = FOCAL_PX * SENSOR_MM / PLATE_W
    # Level, unyawed; the vanishing point sits left of and above centre, so
    # shift the frame (Blender shift is in units of the sensor width).
    data.shift_x = (PLATE_W / 2 - VANISHING_POINT[0]) / PLATE_W
    data.shift_y = (VANISHING_POINT[1] - PLATE_H / 2) / PLATE_W
    data.clip_start, data.clip_end = 0.1, 5000.0
    cam = bpy.data.objects.new("PlateCam", data)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, EYE_HEIGHT)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)     # level, looking +Y
    scene.camera = cam
    return cam


def add_deck(scene):
    """Floor proxy: a satin lane deck. Visible in the beauty pass, holdout in
    the mask pass; only the change an actor makes to it (shadow, headlight
    pools) is transferred onto the painted floor."""
    deck = material("LaneDeck", (0.12, 0.09, 0.08), metallic=0.2, roughness=0.45)
    floor = box_object("Deck", (60.0, 120.0, 0.02), (KERB_X + 10.0, 55.0, -0.01), deck,
                       scene.collection)
    return [floor]


def add_occluders(scene):
    """Holdouts for painted things in front of the actors (alpha 0 in every
    pass, so the bake leaves the painting alone there): the teal ramp, whose
    plinth is the lane's right kerb. At the far end the lane bends right
    under it, so traffic enters from behind it."""
    hold = material("Holdout", (0.0, 0.0, 0.0))
    ramp = box_object("RampHoldout", (20.0, 30.0, 10.0), (RAMP_X + 10.0, WINDOW_Y - 12.0, 5.0),
                      hold, scene.collection)
    ramp.is_holdout = True
    return [ramp]


def add_lights(scene):
    """The painting: an evenly lit hall under a cool ceiling, teal bounce off
    the walls and ramps, the window black."""
    world = bpy.data.worlds.new("HallAir")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.025, 0.04, 0.04, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world
    # Broad ceiling light over the lane, a little left (the walkway side).
    light(scene, "Ceiling", 'AREA', (KERB_X - 4.0, 38.0, 18.0), CEILING, 36000.0, size=30.0)
    # Teal bounce off the ramps on the right (area lights shine down local
    # -Z; +90 deg about Y turns that to -X, toward the lane).
    light(scene, "RampBounce", 'AREA', (RAMP_X + 6.0, 35.0, 3.0), TEAL, 9000.0, size=20.0,
          rot=(0.0, math.radians(90.0), 0.0))

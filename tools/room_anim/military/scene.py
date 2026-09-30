"""Camera-matched Blender scene for the Military concourse (#588).

The painting (assets/concourse/military/concourse_bg.png, 1536x1024, the
#621 repaint) is a one-point view down a walkway to a huge brass-latticed
window, with a vehicle lane (orange dashes) along its right between the
walkway and the armoured ramp.

Measured on the plate (tools/room_anim/README.md, "Military concourse"):
* the walkway's left kerb lamps (x = -0.6433 y + 731.7, all 7 on the line)
  and the lane's dashes (x = 1.2557 y - 54.5, all 16), least-squares fits,
  meet at the vanishing point (465, 414), on the window's lower transom.
* the lattice is square to the frame (level, unyawed camera), so the
  off-centre vanishing point is lens shift, not yaw.
* the middle emblem's ring foreshortens to ~325 x 68 px at y ~808, which
  for a circle gives f = (808 - 414) * 325 / 68 ~ 1880 px. f = 1860 px (a
  43.6 mm lens on 36 mm), the old plate's lens too (3100 px at 2564 wide).
* a floor line at lateral X satisfies (x - 465) / (y - 414) = X / EYE_HEIGHT:
  dashes 1.256, the lamps along the ramp's foot 1.565 (6 lamps through the
  vanishing point). EYE_HEIGHT sets the scale: 8 m puts 2.5 m of lane right
  of the dashes, room for the 1.5 m tug and 2.2 m hopper.

World: metres, +Y down the hall toward the window, +X to the right, Z up,
floor at Z = 0. The camera sits at the origin, EYE_HEIGHT up.

Pure bpy. Imported by military/render_layers.py and render_flyby.py.
"""
import math

import bpy

from stage import (EXPOSURE_EV, PLATE_H, PLATE_W, attach_plate_reference,  # noqa: F401
                   box_object, light, material, reset, setup_render)

VANISHING_POINT = (465.4, 414.0)    # plate px
FOCAL_PX = 1860.0
SENSOR_MM = 36.0
EYE_HEIGHT = 8.0                    # m

DASH_X = 1.2557 * EYE_HEIGHT        # the orange dashes
RAMP_X = 1.565 * EYE_HEIGHT         # lane | armoured ramp (its foot lamps)
LANE_X = DASH_X + 1.1               # the traffic lane right of the dashes; the
                                    # hopper's rack clears the ramp-foot gutter
KERB_X = DASH_X - 1.0               # walkway | lane, left of the dashes
WINDOW_Y = FOCAL_PX * EYE_HEIGHT / (595.0 - VANISHING_POINT[1])  # sill at y 595: ~82 m

STEEL = (0.62, 0.74, 0.95)          # cool hall light off the gunmetal plating
LAMP_WARM = (1.0, 0.72, 0.42)       # the warm lamps along the ramp's foot
CEILING = (0.9, 0.95, 1.0)


def plate_px(x, y, z=0.0):
    """Plate pixel of world point (x, y, z). Pure math, for path planning."""
    vx, vy = VANISHING_POINT
    return vx + FOCAL_PX * x / y, vy + FOCAL_PX * (EYE_HEIGHT - z) / y


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
    # The painted floor is glossy gunmetal: the deck mirrors the train too.
    deck = material("LaneDeck", (0.07, 0.075, 0.08), metallic=0.6, roughness=0.3)
    floor = box_object("Deck", (60.0, 120.0, 0.02), (KERB_X + 10.0, 55.0, -0.01), deck,
                       scene.collection)
    return [floor]


def add_occluders(scene):
    """Holdouts for painted things in front of the actors (alpha 0 in every
    pass, so the bake leaves the painting alone there): the armoured ramp,
    whose foot is the lane's right edge. Traffic enters from behind its far
    end, where the lit terminal stands."""
    hold = material("Holdout", (0.0, 0.0, 0.0))
    ramp = box_object("RampHoldout", (20.0, 30.0, 10.0), (RAMP_X + 10.0, WINDOW_Y - 12.0, 5.0),
                      hold, scene.collection)
    ramp.is_holdout = True
    return [ramp]


def add_lights(scene):
    """The painting: a dim gunmetal hall under cool strip lights, the window
    black, warm lamps along the ramp's foot pooling on the glossy floor."""
    world = bpy.data.worlds.new("HallAir")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.012, 0.016, 0.022, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world
    # Cool strip lights overhead, a little left of the lane.
    light(scene, "Ceiling", 'AREA', (DASH_X - 4.0, 45.0, 20.0), CEILING, 22000.0, size=30.0)
    # Cool bounce off the ramp's gunmetal plating on the right (area lights
    # shine down local -Z; +90 deg about Y turns that to -X, toward the lane).
    light(scene, "RampBounce", 'AREA', (RAMP_X + 6.0, 40.0, 3.0), STEEL, 5000.0, size=20.0,
          rot=(0.0, math.radians(90.0), 0.0))
    # Warm lamps along the ramp's foot, every ~6 m.
    for i, y in enumerate(range(14, 80, 6)):
        light(scene, f"FootLamp{i:02d}", 'POINT', (RAMP_X + 0.2, float(y), 0.3), LAMP_WARM,
              60.0, size=0.15)

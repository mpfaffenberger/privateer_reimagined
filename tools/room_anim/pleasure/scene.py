"""Camera-matched Blender scene for the Pleasure concourse walkers (#598).

The painting (assets/concourse/pleasure/concourse_bg.png, #596) is a casino
hall: three round red couches in the middle ground, and behind them a strip
of carpet along the back wall from the bar's archway on the left, past the
great doors and the commodity kiosk, to a planter and pillar on the right.
That strip is where people walk.

It's a level one-point view with vertical verticals, so the camera is level
with lens shift. Measured on the plate for #594: horizon y 640, vanishing
point x 800, f 1150 px. The painting's scale isn't consistent, and the eye
height sets how tall a walker is against everything: at #594's 1.7 m the
seats are a proper 0.47 m, but the landing-pad door's top is on the
horizon (a 1.7 m door), the arch's capitals 1.46 m up and the kiosk's
screen at hip height. 1.9 m splits it: a 1.9 m door, 1.64 m capitals, a
1.27 m screen, 0.52 m seats, and a 1.75 m walker ~160 px tall on the back
lane. floor_point() maps plate pixels to the floor, and every holdout
below is written as the plate pixels it was traced at, at a depth given as
the plate row of a floor point, so they hold whatever the eye height.

World: metres, +Y away from the camera, +X right, Z up, floor at Z 0. The
camera sits at the origin, EYE_HEIGHT up.

Pure bpy. Imported by pleasure/render_layers.py (run inside Blender).
"""
import json
import math
from pathlib import Path

import bpy

from stage import (EXPOSURE_EV, PLATE_H, PLATE_W, attach_plate_reference,  # noqa: F401
                   box_object, light, material, reset, setup_render)

HORIZON_Y = 640.0                   # plate px
VP_X = 800.0                        # plate px, where depth lines converge
FOCAL_PX = 1150.0
SENSOR_MM = 36.0
LENS_MM = FOCAL_PX * SENSOR_MM / PLATE_W
EYE_HEIGHT = 1.9                    # m, see above

WARM = (1.0, 0.72, 0.42)            # sconces, the gold uplights, the marquee bulbs
CHANDELIER = (1.0, 0.82, 0.6)
NEON_RED = (1.0, 0.12, 0.1)
NEON_CYAN = (0.2, 0.75, 1.0)
SCREEN_BLUE = (0.35, 0.5, 1.0)      # the ship-rental billboard


def floor_depth(py):
    """Depth (m) of the floor seen at plate row `py`."""
    return FOCAL_PX * EYE_HEIGHT / (py - HORIZON_Y)


# Holdout cards, each stood upright at one depth (m) and covering a polygon
# traced on the plate (px). Upright is square to this level camera, so a
# card hides exactly the painted outline of whatever's in front of a walker.
# Every couch, table and the planter stands at the potted plant's base
# (y 836), nearer than any lane (the lanes' feet are at y 830 or above);
# the bar arch's wall at its threshold (y 798), behind the lanes and in
# front of the walkers' starts inside the bar.
COUCH_DEPTH = floor_depth(836)
ARCH_DEPTH = floor_depth(798)
LANE_DEPTH = floor_depth(812)       # the back lanes, for lights over them
WALL_DEPTH = floor_depth(800)       # the neon pillar and the back wall by the doors
COUCHES = json.loads((Path(__file__).resolve().parent / "couches.json").read_text())["couches"]
SILHOUETTES = {
    # The brass side tables (at 8x): top, candle lantern, post. Under a
    # table top there's open floor, so a walker's legs show past the post.
    "TableLeft": (COUCH_DEPTH, [(494, 793), (517, 793), (517, 781), (525, 781), (525, 793),
                                (548, 793), (548, 798), (523, 798), (523, 804), (518, 804),
                                (518, 798), (494, 798)]),
    "TableMidLeft": (COUCH_DEPTH, [(620, 789), (643, 789), (643, 775), (651, 775), (651, 789),
                                   (672, 789), (672, 793), (652, 793), (652, 805), (645, 805),
                                   (645, 793), (620, 793)]),
    "TableCentre": (COUCH_DEPTH, [(853, 797), (881, 797), (881, 784), (886, 784), (886, 797),
                                  (899, 797), (899, 801), (892, 801), (892, 814), (877, 814),
                                  (877, 801), (853, 801)]),
    "TableRight": (COUCH_DEPTH, [(1145, 793), (1153, 793), (1153, 781), (1160, 781),
                                 (1160, 793), (1181, 793), (1181, 797), (1158, 797),
                                 (1158, 806), (1155, 806), (1155, 797), (1145, 797)]),
    "TableFarRight": (COUCH_DEPTH, [(1238, 793), (1266, 793), (1266, 797), (1254, 797),
                                    (1254, 803), (1250, 803), (1250, 797), (1238, 797)]),
    # The pillar, the palm on the planter and the small potted plant's pot
    # at the right edge: both walkers come and go behind them.
    "Planter": (COUCH_DEPTH, [(1449, 0), (1536, 0), (1536, 1024), (1439, 1024), (1439, 836),
                              (1432, 836), (1432, 790), (1439, 790), (1439, 779),
                              (1449, 779)]),
    # The bar archway's left jamb and the arch's curve over the opening
    # (columns' inner edges x 342 and 492, capitals y 662, apex y 575):
    # the walkers step out from behind it.
    "ArchJamb": (ARCH_DEPTH, [(150, 540), (417, 540), (417, 575), (380, 587), (352, 618),
                              (345, 640), (342, 662), (342, 1024), (150, 1024)]),
}


def floor_point(px, py):
    """World (x, y) of the floor point seen at plate pixel (px, py)."""
    depth = floor_depth(py)
    return ((px - VP_X) * depth / FOCAL_PX, depth)


def plate_point(px, py, depth):
    """World (x, y, z) seen at plate pixel (px, py), `depth` m ahead."""
    s = depth / FOCAL_PX
    return ((px - VP_X) * s, depth, EYE_HEIGHT - (py - HORIZON_Y) * s)


def add_plate_camera(scene):
    data = bpy.data.cameras.new("PlateCam")
    data.sensor_fit = 'HORIZONTAL'
    data.sensor_width = SENSOR_MM
    data.lens = LENS_MM
    # Blender shift is in units of the larger sensor dimension (the width).
    data.shift_x = -(VP_X - PLATE_W / 2) / PLATE_W
    data.shift_y = (HORIZON_Y - PLATE_H / 2) / PLATE_W
    data.clip_start, data.clip_end = 0.1, 200.0
    cam = bpy.data.objects.new("PlateCam", data)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, EYE_HEIGHT)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)   # level, facing +Y
    scene.camera = cam
    return cam


def add_deck(scene):
    """The carpet. Visible in the beauty pass, holdout in the mask pass: only
    the shadow a walker casts moves onto the painted floor."""
    carpet = material("Carpet", (0.12, 0.035, 0.025), roughness=0.85)
    return [box_object("Deck", (40.0, 60.0, 0.02), (0.0, 30.0, -0.01), carpet,
                       scene.collection)]


def _card(name, depth, polygon_px, mat, collection):
    """An upright polygon covering `polygon_px` on the plate at `depth`. Seen
    by the camera only, so it casts no shadow and blocks no light."""
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([plate_point(px, py, depth) for px, py in polygon_px], [],
                     [list(range(len(polygon_px)))])
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    obj.visible_shadow = obj.visible_diffuse = obj.visible_glossy = False
    obj.visible_transmission = obj.visible_volume_scatter = False
    return obj


def add_occluders(scene):
    """Holdouts: alpha 0 in every pass, so the bake leaves the painting as
    painted wherever they stand in front of a walker."""
    hold = material("Holdout", (0.0, 0.0, 0.0))
    cards = {f"Couches{i}": (COUCH_DEPTH, poly) for i, poly in enumerate(COUCHES)}
    cards.update(SILHOUETTES)
    occ = [_card(name, depth, poly, hold, scene.collection)
           for name, (depth, poly) in cards.items()]
    for obj in occ:
        obj.is_holdout = True
    return occ


def add_lights(scene):
    """The painting's light: a warm hall (gold walls, a chandelier over the
    aisle, sconces by the arch, uplights at the pillars), the red and cyan
    neon on the pillar by the doors, the blue billboard and the marquee bulbs
    over the left. Never seen by the camera or in glossy rays: a lamp behind
    a walker would clip and halo him (see New Detroit), and the painting has
    its own highlights."""
    world = bpy.data.worlds.new("HallAir")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.05, 0.035, 0.022, 1.0)   # gold-wall bounce
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world
    down = (0.0, 0.0, 0.0)                         # area lights shine down local -Z
    toward_camera = (math.radians(-90.0), 0.0, 0.0)
    lamps = [
        light(scene, "Chandelier", 'POINT', plate_point(802, 400, LANE_DEPTH), CHANDELIER,
              450.0, size=0.6),
        light(scene, "BarGlow", 'POINT', plate_point(420, 700, ARCH_DEPTH + 2.5), WARM, 120.0,
              size=0.8),
        # Neon tubes on the pillar beside the doors.
        light(scene, "NeonRed", 'AREA', plate_point(977, 500, WALL_DEPTH - 0.2), NEON_RED,
              120.0, size=0.5, rot=toward_camera),
        light(scene, "NeonCyan", 'AREA', plate_point(1032, 460, WALL_DEPTH - 0.2), NEON_CYAN,
              120.0, size=0.6, rot=toward_camera),
        # The billboard hangs high on the right, facing down into the hall.
        light(scene, "Billboard", 'AREA', (4.5, 8.5, 4.5), SCREEN_BLUE, 400.0, size=3.0,
              rot=(math.radians(-35.0), math.radians(-30.0), 0.0)),
        # The marquee's bulb bank overhead on the left.
        light(scene, "Marquee", 'AREA', (-4.5, 7.0, 4.5), WARM, 600.0, size=4.0,
              rot=(math.radians(-25.0), math.radians(35.0), 0.0)),
        light(scene, "Ceiling", 'AREA', (0.0, 10.0, 6.0), CHANDELIER, 300.0, size=8.0, rot=down),
    ]
    # Sconces either side of the bar arch, and the gold uplights at the
    # neon pillar's base and the landing-pad door frame.
    for name, (px, py, depth) in {"SconceL": (283, 630, ARCH_DEPTH),
                                  "SconceR": (553, 625, ARCH_DEPTH),
                                  "UplightL": (1012, 720, WALL_DEPTH),
                                  "UplightR": (1065, 720, WALL_DEPTH),
                                  "DoorFrame": (1255, 720, floor_depth(822))}.items():
        x, y, z = plate_point(px, py, depth)
        lamps.append(light(scene, name, 'POINT', (x, y - 0.25, z), WARM, 40.0, size=0.15))
    for lamp in lamps:
        lamp.visible_camera = lamp.visible_glossy = False

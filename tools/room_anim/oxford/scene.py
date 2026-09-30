"""Camera-matched Blender scene for the Oxford concourse (#592).

The camera comes from camera_match.py (pure math, fitted to the painting's
vanishing points); everything here is placed in its street frame (u, v):

* a proxy street deck at Z = 0 and the flower beds as low boxes: visible in
  the beauty pass, holdout in the mask pass, so only the light an actor adds
  or takes away (headlights, glow, shadow) moves onto the painted street,
* holdouts for painted things that stand in front of the traffic (the clock
  tower, the market tents, lamps and a banner), as painted silhouettes,
* night lighting like the painting's: dim blue sky ambience, warm street
  lamps along the kerbs.

Pure bpy. Imported by render_layers.py (run inside Blender).
"""
import math

import bpy
from mathutils import Matrix

import camera_match as cm
from stage import (EXPOSURE_EV, PLATE_H, PLATE_W, attach_plate_reference,  # noqa: F401
                   box_object, light, material, reset, setup_render)

LAMP_WARM = (1.0, 0.72, 0.45)       # the sodium-warm lamp heads
# Calibrated against the plate (empty deck vs painted street, linear light):
# the painting's night is bright and cool, with warm pools under the lamps.
NIGHT_SKY = (0.26, 0.29, 0.36)
LAMP_W = 300.0                      # per street lamp; more blew the car out to orange

# Flower beds (u0, u1, v0, v1), read off the rectified plate
# (build/room_anim/oxford/map.png): central, left, right, top.
BEDS = [(0.0, 20.0, 0.0, 20.0), (-25.6, -5.6, 0.0, 18.8), (-3.8, 17.5, 27.0, 46.0),
        (-27.0, -6.0, 27.0, 46.0)]
BED_HEIGHT = 0.8                    # m, the walled beds' kerb


def add_plate_camera(scene):
    data = bpy.data.cameras.new("PlateCam")
    data.sensor_fit = 'HORIZONTAL'
    data.sensor_width = cm.SENSOR_MM
    data.lens = cm.LENS_MM
    data.clip_start, data.clip_end = 1.0, 1000.0
    cam = bpy.data.objects.new("PlateCam", data)
    scene.collection.objects.link(cam)
    # Level and facing +Y is X 90 deg; pitch it down, then roll about the
    # view axis (the camera's local Z), same sign as camera_match's roll
    # (render_layers.py --check holds the two to within 0.5 px).
    cam.matrix_world = (Matrix.Translation((0.0, 0.0, cm.EYE_HEIGHT)) @
                        Matrix.Rotation(math.radians(90.0 - cm.PITCH_DEG), 4, 'X') @
                        Matrix.Rotation(math.radians(cm.ROLL_DEG), 4, 'Z'))
    scene.camera = cam
    return cam


def street_box(name, u0, u1, v0, v1, z0, z1, mat, collection):
    """A box spanning street-frame (u0..u1, v0..v1), from z0 to z1 m."""
    x, y = cm.to_world((u0 + u1) / 2, (v0 + v1) / 2)
    obj = box_object(name, (u1 - u0, v1 - v0, z1 - z0), (x, y, (z0 + z1) / 2), mat, collection)
    obj.rotation_euler = (0.0, 0.0, math.radians(cm.GRID_DEG))
    return obj


def add_deck(scene):
    """The paved streets and the raised flower beds."""
    paving = material("Paving", (0.24, 0.22, 0.2), roughness=0.45)
    beds = material("Beds", (0.03, 0.05, 0.02), roughness=0.9)
    col = scene.collection
    decks = [street_box("Deck", -120.0, 120.0, -80.0, 140.0, -0.02, 0.0, paving, col)]
    decks += [street_box(f"Bed{i}", *b, 0.0, BED_HEIGHT, beds, col) for i, b in enumerate(BEDS)]
    return decks


# Painted things in front of the traffic, traced on the plate (px) and stood
# at the view depth of their footprint (u, v): anything farther from the
# camera is hidden exactly where the paint is. 3D fits missed: the tents'
# eave height was a guess (the car showed over their canopies), and a
# grid-aligned tower box is seen corner-on, 190 px wide to the spire's 125.
SILHOUETTES = {
    # The striped market tents on the avenue, roof and stall frame, from
    # their fitted centres (a 7 x 6.3 m footprint from the eave corners).
    "TentNorth": ((27.1, 4.5), [(962, 842), (1072, 772), (1215, 852), (1215, 878),
                                (1110, 972), (963, 878)]),
    "TentSouth": ((27.3, -4.2), [(950, 866), (1078, 952), (1080, 1024), (815, 1024),
                                 (815, 955)]),
    # The lamp on the avenue's east kerb (base 1392, 545: u 22.0, a metre
    # nearer than the lane).
    "AvenueLamp": ((22.0, 34.8), [(1387, 489), (1398, 489), (1398, 548), (1387, 548)]),
    # The clock tower from its axis (u -2.5, v 20.6: projects up the painted
    # spire): pinnacles, clock block, shaft. The top car passes behind it.
    "ClockTower": ((-2.5, 20.6), [(753, 0), (892, 0), (890, 305), (870, 312), (870, 470),
                                  (775, 470), (775, 312), (757, 305)]),
    # The roof at the top edge that the banner street runs out under: the
    # top car comes out from behind it.
    "TopLeftRoof": ((-47.0, 46.0), [(470, 0), (646, 0), (646, 52), (610, 64), (470, 64)]),
    # The red banner pole on the right bed's corner (base 1243, 344: v 45.3).
    "GardenBanner": ((5.6, 45.3), [(1241, 243), (1264, 243), (1263, 323), (1251, 323),
                                   (1250, 344), (1236, 346), (1237, 300), (1241, 300)]),
    # The lamp at the avenue crossing (base 1495, 428).
    "CrossingLamp": ((20.0, 47.1), [(1489, 348), (1511, 348), (1505, 430), (1490, 430)]),
}


def silhouette(name, anchor_uv, polygon_px, mat, collection):
    """A flat camera-facing polygon covering `polygon_px` on the plate, at
    the view depth of street point `anchor_uv`. Seen by the camera only, so
    it casts no shadow and blocks no light."""
    d = cm.depth(*cm.to_world(*anchor_uv))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([cm.at_depth(px, py, d) for px, py in polygon_px], [],
                     [list(range(len(polygon_px)))])
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    obj.visible_shadow = obj.visible_diffuse = obj.visible_glossy = False
    obj.visible_transmission = obj.visible_volume_scatter = False
    return obj


def add_occluders(scene):
    """Holdouts: alpha 0 in every pass, so the bake leaves the painting as
    painted wherever they stand in front of an actor."""
    hold = material("Holdout", (0.0, 0.0, 0.0))
    occluders = [silhouette(name, uv, poly, hold, scene.collection)
                 for name, (uv, poly) in SILHOUETTES.items()]
    for obj in occluders:
        obj.is_holdout = True
    return occluders


def add_lights(scene):
    world = bpy.data.worlds.new("OxfordNight")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (*NIGHT_SKY, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world
    # Warm street lamps, 4 m up, every 12 m along both kerbs of the avenue
    # (u 20 / 25) and the top street (v 46 / 52), like the painted ones.
    for i, t in enumerate(range(-24, 72, 12)):
        for side, u in (("W", 19.6), ("E", 25.4)):
            light(scene, f"AvenueLamp{side}{i:02d}", 'POINT', (*cm.to_world(u, float(t)), 4.0),
                  LAMP_WARM, LAMP_W, size=0.3)
    for i, t in enumerate(range(-60, 40, 12)):
        for side, v in (("S", 46.4), ("N", 52.0)):
            light(scene, f"TopLamp{side}{i:02d}", 'POINT', (*cm.to_world(float(t), v), 4.0),
                  LAMP_WARM, LAMP_W, size=0.3)

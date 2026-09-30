"""Camera-matched Blender scene for the Refinery concourse (#584).

The painting (assets/concourse/refinery/concourse_bg.png) looks down from a
high balcony into a round, two-level atrium: a ring of floor around a sunken
garden, shopfronts and cargo bays around its wall, and a central column on a
bridge in the foreground. Above, the dome's glass looks out on space and the
refinery.

Measured on the plate (README, "Refinery"):
* the column and the cable hanging down to it are vertical at x ~1135, so
  that's the principal point's column (lens shift right); the atrium's axis
  is there too, since the floor rings are symmetric about it,
* the wall's foot (floor meets shopfronts) is an ellipse through (1125, 703),
  (1400, 718), (1500, 735), (720, 775), (475, 828), (380, 880), (300, 1000),
  and the floor lamps near the garden sit on an inner one,
* a level-lens least-squares fit of both rings as concentric circles
  (f = 1100 px fixed; the rings alone can't separate focal length from pitch)
  gives: pitched 9.5 deg down, the camera 1.17 R back from the axis and
  0.75 R up, the lamp ring at 0.56 R. The axis foot then lands at y 984,
  under the painted column, which the fit never saw.
* scale: the cargo bay doors at the back are ~120 px tall; at 4.7 m that
  makes the wall radius R = 20 m. The door (x 805-1000) is then the wall
  between -38 and -16 deg, and its 4.7 m lintel lands on the painted one
  (y ~590).

World: metres, the atrium axis is the Z axis, floor at Z = 0, the camera on
-Y looking toward +Y. Pure bpy. Imported by refinery/render_layers.py.
"""
import math

import bmesh
import bpy

from stage import (EXPOSURE_EV, PLATE_H, PLATE_W, attach_plate_reference,  # noqa: F401
                   box_object, light, material, reset, setup_render)

AXIS_X = 1135.0                     # plate px: principal point column = atrium axis
FOCAL_PX = 1100.0
SENSOR_MM = 36.0
LENS_MM = FOCAL_PX * SENSOR_MM / PLATE_W
PITCH = math.radians(9.5)           # down from level

WALL_R = 20.0                       # m, the wall's foot (shopfronts, bays)
LAMP_R = 0.56 * WALL_R              # m, the floor lamps' ring by the garden
GARDEN_R = 0.52 * WALL_R            # m, the planters' outer edge
GARDEN_HEIGHT = 2.2                 # m, the foliage's top, roughly
BAY_DOOR = (-38.0, -16.0)           # deg (floor_xy), the cargo bay's opening
BAY_DEPTH = 6.0                     # m of bay floor behind the wall
CAM_BACK = 1.17 * WALL_R            # m, camera distance from the axis (-Y)
EYE_HEIGHT = 0.75 * WALL_R          # m above the floor
CAMERA_AT = (0.0, -CAM_BACK, EYE_HEIGHT)

WARM = (1.0, 0.66, 0.36)            # shop and bay light
COOL = (0.62, 0.78, 1.0)            # the dome's starlight, blue strip lamps


def floor_xy(radius, angle_deg):
    """World (x, y) on the floor at `radius` from the axis. Angle 0 is the
    far side (+Y, the back wall), positive clockwise seen from above, i.e.
    toward the right of the picture."""
    a = math.radians(angle_deg)
    return radius * math.sin(a), radius * math.cos(a)


def to_plate(x, y, z=0.0):
    """Plate pixel of world point (x, y, z). Pure math, so constants stay
    traceable to the painting and paths can be planned in screen space."""
    dy, dz = y + CAM_BACK, z - EYE_HEIGHT
    depth = dy * math.cos(PITCH) - dz * math.sin(PITCH)
    up = dy * math.sin(PITCH) + dz * math.cos(PITCH)
    return AXIS_X + FOCAL_PX * x / depth, PLATE_H / 2 - FOCAL_PX * up / depth


def plate_ray(px, py):
    """Unit world direction from the camera through plate pixel (px, py)."""
    x, up = (px - AXIS_X) / FOCAL_PX, (PLATE_H / 2 - py) / FOCAL_PX
    s, c = math.sin(PITCH), math.cos(PITCH)
    d = (x, c + up * s, -s + up * c)                 # forward + x right + up * up
    n = math.sqrt(sum(v * v for v in d))
    return tuple(v / n for v in d)


def plate_point(px, py, distance):
    """World point `distance` metres from the camera through plate pixel
    (px, py): plans far-off flight paths in screen space."""
    return tuple(o + distance * v for o, v in zip(CAMERA_AT, plate_ray(px, py)))


def add_plate_camera(scene):
    data = bpy.data.cameras.new("PlateCam")
    data.sensor_fit = 'HORIZONTAL'
    data.sensor_width = SENSOR_MM
    data.lens = LENS_MM
    # Blender shift is in units of the larger sensor dimension (the width);
    # a negative shift_x puts the optical axis right of the frame centre.
    data.shift_x = -(AXIS_X - PLATE_W / 2) / PLATE_W
    data.clip_start, data.clip_end = 0.1, 500.0
    cam = bpy.data.objects.new("PlateCam", data)
    scene.collection.objects.link(cam)
    cam.location = CAMERA_AT
    cam.rotation_euler = (math.pi / 2 - PITCH, 0.0, 0.0)
    scene.camera = cam
    return cam


def disc(name, radius, z, mat, collection, segments=128, hole=0.0):
    """A flat disc (or ring, with `hole`) at height z, through the data API."""
    verts, faces = [], []
    for i in range(segments):
        a = 2 * math.pi * i / segments
        verts.append((radius * math.cos(a), radius * math.sin(a), z))
        verts.append((hole * math.cos(a), hole * math.sin(a), z))
    for i in range(segments):
        j = (i + 1) % segments
        faces.append((2 * i, 2 * j, 2 * j + 1, 2 * i + 1))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def add_deck(scene):
    """The atrium's floor ring, glossy dark plating like the painting's.
    Visible in the beauty pass, holdout in the mask pass: only the change an
    actor makes to it (reflection, shadow, headlight pool) reaches the plate."""
    plating = material("FloorPlating", (0.05, 0.05, 0.055), metallic=0.6, roughness=0.3)
    return [disc("Deck", WALL_R + BAY_DEPTH, 0.0, plating, scene.collection, hole=GARDEN_R)]


def _wall_arc(name, radius, a0, a1, height, mat, collection, step=2.0):
    """An open cylindrical shell of the wall between floor_xy angles a0, a1."""
    n = max(1, round((a1 - a0) / step))
    verts = []
    for i in range(n + 1):
        x, y = floor_xy(radius, a0 + (a1 - a0) * i / n)
        verts += [(x, y, 0.0), (x, y, height)]
    faces = [(2 * i, 2 * i + 2, 2 * i + 3, 2 * i + 1) for i in range(n)]
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def add_occluders(scene):
    """Holdouts for painted things in front of an actor: the garden's
    foliage (a drum over the planters) and the wall either side of the
    cargo bay's door, so a vehicle driving in vanishes behind it. Alpha 0
    in every pass, so the painting stays untouched there."""
    hold = material("Holdout", (0.0, 0.0, 0.0))
    mesh = bpy.data.meshes.new("GardenHoldout")
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=96, radius1=GARDEN_R,
                          radius2=GARDEN_R, depth=GARDEN_HEIGHT)
    bmesh.ops.translate(bm, vec=(0.0, 0.0, GARDEN_HEIGHT / 2), verts=bm.verts)
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(hold)
    garden = bpy.data.objects.new("GardenHoldout", mesh)
    scene.collection.objects.link(garden)
    walls = [_wall_arc("BayWallL", WALL_R, BAY_DOOR[0] - 45.0, BAY_DOOR[0], 10.0, hold,
                       scene.collection),
             _wall_arc("BayWallR", WALL_R, BAY_DOOR[1], BAY_DOOR[1] + 30.0, 10.0, hold,
                       scene.collection)]
    for obj in (garden, *walls):
        obj.is_holdout = True
    return [garden, *walls]


def add_lights(scene):
    """Warm light from the shopfronts and bays around the wall, cool
    starlight from the dome, dim warm ambience."""
    world = bpy.data.worlds.new("AtriumAir")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.025, 0.022, 0.02, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world
    # The painting's floor is dim; a hot key made the yellow tug glare.
    light(scene, "DomeSky", 'AREA', (0.0, 0.0, 30.0), COOL, 2000.0, size=40.0)
    for i in range(12):                                   # shop/bay spill round the wall
        x, y = floor_xy(WALL_R - 1.0, i * 30.0)
        light(scene, f"Shop{i:02d}", 'POINT', (x, y, 3.0), WARM, 900.0, size=1.5)
    for i in range(16):                                   # the floor lamps by the garden
        x, y = floor_xy(LAMP_R, i * 22.5 + 11.25)
        light(scene, f"FloorLamp{i:02d}", 'POINT', (x, y, 0.3), (1.0, 0.9, 0.75), 60.0,
              size=0.15)

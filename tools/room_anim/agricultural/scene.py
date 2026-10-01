"""Camera-matched Blender scene for the Agricultural concourse atrium (#605).

The camera comes from camera_match.py (fitted to circles round the kiosk's
axis); everything here is placed round that axis:

* proxy decks: the bridge, its arm, the kiosk's platform and column (deck
  level) and the lower ring floor. Visible in the beauty pass, holdout in the
  mask pass, so only the change a walker makes to them (his shadow) moves
  onto the painting,
* holdouts for painted things in front of the walkers, as plate silhouettes
  on upright cards: the glass lobby's wall (and, partly, its glass), the
  cantilevered beam and the balcony's lip at the bottom left,
* dusk light like the painting's: warm lamps under the balcony, a cool sky
  through the windows behind, a soft warm fill.

Pure bpy. Imported by render_layers.py (run inside Blender).
"""
import math

import bmesh
import bpy

import camera_match as cm
from stage import (EXPOSURE_EV, PLATE_H, PLATE_W, attach_plate_reference,  # noqa: F401
                   box_object, light, material, reset, setup_render)

BRIDGE_W = 1.9              # m, the bridge and the arm (both edges measured on the plate)
DECK_THICK = 0.4
CANOPY_Z = 2.6              # the kiosk's canopy, over the column

WARM = (1.0, 0.78, 0.55)    # the alcove lamps and the kiosk's downlights
DUSK = (0.55, 0.62, 0.8)    # the sky through the windows, behind and above


def add_plate_camera(scene):
    data = bpy.data.cameras.new("PlateCam")
    data.sensor_fit = 'HORIZONTAL'
    data.sensor_width = cm.SENSOR_MM
    data.lens = cm.LENS_MM
    # Blender shift is in units of the larger sensor dimension (the width).
    data.shift_x = -(cm.AXIS_X_PX - PLATE_W / 2) / PLATE_W
    data.shift_y = (cm.HORIZON_Y - PLATE_H / 2) / PLATE_W
    data.clip_start, data.clip_end = 0.5, 500.0
    cam = bpy.data.objects.new("PlateCam", data)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, cm.EYE_HEIGHT)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)   # level, facing +Y
    scene.camera = cam
    return cam


def _cylinder(name, radius, z0, z1, mat, collection, x=0.0, y=cm.AXIS_Y):
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=48, radius1=radius, radius2=radius,
                          depth=z1 - z0)
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    obj.location = (x, y, (z0 + z1) / 2)
    collection.objects.link(obj)
    return obj


def _radial_strip(name, deg, r0, r1, mat, collection):
    """A deck strip BRIDGE_W wide along the radial line at `deg`, r0..r1."""
    x, y = cm.around_axis((r0 + r1) / 2, deg)
    obj = box_object(name, (r1 - r0, BRIDGE_W, DECK_THICK), (x, y, cm.DECK_Z - DECK_THICK / 2),
                     mat, collection)
    obj.rotation_euler = (0.0, 0.0, math.radians(deg))
    return obj


def add_decks(scene):
    """Floor proxies (and the kiosk's column, which a walker's shadow can
    cross). Grey stand-ins: the bake only takes the light a walker removes."""
    deck = material("Deck", (0.42, 0.4, 0.37), roughness=0.5)
    bridge = material("Bridge", (0.15, 0.14, 0.13), roughness=0.5)   # painted darker
    floor = material("LowerFloor", (0.07, 0.06, 0.05), roughness=0.6)
    steel = material("Column", (0.3, 0.3, 0.32), metallic=0.6, roughness=0.35)
    col = scene.collection
    return [
        _cylinder("Platform", cm.PLATFORM_R, cm.DECK_Z - DECK_THICK, cm.DECK_Z, deck, col),
        _cylinder("Column", cm.COLUMN_R, cm.DECK_Z, CANOPY_Z, steel, col),
        _radial_strip("Bridge", -148.0, cm.PLATFORM_R, 14.0, bridge, col),
        _radial_strip("Arm", 24.0, cm.PLATFORM_R, 18.0, deck, col),
        box_object("Lower", (80.0, 60.0, 0.02), (0.0, 40.0, cm.LOWER_Z - 0.01), floor, col),
    ]


# Painted things in front of the walkers, traced on the plate (px) and stood
# upright at a view depth `y` (m): anything farther is hidden exactly where
# the paint is. Upright, not camera-facing, so a walker goes behind one
# whole, never feet first.
LOBBY_Y = cm.floor_point(870, 708, cm.LOWER_Z)[1]    # the lobby's glass front, ~50.8 m
SILHOUETTES = {
    # The lit wall panel right of the glass lobby, to its foot: the lobby
    # walker starts inside, behind it.
    "LobbyWall": (LOBBY_Y, [(957, 505), (1190, 505), (1190, 712), (957, 712)]),
    # The cantilevered beam over the lower floor, with the bracket under its
    # end, and the balcony's curved lip left of it (both at the deck's depth
    # there, far in front of the floor below): the lobby walker passes under
    # the beam's end and goes behind the lip.
    "LeftBeam": (cm.floor_point(510, 945)[1], [(285, 745), (345, 745), (527, 930),
                                               (516, 952), (468, 952), (362, 920),
                                               (356, 880), (285, 815)]),
    "LeftLip": (cm.floor_point(330, 1024)[1], [(398, 850), (378, 900), (356, 950),
                                               (338, 1000), (330, 1024), (0, 1024), (0, 850)]),
}
# The lobby's glass front: a walker inside shows through it at 60%, under the
# painted reflections.
GLASS = (LOBBY_Y, [(763, 515), (957, 515), (957, 712), (763, 712)])
GLASS_HOLD = 0.4


def silhouette(name, y, polygon_px, mat, collection):
    """A flat polygon covering `polygon_px` on the plate, upright at view
    depth `y`. Seen by the camera only: it casts no shadow, blocks no light."""
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([cm.on_upright(px, py, y) for px, py in polygon_px], [],
                     [list(range(len(polygon_px)))])
    mesh.materials.append(mat)
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    obj.visible_shadow = obj.visible_diffuse = obj.visible_glossy = False
    obj.visible_transmission = obj.visible_volume_scatter = False
    return obj


def _glass_material():
    """Partly holdout: what's behind keeps (1 - GLASS_HOLD) of its alpha."""
    mat = bpy.data.materials.new("LobbyGlass")
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    mix = nodes.new("ShaderNodeMixShader")
    mix.inputs["Fac"].default_value = GLASS_HOLD
    links.new(nodes.new("ShaderNodeBsdfTransparent").outputs[0], mix.inputs[1])
    links.new(nodes.new("ShaderNodeHoldout").outputs[0], mix.inputs[2])
    links.new(mix.outputs[0], nodes.new("ShaderNodeOutputMaterial").inputs["Surface"])
    return mat


def add_occluders(scene):
    """Holdouts: alpha 0 in every pass (the glass: partly), so the bake
    leaves the painting as painted wherever they stand in front."""
    hold = material("Holdout", (0.0, 0.0, 0.0))
    occluders = [silhouette(name, y, poly, hold, scene.collection)
                 for name, (y, poly) in SILHOUETTES.items()]
    for obj in occluders:
        obj.is_holdout = True
    occluders.append(silhouette("LobbyGlass", *GLASS, _glass_material(), scene.collection))
    return occluders


def add_lights(scene):
    """Dusk: a cool sky through the windows behind, warm lamps round the
    atrium. Lights light the walkers but are never seen (camera, glossy)."""
    world = bpy.data.worlds.new("AtriumAir")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.05, 0.045, 0.04, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world
    lamps = [
        # The sky through the windows: high behind the kiosk, a cool rim.
        light(scene, "Windows", 'AREA', (0.0, cm.AXIS_Y + 45.0, 30.0), DUSK, 12000.0,
              size=40.0, rot=(math.radians(-55.0), 0.0, 0.0)),
        # Warm downlights over the bridge and kiosk (the deck is lit bright).
        light(scene, "KioskLights", 'AREA', (0.0, cm.AXIS_Y - 1.0, 7.0), WARM, 1300.0,
              size=6.0),
        # The lower ring's lit alcoves: a warm glow from its outer wall,
        # level, so it lights a walker's side more than the floor.
        light(scene, "AlcoveGlow", 'AREA', (*cm.around_axis(21.5, 135.0), cm.LOWER_Z + 2.0),
              WARM, 400.0, size=14.0, rot=(math.radians(-90.0), 0.0, math.radians(45.0))),
        # Inside the glass lobby, dimmer.
        light(scene, "LobbyLamp", 'POINT', (-5.0, LOBBY_Y + 3.0, cm.LOWER_Z + 4.0), WARM, 10.0,
              size=1.0),
    ]
    for lamp in lamps:
        lamp.visible_camera = lamp.visible_glossy = False

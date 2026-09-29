"""Camera-matched Blender scene for the New Constantinople concourse (#515).

The painted plate (assets/concourse/newcon/concourse_bg.png, 1536x1024) stays
the hero image. This module rebuilds just enough of its space in Blender to
render animated layers that sit *in* the painting:

* a camera matched to the plate (level, lens-shifted one-point perspective:
  vanishing point ~(1230, 551) px, eye height 5 m above the deck),
* a glossy proxy deck. Layers are rendered over it and then differenced
  against a render of the empty deck (see bake_layer.py), so the painted
  floor picks up the actor's reflection, glow and shadow while the deck
  itself never replaces paint,
* lights approximating the painting: dim cool steel ambience from the arches,
  warm amber spill from the promenade shopfronts.

Everything is in metres. +Y runs down the hall toward the vanishing point,
+X to the right, Z up, deck at Z=0. Landmarks were measured on the plate as
the ratio (y - 551) / (1230 - x), which equals (EYE_HEIGHT - z) / -X for a
point at height z: red guide stripe X=-21.6 (deck), kerb lamps X=-25 (deck),
walkway kerb top 0.9 m up at X=-25, shopfront base X=-31 at 0.9 m, right
platform edge X=-4.2.

Pure bpy, no add-ons. Imported by render_layers.py (run inside Blender).
"""
import math

import bpy

# Shared helpers (tools/room_anim/stage.py), re-exported under this module's
# name so callers keep using hall.reset(), hall.setup_render(), ...
from stage import (EXPOSURE_EV, PLATE_H, PLATE_W, attach_plate_reference,  # noqa: F401
                   box_object, light, material, reset, setup_render)

VANISHING_POINT = (1230.0, 551.0)   # plate pixels, measured from the stripe/kerb
EYE_HEIGHT = 5.0                    # metres above the deck
LENS_MM, SENSOR_MM = 28.0, 36.0

STRIPE_X, PLATFORM_X = -21.6, -4.2
# Raised promenade walkway between the kerb and the shopfronts.
KERB_X, SHOPFRONT_X, PROMENADE_Z = -25.0, -31.0, 0.9

AMBER = (1.0, 0.58, 0.24)
COOL_STEEL = (0.55, 0.68, 1.0)


def add_plate_camera(scene):
    data = bpy.data.cameras.new("PlateCam")
    data.sensor_fit = 'HORIZONTAL'
    data.sensor_width = SENSOR_MM
    data.lens = LENS_MM
    # Blender shift is in units of the larger sensor dimension (the width).
    data.shift_x = -(VANISHING_POINT[0] - PLATE_W / 2) / PLATE_W
    data.shift_y = (VANISHING_POINT[1] - PLATE_H / 2) / PLATE_W
    cam = bpy.data.objects.new("PlateCam", data)
    scene.collection.objects.link(cam)
    cam.location = (0.0, 0.0, EYE_HEIGHT)
    cam.rotation_euler = (math.radians(90.0), 0.0, 0.0)   # level, facing +Y
    scene.camera = cam
    return cam


def add_deck(scene):
    """Floor proxies: the wet lane deck and the raised promenade walkway.
    Visible in the beauty pass, holdout in the mask pass. Their absolute look
    is irrelevant: only the change an actor makes to them is transferred onto
    the painting."""
    wet = material("WetDeck", (0.05, 0.055, 0.06), metallic=0.6, roughness=0.14)
    walk = material("Walkway", (0.06, 0.06, 0.065), metallic=0.4, roughness=0.45)
    lane = box_object("Deck", (60.0, 260.0, 0.02), (-18.0, 120.0, -0.01), wet,
                      scene.collection)
    width = 12.0          # runs back under the shopfronts; only the front 6 m show
    promenade = box_object("Promenade", (width, 260.0, PROMENADE_Z),
                           (KERB_X - width / 2, 120.0, PROMENADE_Z / 2), walk,
                           scene.collection)
    return [lane, promenade]


def add_occluders(scene):
    """Holdouts for painted things that stand in front of the actors: alpha 0
    in every pass, so the bake leaves the painting untouched there."""
    hold = material("Holdout", (0.0, 0.0, 0.0))
    # Foreground cargo cart, bottom-left (plate x 0-262, y 772-985): wheels
    # on the deck at ~14.4 m, right edge X -11.7 m, crates ~2.3 m tall. A thin
    # slab at that depth: a deeper box would hold out real lane floor behind.
    cart = box_object("CartHoldout", (8.2, 1.0, 2.3), (-15.9, 14.4, 1.15), hold,
                      scene.collection)
    # Kiosk pod on the walkway (plate x 575-690, base y ~625): ~66 m deep,
    # protruding to X -29.9 from the shopfront line. Walkers hugging the
    # shopfronts (X < -29.9) pass behind it.
    kiosk = box_object("KioskHoldout", (6.4, 4.0, 8.0), (-33.1, 68.0, PROMENADE_Z + 4.0),
                       hold, scene.collection)
    for obj in (cart, kiosk):
        obj.is_holdout = True
    return [cart, kiosk]


def add_lights(scene):
    world = bpy.data.worlds.new("HallAir")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.018, 0.024, 0.04, 1.0)
    bg.inputs["Strength"].default_value = 1.0
    scene.world = world

    # Cool skylight spill through the arched ribs, high and to the right.
    light(scene, "ArchSpill", 'AREA', (10.0, 80.0, 40.0), COOL_STEEL, 15000.0, size=60.0,
          rot=(math.radians(20.0), math.radians(-25.0), 0.0))
    # Warm window spill from the shopfronts onto the walkway and deck.
    for i, y in enumerate(range(18, 200, 16)):
        light(scene, f"Shopfront{i:02d}", 'POINT', (SHOPFRONT_X + 0.5, float(y), 3.0), AMBER,
              1500.0, size=2.0)
    # Small deck lamps along the right platform lip (the white dots).
    for i, y in enumerate(range(14, 120, 12)):
        light(scene, f"PlatformLamp{i:02d}", 'POINT', (PLATFORM_X + 0.3, float(y), 0.4),
              (1.0, 0.9, 0.75), 150.0, size=0.2)

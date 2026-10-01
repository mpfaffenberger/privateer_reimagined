"""The mining concourse's ore train (#558): a yellow tug towing an ore hopper,
the same beat as the original game's mining concourse (its legacy
`concourse_car` overlay), rebuilt from the original base-vehicle meshes.

    tug    ships_wcnews/truck.obj  yellow tow tug (../vehicles.py, shared)
    hopper ships_wcnews/cart.obj   grey ore hopper, heaped with ore here
"""
import random

import bmesh
import bpy

import ships
from stage import animate_straight_pass, material
from vehicles import sweep_beacon, tow_tug, trail

HOPPER_LENGTH = 3.4                         # m
HEADLIGHT = (1.0, 0.86, 0.66)
ORE_ROCK = (0.34, 0.17, 0.07)               # the painting's orange rock, in shadow
HOPPER_GRIME = (0.38, 0.36, 0.34)           # the stock hopper is near-white; mine dirt
TUG_GRIME = (0.8, 0.76, 0.7)                # keep the yellow, knock off the showroom shine


def _ore_pile(parent, size, seed=558):
    """A heaped, lumpy mound filling the hopper's top: a squashed icosphere
    with its vertices pushed around, flat underneath (sunk into the bin)."""
    width, length, height = size
    mesh = bpy.data.meshes.new("Ore")
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=3, radius=1.0)
    rng = random.Random(seed)
    for v in bm.verts:
        v.co *= 1.0 + rng.uniform(-0.12, 0.12)
        v.co.z = max(v.co.z, -0.1)
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(material("Ore", ORE_ROCK, roughness=0.9))
    obj = bpy.data.objects.new("Ore", mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.parent = parent
    obj.scale = (width * 0.42, length * 0.40, height * 0.28)
    obj.location = (0.0, 0.0, height * 0.92)
    return obj


def build_ore_train(tug_tint=TUG_GRIME, hopper_tint=HOPPER_GRIME):
    """-> (tug root, hopper root, beacon). Both face +Y (nose), origin on the
    floor. The tints multiply the stock textures (grime for the lighting)."""
    tug, beacon = tow_tug(tug_tint, HEADLIGHT)
    hopper = ships.import_ship("cart", HOPPER_LENGTH, "Hopper", grounded=True,
                               tint=hopper_tint)
    _ore_pile(hopper, hopper["size"])
    return tug, hopper, beacon


def animate_train(tug, hopper, beacon, x, y_start, y_end, frame_start, frame_end,
                  heading_deg, beacon_rpm=40.0, fps=24):
    """Drive the train down a straight line; the hopper trails the tug by the
    coupling, and the beacon's spot sweeps round at `beacon_rpm`."""
    gap = trail(HOPPER_LENGTH)
    back = -gap if y_end > y_start else gap            # behind = against the travel
    rumble = 0.008                                     # m; ground vehicles jiggle, not bob
    animate_straight_pass(tug, x, y_start, y_end, 0.0, frame_start, frame_end,
                          bob=rumble, heading_deg=heading_deg)
    animate_straight_pass(hopper, x, y_start + back, y_end + back, 0.0, frame_start,
                          frame_end, bob=rumble, heading_deg=heading_deg)
    sweep_beacon(beacon, frame_start, frame_end, fps, beacon_rpm)

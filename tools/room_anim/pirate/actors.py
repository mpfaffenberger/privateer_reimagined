"""Actors for the pirate concourse (#586): a cargo pod hovering in from the
far hall, and pirates crossing between the far hall and the side bay.

    pod      ships_wcnews/trailer.obj  grey cargo pod with red dome lamps,
             grimed, hovering on a blue underglow
    pirates  walkers.build_walker() proxies in pirate colours
"""
import math

import bpy
from mathutils import Vector

import ships
from stage import emitter, light
from walkers import build_walker

POD_LENGTH = 4.2                    # m
POD_GRIME = (0.3, 0.27, 0.24)        # dull the showroom grey; this is a smugglers' den
HEADLAMP = (1.0, 0.88, 0.7)
UNDERGLOW = (0.35, 0.65, 1.0)       # hover-field blue, like the painting's cold lamps
BEACON_RED = (1.0, 0.1, 0.04)

# (coat, trousers, skin, head) per pirate
PIRATES = {
    "bandana": ((0.13, 0.075, 0.045), (0.05, 0.045, 0.04), (0.42, 0.29, 0.21),
                (0.4, 0.035, 0.03)),
    "longcoat": ((0.06, 0.07, 0.065), (0.08, 0.06, 0.045), (0.3, 0.2, 0.15), None),
}


def blink(strobe, frames, period_frames, strength=25.0):
    """Key an emitter to flash for a sixth of every `period_frames`."""
    socket = (strobe.data.materials[0].node_tree.nodes["Principled BSDF"]
              .inputs["Emission Strength"])
    first, last = frames
    for f in range(first, last + 1):
        socket.default_value = strength if (f - first) % period_frames < period_frames / 6 else 0.0
        socket.keyframe_insert("default_value", frame=f)


def build_pod():
    """-> (root empty (+Y nose, origin under the hull), strobe): the grav pod
    with headlamps, a blue hover glow pooling on the deck and a red strobe
    (key it with blink())."""
    pod = ships.import_ship("trailer", POD_LENGTH, "Pod", grounded=True, tint=POD_GRIME)
    w, length, h = pod["size"]
    nose = Vector((0.0, length / 2, h * 0.4))
    for side in (-1, 1):
        lamp = nose + Vector((side * w * 0.12, 0.0, 0.0))
        emitter(f"PodLamp{side}", pod, lamp, 0.09, HEADLAMP, 5.0)
        beam = light(bpy.context.scene, f"PodBeam{side}", 'SPOT', (0.0, 0.0, 0.0), HEADLAMP,
                     40.0, size=0.08)
        beam.data.spot_size, beam.data.spot_blend = math.radians(55.0), 0.5
        # Spots shine down local -Z; +78 deg about X aims them ahead, onto the deck.
        beam.parent, beam.location = pod, lamp + Vector((0.0, 0.1, 0.0))
        beam.rotation_euler = (math.radians(78.0), 0.0, 0.0)
    glow = light(bpy.context.scene, "PodHover", 'AREA', (0.0, 0.0, 0.0), UNDERGLOW, 40.0,
                 size=2.0)
    glow.parent, glow.location = pod, (0.0, 0.0, -0.05)       # faces down (-Z)
    emitter("PodField", pod, (0.0, 0.0, 0.02), 0.25, UNDERGLOW, 1.5).scale = (2.4, 7.0, 0.15)
    strobe = emitter("PodStrobe", pod, Vector((0.0, 0.0, h * 1.02)), 0.07, BEACON_RED, 0.0)
    return pod, strobe


def build_pirate(name, look):
    """-> (root, limbs): a pirate proxy in one of the PIRATES looks."""
    coat, trousers, skin, head = PIRATES[look]
    return build_walker(name, coat, trousers, skin_rgb=skin, head_rgb=head)

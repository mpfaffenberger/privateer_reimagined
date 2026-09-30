"""The Oxford concourse's traffic (#592): the original game's air-car
(ships_wcnews/aircar.obj, the bubble-canopy runabout) gliding down the
university town's streets, lamps lit.

At 50-70 px on screen its stock textures (low-res gold on everything) smear
into an orange blob, so each material slot gets a clean shader instead: the
big canopy dome and body in one glossy paint, gunmetal chassis, and the two
lobes at the tail as its tail lights. From 34 deg above the dome is most of
the car; as dark glass it rendered as a black hole in the hull, as clear
glass it showed an unlit, muddy cockpit. Glossy paint catches the street
lamps as it passes under them.
"""
import math

from mathutils import Vector

import camera_match as cm
import ships
from stage import emitter, light, material

CAR_LENGTH = 4.0                    # m; ~1.3x the painted lamp posts
HEADLIGHT = (1.0, 0.88, 0.7)
TAIL_RED = (1.0, 0.12, 0.04)
HOVER_CYAN = (0.45, 0.85, 1.0)      # the painting's cyan accent lights
OXBLOOD = (0.16, 0.028, 0.024)      # the tents' red, a shade deeper
RACING_GREEN = (0.02, 0.075, 0.04)
PAINTS = {"oxblood": OXBLOOD, "racing_green": RACING_GREEN}   # routes.json "paint"


def _slot_materials(name, paint):
    """aircar.obj material slot -> clean shader. Slots not listed are
    chassis."""
    chassis = material(f"{name}Chassis", (0.035, 0.035, 0.04), metallic=0.8, roughness=0.35)
    paint = material(f"{name}Paint", paint, metallic=0.3, roughness=0.22)
    return chassis, {
        "BODYTEX": paint,
        "AWINDSHIELD": paint,
        "GLOW_CENTER": material(f"{name}TailLamp", (0.25, 0.02, 0.01), roughness=0.3,
                                emission=TAIL_RED, strength=0.5),
        "GLOW_EDGE": material(f"{name}EdgeGlow", (0.0, 0.0, 0.0), emission=HOVER_CYAN,
                              strength=4.0),
    }


def _spot(name, parent, loc, rgb, energy, angle_deg, tilt_deg):
    """A spot on `parent` shining forward (+Y), `tilt_deg` down to the street."""
    obj = light(parent.users_scene[0], name, 'SPOT', loc, rgb, energy, size=0.08,
                rot=(math.radians(90.0 - tilt_deg), 0.0, 0.0))
    obj.data.spot_size, obj.data.spot_blend = math.radians(angle_deg), 0.5
    obj.parent = parent
    return obj


def build_aircar(name, paint=OXBLOOD):
    """-> root empty, nose +Y, origin under the hull (grounded)."""
    car = ships.import_ship("aircar", CAR_LENGTH, name, grounded=True)
    chassis, by_slot = _slot_materials(name, paint)
    for part in car.children:
        for slot in part.material_slots:
            slot.material = by_slot.get(slot.material.name.split(":")[-1], chassis)
    w, length, h = car["size"]
    for side in (-1, 1):
        head = Vector((side * w * 0.28, length * 0.47, h * 0.35))
        emitter(f"{name}Head{side}", car, head, 0.08, HEADLIGHT, 12.0)
        # Short, steep beams: light spilled far down the street makes every
        # frame a big sprite, which bake_layer stores at half size (soft).
        _spot(f"{name}Beam{side}", car, head + Vector((0, 0.15, 0)), HEADLIGHT, 25.0, 40.0, 30.0)
    # A soft cyan pool on the street under the hull: at this size that light,
    # not the hull, is what says "hovering".
    pool = light(car.users_scene[0], f"{name}Pool", 'AREA', (0.0, 0.0, -0.1), HOVER_CYAN,
                 50.0, size=1.6)
    pool.parent = car
    return car


def animate_drive(root, start_uv, end_uv, hover, speed, fps, bob=0.04):
    """Constant speed from `start_uv` to `end_uv` (street frame, m) at
    `hover` m, from frame 1, with a slow hover bob. Returns the last frame."""
    root.rotation_euler = (0.0, 0.0, math.radians(cm.heading_deg(*start_uv, *end_uv)))
    (x0, y0), (x1, y1) = cm.to_world(*start_uv), cm.to_world(*end_uv)
    last = 1 + round(math.hypot(x1 - x0, y1 - y0) / speed * fps)
    for f in range(1, last + 1):
        t = (f - 1) / (last - 1)
        root.location = (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t,
                         hover + bob * math.sin(2.0 * math.pi * 0.4 * (f - 1) / fps))
        root.keyframe_insert("location", frame=f)
    return last

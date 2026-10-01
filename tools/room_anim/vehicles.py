"""The yellow tow tug the floor trains hitch to (#603; built for the mining ore
train, #558, then the military munitions train, #588). Runs inside Blender.

    tug  ships_wcnews/truck.obj  twin headlamps, an amber beacon on the roof

Each base brings its own trailer, grime and path; this module owns the tug,
the hitch distance and the beacon's sweep.
"""
import math

from mathutils import Vector

import ships
from stage import emitter, spot

TUG_LENGTH = 3.0                    # m
COUPLING_GAP = 0.5                  # m between the tug and its trailer
BEACON_AMBER = (1.0, 0.45, 0.08)
BEACON_TILT = math.radians(60.0)    # off straight down: the beam rakes the floor


def tow_tug(tint, headlight):
    """-> (tug root, beacon). Nose +Y, origin on the floor; `tint` multiplies
    the stock texture (grime for the room's lighting)."""
    tug = ships.import_ship("truck", TUG_LENGTH, "Tug", grounded=True, tint=tint)
    w, length, h = tug["size"]
    nose = Vector((0.0, length / 2, h * 0.45))
    for side in (-1, 1):
        lamp = nose + Vector((side * w * 0.3, 0.0, 0.0))
        emitter(f"Headlamp{side}", tug, lamp, 0.07, headlight, 6.0)
        # +80 deg about X aims the spots forward (+Y) and 10 deg down onto
        # the floor ahead.
        spot(f"Headlight{side}", tug, lamp + Vector((0, 0.1, 0)),
             (math.radians(80.0), 0.0, 0.0), headlight, 30.0, 50.0)
    beacon_at = Vector((0.0, -length * 0.1, h * 1.05))
    emitter("BeaconGlass", tug, beacon_at, 0.09, BEACON_AMBER, 5.0)
    beacon = spot("Beacon", tug, beacon_at, (BEACON_TILT, 0.0, 0.0),
                  BEACON_AMBER, 60.0, 40.0, blend=0.6)
    return tug, beacon


def trail(trailer_length):
    """m from the tug's centre back to its trailer's, coupled."""
    return (TUG_LENGTH + trailer_length) / 2 + COUPLING_GAP


def sweep_beacon(beacon, frame_start, frame_end, fps, rpm=40.0):
    """Key the beacon's beam round its post at `rpm`, every frame."""
    for f in range(frame_start, frame_end + 1):
        turn = 2 * math.pi * rpm / 60.0 * (f - frame_start) / fps
        beacon.rotation_euler = (BEACON_TILT, 0.0, turn)
        beacon.keyframe_insert("rotation_euler", frame=f)

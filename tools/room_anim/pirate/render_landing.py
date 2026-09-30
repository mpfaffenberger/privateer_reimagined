"""Render a pair of pirate Talons buzzing the pirate landing pad (#587).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/pirate/render_landing.py -- --frames 1:132:12 --samples 8
    ... --                                  # the full pass

The pad is the same crater as the mining base's, so it shares that sky
machinery (../flyover.py, ../bake_crater.py). Where the mining pad gets a
slow Galaxy freighter, the pirates get their own fighters: two Talons
(ships_wcnews/talon5.obj) in echelon, low and fast, in from the left edge
and out past the right, closing on the camera so they grow as they climb.
The leader's centre goes from rim - 28 to rim - 48 px and the wingman
flies ~14 px lower, which keeps both inside even the tightest pirate skies
(gothri and kamekh, rim at 84 px).
"""
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import flyover  # noqa: E402
import ships  # noqa: E402
import stage  # noqa: E402

LAYER = "talon_raid"
LENGTH = 24.0                       # m; ~100 px long on screen
ALTITUDE = 100.0                    # m above the camera
RISE = (-28.0, -48.0)               # leader's centre relative to the rim, px: start, end
SCREEN_X = (-140.0, 1700.0)         # off the left edge -> off the right edge
PASS_SECONDS = 5.5
# Wingman: back, out to port (away from the camera, so ~14 px lower on screen)
# and a touch higher, in the leader's frame (m).
WING_OFFSET = (-18.0, -32.0, 4.0)
BANK_DEG = 10.0                     # rolled a little into an easy right-hand drift
ENGINE = (1.0, 0.42, 0.12)          # the Talon's hot orange exhaust


def _talon(name, frames, strobe_phase):
    ship = ships.import_ship("talon5", LENGTH, name)
    w, length, h = ship["size"]
    for side in (-1, 1):
        stage.emitter(f"{name}Engine{side}", ship, (side * w * 0.1, -length / 2, 0.0), 0.7,
                      ENGINE, 40.0)
    flyover.nav_lights(ship, frames, 0.4, 30.0, 0.5, 60.0, strobe_every_s=1.1,
                       strobe_phase=strobe_phase)
    return ship


def _raid(frames, rim_y):
    lead = _talon("TalonLead", frames, 0)
    wing = _talon("TalonWing", frames, 9)
    start, end = flyover.screen_path(rim_y, ALTITUDE, RISE, SCREEN_X)
    flyover.fly_straight(lead, start, end, ALTITUDE, frames)
    heading = lead.rotation_euler.z
    ox, oy, oz = WING_OFFSET              # leader frame -> world, about Z
    dx = ox * math.cos(heading) - oy * math.sin(heading)
    dy = ox * math.sin(heading) + oy * math.cos(heading)
    flyover.fly_straight(wing, (start[0] + dx, start[1] + dy), (end[0] + dx, end[1] + dy),
                         ALTITUDE + oz, frames)
    for ship in (lead, wing):
        ship.rotation_euler.y = math.radians(BANK_DEG)     # roll about the nose (+Y)
    return [lead, wing]


if __name__ == "__main__":
    flyover.run(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [], "pirate",
                LAYER, PASS_SECONDS, _raid, __doc__.splitlines()[0])

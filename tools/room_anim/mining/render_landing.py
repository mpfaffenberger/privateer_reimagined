"""Render the Galaxy freighter passing over the mining landing pad (#561).

Run inside Blender (headless):
    blender --background --factory-startup \\
        --python tools/room_anim/mining/render_landing.py -- --frames 1:384:48 --samples 8
    ... --                                  # the full pass

Shared crater-sky machinery (canonical camera, sun, rim-relative paths, the
render loop) is in ../flyover.py. The freighter's path is solved from screen
targets relative to the rim: its centre rises from rim + 10 (emerging from
behind the rim, off the right edge) to rim - 30 (off the left edge). At a
constant ALTITUDE that means closing on the camera, so it also grows ~10% as
it climbs: the original game's beat (the legacy landing_shp overlay). The
climb stays shallow so the ~50 px ship still clears the top edge on the
tightest skies.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import flyover  # noqa: E402
import ships  # noqa: E402

LAYER = "galaxy_flyover"
LENGTH = 100.0                      # m; ~110 px long, ~50 px tall on screen
ALTITUDE = 400.0                    # m above the camera
RISE = (10.0, -30.0)                # ship centre relative to the rim, px: start, end
SCREEN_X = (1616.0, -80.0)          # off the right edge -> off the left edge
PASS_SECONDS = 16.0


def _freighter(frames, rim_y):
    ship = ships.import_ship("mrchship", LENGTH, "Galaxy")
    flyover.nav_lights(ship, frames, 1.3, 30.0, 1.5, 60.0)
    start, end = flyover.screen_path(rim_y, ALTITUDE, RISE, SCREEN_X)
    flyover.fly_straight(ship, start, end, ALTITUDE, frames)
    return [ship]


if __name__ == "__main__":
    flyover.run(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [], "mining",
                LAYER, PASS_SECONDS, _freighter, __doc__.splitlines()[0])

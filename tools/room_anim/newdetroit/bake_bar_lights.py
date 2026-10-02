#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""The New Detroit bar's lights (#676), flicker.py on bar_bg.png: its glass
lanterns flicker like the pirate concourse's (#586), one layer each on its
own seed so no two flicker in step, and the red beacons on the towers out
the windows blink. The ceiling domes are electric and stay steady.

    uv run tools/room_anim/newdetroit/bake_bar_lights.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import flicker  # noqa: E402
from bake_layer import load_rgba  # noqa: E402
from base import paths  # noqa: E402

# name -> [(centre x, centre y, glow sigma px)], seed. Plate pixels, the
# lanterns' bright glass measured on bar_bg.png: on the back ledges (left,
# middle, right), the sconce left of the left one, the one on the
# right-hand wall pillar, and the one on the middle booth's partition.
LANTERNS = {
    "lantern_left": ([(310.0, 455.0, 14.0)], 6761),
    "lantern_sconce": ([(260.0, 454.0, 8.0)], 6762),
    "lantern_middle": ([(775.0, 437.0, 14.0)], 6763),
    "lantern_right": ([(1048.0, 442.0, 12.0)], 6764),
    "lantern_wall": ([(1160.0, 406.0, 13.0)], 6765),
    "lantern_booth": ([(750.0, 553.0, 15.0)], 6766),
}

# name -> lights, (period s, lit s, dimmed level). The red dots out the
# windows (their centres of mass): four up the tower's corner, blinking
# together, and two on the mast far left, on a period of their own so the
# two never fall into step. Their cores are clipped white-hot in the
# painting, so "off" has to go deep (0.08) to read as off.
BEACONS = {
    "beacon_tower": ([(382.0, 75.4, 6.0), (382.1, 103.1, 6.0), (383.8, 115.7, 6.0),
                      (382.5, 129.6, 6.0)], (2.0, 1.0, 0.08)),
    "beacon_mast": ([(90.7, 80.0, 6.0), (94.0, 92.5, 5.0)], (3.0, 0.6, 0.08)),
}


def main():
    where = paths("newdetroit")
    plate = load_rgba(where.room / "bar_bg.png")[..., :3]
    out = where.anim / "bar"
    for name, (lamps, seed) in LANTERNS.items():
        flicker.bake(out, name, lamps, flicker.flame(seed), plate)
    for name, (lamps, (period, lit, low)) in BEACONS.items():
        flicker.bake(out, name, lamps, flicker.blink(period, lit, low), plate)


if __name__ == "__main__":
    main()

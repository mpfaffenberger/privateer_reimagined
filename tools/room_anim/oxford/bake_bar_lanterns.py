#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""The Oxford bar's lanterns (#677), flicker.py on bar_bg.png: its four oil
lanterns flicker like the pirate concourse's (#586), one layer each on its
own seed so no two flicker in step. The electric light (chandelier,
sconces, the bottle shelves) holds steady.

    uv run tools/room_anim/oxford/bake_bar_lanterns.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import flicker  # noqa: E402
from bake_layer import load_rgba  # noqa: E402
from base import paths  # noqa: E402

# name -> [(centre x, centre y, glow sigma px), ...], seed. Plate pixels:
# each lantern's glass and the pool it throws on its tabletop (or the floor),
# found as the warm, near-white cores below the chandelier.
LANTERNS = {
    "lantern_middle": ([(690.0, 536.0, 12.0), (691.0, 572.0, 20.0)], 677),
    "lantern_left": ([(391.0, 509.0, 9.0), (393.0, 534.0, 13.0)], 6770),
    "lantern_back": ([(628.0, 468.0, 8.0)], 67700),
    "lantern_floor": ([(325.0, 648.0, 10.0), (324.0, 679.0, 10.0)], 677000),
}


def main():
    where = paths("oxford")
    plate = load_rgba(where.room / "bar_bg.png")[..., :3]
    for name, (lamps, seed) in LANTERNS.items():
        flicker.bake(where.anim / "bar", name, lamps, flicker.flame(seed), plate)


if __name__ == "__main__":
    main()

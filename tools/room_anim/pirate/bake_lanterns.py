#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Flickering lanterns for the pirate concourse (#586): the flicker itself is
flicker.py's, shared with the New Detroit bar (#676).

    uv run tools/room_anim/pirate/bake_lanterns.py
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import flicker  # noqa: E402
from bake_layer import load_rgba  # noqa: E402
from base import paths  # noqa: E402

# name -> [(centre x, centre y, glow sigma px), ...], seed. Plate pixels,
# measured on the painting: the cage lantern hanging top left, and the
# two small lamps further down the left wall.
LANTERNS = {
    "lantern_hanging": ([(288.0, 230.0, 40.0)], 586),
    "lanterns_wall": ([(373.0, 376.0, 14.0), (402.0, 392.0, 12.0)], 5860),
}


def main():
    where = paths("pirate")
    plate = load_rgba(where.plate)[..., :3]
    for name, (lamps, seed) in LANTERNS.items():
        flicker.bake(where.anim, name, lamps, flicker.flame(seed), plate)


if __name__ == "__main__":
    main()

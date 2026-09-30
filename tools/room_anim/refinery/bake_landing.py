#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Find the open hangar door in every Refinery landing composite (#585).

The landing pad is a hangar whose far wall is one big door under a truss
gantry, open onto space: 18 per-hull composites
(assets/concourse/refinery/landing_ships/<hull>.png), framed differently.
The door takes the place of a crater's sky, so this is bake_crater.py (#587)
with its own finder, door_sky(). For every composite it writes to
assets/concourse/refinery/anim/landing/:

    <hull>_mask.png   L8, 255 = open space through the door
    <hull>_fill.png   its colour (small, drawn stretched)
    anchors.json      {"<hull>": [cx, cy, r]}: the door's centre x, the truss's
                      underside y and the door's half width (see find_door())
    stars_far.png, stars_near.png
                      star tiles shared by every hull: the door shows
                      near-black space, so these are a sparse addition, like
                      the legacy `lst` overlay's drifting points of light
    <layer>.{json,png}
                      ships passing outside (render_landing.py), under the
                      plate and anchored, so the gantry and the walls occlude
                      them on every hull

Usage (from the repo root):
    uv run tools/room_anim/refinery/bake_landing.py [--debug build/room_anim/refinery/doors.png]
    uv run tools/room_anim/refinery/bake_landing.py --stars-only
    uv run tools/room_anim/refinery/bake_landing.py --layers-only   # after render_landing.py
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bake_crater  # noqa: E402
from sky import star_removed  # noqa: E402

# Finding the door: a row of five lamps hangs under the truss's bottom chord,
# evenly spaced across the door, in every composite. They are the brightest
# pinpoints in the upper middle (LAMP_BOX), LAMP_LEVEL over their
# surroundings; the row is the y shared by the most of them (within LAMP_ROW
# px), and the lamps sit on a regular grid, ~LAMP_PITCH px apart. An end lamp
# can be lost in glare; the five-lamp grid is then placed so the door's
# centre is nearest DOOR_CX, where it is on most composites.
LAMP_BOX = (250, 100, 1300, 340)         # x0, y0, x1, y1 in plate px
LAMP_LEVEL = 60
LAMP_ROW = 5
LAMP_PITCH = 175.0
PITCH_RANGE = (140.0, 210.0)
GRID_TOLERANCE = 10                      # px off the grid a lamp may be
DOOR_CX = 810.0
# The sky starts this far (in door half-widths) below the lamps, clear of
# their glow and the chord, and is searched this wide: the end lamps are
# ~0.05 r inside the truss's side beams, and the rim scan stops at a lit
# beam by itself (its opening drops the lattice's narrow gaps).
SKY_BELOW_LAMPS = 0.01
SKY_HALF_WIDTH = 1.12
# Painted pinpoints of light in the door (distant platforms) stay painted:
# anything this far over the star-removed level is cut out of the mask.
DOT_LEVEL = 10


def find_lamps(plate):
    """-> (row y, [lamp x, ...]) of the lamps under the truss."""
    grey = plate.convert("L")
    residual = (np.asarray(grey, dtype=np.int16) -
                np.asarray(star_removed(grey), dtype=np.int16))
    x0, y0, x1, y1 = LAMP_BOX
    box = np.zeros_like(residual)
    box[y0:y1, x0:x1] = residual[y0:y1, x0:x1]
    peaks = Image.fromarray(box.clip(0, 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(15))
    ys, xs = np.nonzero((box >= np.asarray(peaks, dtype=np.int16)) & (box > LAMP_LEVEL))
    row = max(ys, key=lambda y: np.sum(np.abs(ys - y) <= LAMP_ROW))
    on_row = sorted(set(int(x) for x, y in zip(xs, ys) if abs(y - row) <= LAMP_ROW))
    lamps = [x for i, x in enumerate(on_row) if i == 0 or x - on_row[i - 1] > 30]   # dedupe
    return float(np.median(ys[np.abs(ys - row) <= LAMP_ROW])), lamps


def _grid(lamps):
    """-> (origin x, pitch, slots): the largest subset of `lamps` on a regular
    grid (pitch PITCH_RANGE, at most five slots): side-column lights on the
    same row fall off it."""
    best = None
    for i, a in enumerate(lamps):
        for b in lamps[i + 1:]:
            for n in range(1, 5):
                pitch = (b - a) / n
                if not PITCH_RANGE[0] <= pitch <= PITCH_RANGE[1]:
                    continue
                on = [(x, round((x - a) / pitch)) for x in lamps
                      if abs(x - a - round((x - a) / pitch) * pitch) <= GRID_TOLERANCE]
                slots = [s for _, s in on]
                if max(slots) - min(slots) > 4:
                    continue
                fit = np.polyfit([s for _, s in on], [x for x, _ in on], 1)
                score = (len(on), -abs(fit[0] - LAMP_PITCH))
                if best is None or score > best[0]:
                    best = (score, fit[1], fit[0], slots)
    if best is None or best[0][0] < 3:
        raise ValueError(f"no lamp row under the truss (found {lamps})")
    return best[1], best[2], best[3]


def find_door(plate):
    """-> [cx, cy, r]: the middle lamp's x, the lamp row's y and two lamp
    pitches (half the door's width). Fits the lamps to a five-lamp grid."""
    y, lamps = find_lamps(plate)
    origin, pitch, slots = _grid(lamps)
    # Which slot the grid's first lamp is, when end lamps are lost: the one
    # that puts the door's centre nearest DOOR_CX.
    first = min(range(max(slots) - 4, min(slots) + 1),
                key=lambda k: abs(origin + (k + 2) * pitch - DOOR_CX))
    return [round(float(origin + (first + 2) * pitch), 1), round(float(y), 1),
            round(float(2 * pitch), 1)]


def door_sky(plate):
    """-> (mask L image, [cx, cy, r]). Between the end lamps, sky runs down
    from just under the truss to the first sustained bright run (the pad's
    edge, a big hull, the lit city): bake_crater's rim scan on the door's crop."""
    anchor = find_door(plate)
    cx, cy, r = anchor
    left, right = round(cx - SKY_HALF_WIDTH * r), round(cx + SKY_HALF_WIDTH * r)
    top = round(cy + SKY_BELOW_LAMPS * r)
    crop = plate.crop((left, top, right + 1, plate.height))
    sub, _ = bake_crater.find_sky(crop)
    mask = Image.new("L", plate.size, 0)
    mask.paste(sub, (left, top))
    grey = plate.convert("L")
    dots = (np.asarray(grey, dtype=np.int16) -
            np.asarray(star_removed(grey), dtype=np.int16)) >= DOT_LEVEL
    keep = Image.fromarray(dots.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(7))
    keep = np.asarray(keep.filter(ImageFilter.GaussianBlur(1.2)), dtype=np.float32) / 255.0
    m = np.asarray(mask, dtype=np.float32) * (1.0 - keep)
    return Image.fromarray((m + 0.5).astype(np.uint8)), anchor


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="contact sheet of every detected door")
    only = ap.add_mutually_exclusive_group()
    only.add_argument("--stars-only", action="store_true", help="just re-make the star tiles")
    only.add_argument("--layers-only", action="store_true", help="just bake the ship layers")
    args = ap.parse_args()
    where = bake_crater.landing_paths("refinery")
    if not (args.stars_only or args.layers_only):
        bake_crater.bake_skies(where, args.debug, find=door_sky)
    if not args.layers_only:
        bake_crater.bake_stars(where)
    if not args.stars_only:
        bake_crater.bake_layers(where)


if __name__ == "__main__":
    main()

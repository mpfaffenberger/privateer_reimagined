#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Cut the Refinery concourse's sky out of the plate and build drifting stars (#584).

The dome's glass shows space through three windows: left of the big near
arch (the sun and its glare), between the two arches (painted asteroids and a
hauler), and right of the far arch, over the refinery's towers. The sky is
flat 2D behind a painted cutout, so this is image processing, as in New Con.

Outputs (assets/concourse/refinery/anim/):
    sky_mask.png, sky_fill.png, stars_far.png, stars_near.png
    (see newcon/bake_sky.py; the stars come from the painting's own)
and, after refinery/render_sky.py, the ships crossing the sky:
    <layer>.{png,json}   under the plate, so arches and towers occlude them

Usage (from the repo root):
    uv run tools/room_anim/refinery/bake_sky.py [--debug build/room_anim/refinery/sky_debug.png]
    uv run tools/room_anim/refinery/bake_sky.py --layers-only     # after render_sky.py
"""
import argparse
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from bake_layer import bake_passes  # noqa: E402
from sky import (RNG_SEED, PaintedStars, hazy_sky_fill, star_removed, star_tile,  # noqa: E402
                 window_mask)

REFINERY = paths("refinery")
SKY_TIMING = REFINERY.tools / "sky_layers.json"

# Coarse windows in plate px, each with the star-removed luminance at or
# below which a pixel is sky. The haze round the sun runs up to ~70, so the
# near windows are loose; the refinery's dim towers need a tight one. Where
# the glare is brighter still the painting stays: glare hides stars anyway.
WINDOWS = [
    # Left of the near arch: the sun's window, above the dome's lower frame.
    ([(0, 0), (172, 0), (150, 65), (125, 130), (100, 200), (86, 244), (0, 236)], 70),
    # Between the arches, down to the sill.
    ([(398, 0), (624, 0), (614, 60), (604, 140), (597, 210), (594, 318), (430, 345),
      (340, 372), (196, 305), (226, 225), (271, 150), (331, 75)], 70),
    # Right of the far arch, over the refinery (the threshold finds the skyline).
    ([(738, 0), (1536, 0), (1536, 200), (682, 200), (700, 100)], 20),
]
# Painted things in the sky that are dark enough to pass for it.
def _octagon(cx, cy, rx, ry):
    return [(cx + rx * math.cos(a), cy + ry * math.sin(a))
            for a in (math.pi * (k + 0.5) / 4 for k in range(8))]


HOLES = [
    _octagon(326, 263, 22, 20),      # big asteroid
    _octagon(240, 290, 13, 13),      # small asteroid
    _octagon(270, 322, 12, 11),      # pebble
    _octagon(390, 320, 56, 24),      # the painted hauler
    [(484, 236), (520, 205), (566, 205), (604, 196), (604, 334), (484, 334)],  # pipe gantry
]
FLOOR_SEED = (768, 1000)     # certainly not sky (the atrium floor)
# Thin bright lines crossing the sky (the hanging cable, antennas, tower
# spikes) are a pixel or two wide, so the mask's opening erases them. They
# are found as residual (plate over star-removed) of at least LINE_LEVEL in a
# vertical run of LINE_RUN rows: stars are only a few pixels tall.
LINE_LEVEL, LINE_RUN = 10, 15


def _running(values, width, reduce, axis):
    """Running min/max of `width` along `axis` (edge-padded, same shape)."""
    pad = [(0, 0)] * values.ndim
    pad[axis] = (width // 2, width - 1 - width // 2)
    windows = np.lib.stride_tricks.sliding_window_view(np.pad(values, pad, mode="edge"),
                                                        width, axis=axis)
    return reduce(windows, axis=-1)


def thin_lines(plate):
    """Bool map of long, near-vertical bright hairlines (see LINE_RUN)."""
    grey = plate.convert("L")
    residual = (np.asarray(grey, dtype=np.int16) -
                np.asarray(star_removed(grey), dtype=np.int16))
    bright = _running(residual >= LINE_LEVEL, 3, np.max, 1)     # tolerate a 1 px lean
    runs = _running(bright, LINE_RUN, np.min, 0)
    grown = _running(_running(runs, LINE_RUN, np.max, 0), 5, np.max, 1)
    return grown & bright


def bake_sky(debug):
    out = REFINERY.anim
    plate = Image.open(REFINERY.plate).convert("RGB")
    mask = window_mask(plate, WINDOWS, FLOOR_SEED, HOLES)
    # After the mask's hole fill, so a hairline floating in a window stays.
    keep = Image.fromarray(thin_lines(plate).astype(np.uint8) * 255)
    keep = np.asarray(keep.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(0.7)),
                      dtype=np.float32) / 255.0
    mask = Image.fromarray((np.asarray(mask, dtype=np.float32) * (1.0 - keep)).astype(np.uint8))
    out.mkdir(parents=True, exist_ok=True)
    mask.save(out / "sky_mask.png", optimize=True)
    hazy_sky_fill(plate).save(out / "sky_fill.png", optimize=True)
    stars = PaintedStars(plate, mask)
    rng = np.random.default_rng(RNG_SEED + 584)
    star_tile(stars, 0.65, (0.32, 0.55), rng).save(out / "stars_far.png", optimize=True)
    star_tile(stars, 0.35, (0.4, 0.75), rng).save(out / "stars_near.png", optimize=True)
    print(f"sky {np.mean(np.asarray(mask) > 127):.1%} of plate; painted stars "
          f"{stars.density * 1e4:.1f}/10k px, contrast median "
          f"{np.median(stars.contrast) * 255:.0f}")
    if debug:
        vis = np.asarray(plate, dtype=np.float32)
        m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
        vis = vis * (1 - 0.6 * m) + np.array([0, 170, 255]) * 0.6 * m
        Path(debug).parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(vis.astype(np.uint8)).crop((0, 0, 1536, 420)).save(debug)


def bake_layers():
    """Straight-alpha sky passes (render_sky.py) -> under-plate sprite sheets."""
    bake_passes(SKY_TIMING, REFINERY.build / "sky", REFINERY.anim, (1536, 1024))



def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="write a mask-over-plate visualisation here")
    ap.add_argument("--layers-only", action="store_true", help="just bake the ship layers")
    args = ap.parse_args()
    if args.layers_only:
        bake_layers()
    else:
        bake_sky(args.debug)


if __name__ == "__main__":
    main()

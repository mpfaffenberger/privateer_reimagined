#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Cut the Military concourse's window out of the plate and drift its stars (#588).

The plate (assets/concourse/military/concourse_bg.png, the #621 repaint)
looks out through a huge brass-latticed window, plus a narrow slot right of
the pillar. As on New Con (newcon/bake_sky.py), the drifting tiles are drawn
from the painting's own stars (sky.PaintedStars), so they match the paint.
The original game drifted a star overlay across this window (the legacy
concourse_stt/stb overlays).

Outputs (assets/concourse/military/anim/):
    sky_mask.png    L8, 255 = sky: the engine's plate alpha, so stars and the
                    under-plate fighters show through the window only.
    sky_fill.png    the (black) sky colour, small and drawn stretched.
    stars_far.png / stars_near.png   tileable star fields, two drift speeds.
    fighter_pair.{png,json}   the Stiletto pair crossing beyond the window
                    (render_flyby.py): straight alpha, under the plate.

Usage (from the repo root):
    uv run tools/room_anim/military/bake_sky.py [--debug build/room_anim/military/sky_debug.png]
    uv run tools/room_anim/military/bake_sky.py --layers-only     # after render_flyby.py
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from bake_layer import bake_passes  # noqa: E402
from sky import RNG_SEED, PaintedStars, sky_fill, solidify, star_tile  # noqa: E402

MILITARY = paths("military")
TIMING = MILITARY.tools / "sky_layers.json"

# Coarse window regions (plate px). Anything outside them is never sky,
# however dark. Inside, the star-removed max channel decides.
WINDOWS = [
    # The big latticed window, down to the sill behind the walkway; the
    # brass lattice and the arch are bright, so the threshold drops them.
    [(12, 0), (880, 0), (880, 598), (12, 598)],
    # The slot between the pillar and the arch on the right.
    [(985, 0), (1085, 0), (1085, 395), (985, 395)],
]
SKY_MAX = 30               # star-removed max channel at or below this is sky
# Keep OUTLINE px off anything bright, so stars and ships don't creep onto
# the lattice's dark edges.
OUTLINE = 2
FLOOR_SEED = (768, 900)    # certainly not sky (the walkway)


def sky_mask(plate):
    peak = Image.fromarray(np.asarray(plate).max(axis=2))
    base = np.asarray(peak.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MaxFilter(5)))
    inside = Image.new("L", plate.size, 0)
    draw = ImageDraw.Draw(inside)
    for poly in WINDOWS:
        draw.polygon(poly, fill=255)
    # Bright = the lattice and frame, on the star-removed plate: painted stars
    # mustn't count, or each one punches a hole that eats the window's edge.
    bright = Image.fromarray(((base > SKY_MAX) * 255).astype(np.uint8))
    near_bright = np.asarray(bright.filter(ImageFilter.MaxFilter(2 * OUTLINE + 1))) > 0
    dark = (base <= SKY_MAX) & (np.asarray(inside) > 0) & ~near_bright
    return solidify(Image.fromarray(dark.astype(np.uint8) * 255), FLOOR_SEED)


def bake_sky(debug):
    out = MILITARY.anim
    plate = Image.open(MILITARY.plate).convert("RGB")
    mask = sky_mask(plate)
    out.mkdir(parents=True, exist_ok=True)
    mask.save(out / "sky_mask.png", optimize=True)
    sky_fill(plate, mask).save(out / "sky_fill.png", optimize=True)
    print(f"sky {np.mean(np.asarray(mask) > 127):.1%} of plate")
    if debug:
        vis = np.asarray(plate, dtype=np.float32)
        m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
        vis = vis * (1 - 0.6 * m) + np.array([0, 170, 255]) * 0.6 * m
        Path(debug).parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(vis.astype(np.uint8)).save(debug)


def bake_stars():
    out = MILITARY.anim
    plate = Image.open(MILITARY.plate).convert("RGB")
    stars = PaintedStars(plate, Image.open(out / "sky_mask.png"))
    rng = np.random.default_rng(RNG_SEED + 588)
    # As on New Con: most stars small and far, a third larger and near.
    star_tile(stars, 0.65, (0.32, 0.55), rng).save(out / "stars_far.png", optimize=True)
    star_tile(stars, 0.35, (0.4, 0.75), rng).save(out / "stars_near.png", optimize=True)
    print(f"stars: painted {stars.density * 1e4:.1f}/10k px, contrast median "
          f"{np.median(stars.contrast) * 255:.0f}")


def bake_layers():
    """Straight-alpha sky passes -> under-plate sprite sheets."""
    bake_passes(TIMING, MILITARY.build, MILITARY.anim, Image.open(MILITARY.plate).size,
                max_px=None)                       # small and sharp: keep full size



def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="write a mask-over-plate visualisation here")
    only = ap.add_mutually_exclusive_group()
    only.add_argument("--stars-only", action="store_true", help="just re-make the star tiles")
    only.add_argument("--layers-only", action="store_true", help="just bake the sky layers")
    args = ap.parse_args()
    if not (args.stars_only or args.layers_only):
        bake_sky(args.debug)
    if not args.layers_only:
        bake_stars()
    if not args.stars_only:
        bake_layers()


if __name__ == "__main__":
    main()

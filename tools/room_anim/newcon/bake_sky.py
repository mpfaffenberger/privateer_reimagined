#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Cut the New Con concourse sky out of the plate and build drifting stars (#515).

Why not Blender: the sky is a flat 2D backdrop seen through a painted cutout,
so there is no perspective to match. Pure image processing is simpler and
reproduces the painting's own sky exactly.

Outputs (assets/concourse/newcon/anim/):
    sky_mask.png   L8, 255 = sky. The engine uses it as the plate's alpha so
                   the layers below show through the windows.
    sky_fill.png   the painted sky with its stars removed (a quarter-res
                   gradient, drawn stretched behind everything).
    stars_far.png / stars_near.png   tileable RGBA star fields matched to the
                   painted stars' density and brightness; the engine scrolls
                   them at different rates for parallax.

Mask = hand-authored coarse window polygons (robust against the navy-dark rib
faces) refined per pixel by darkness of the star-removed plate (accurate lit
rib edges).

Usage (from the repo root):
    uv run tools/room_anim/newcon/bake_sky.py [--debug build/room_anim/newcon/sky_debug.png]
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from sky import RNG_SEED, PaintedStars, sky_fill, solidify, star_removed, star_tile  # noqa: E402

NEWCON = paths("newcon")

# Coarse window regions in plate pixels (1536x1024), each with the darkness
# threshold that refines its edges: star-removed luminance at or below it is
# sky. Anything outside every window is never sky, however dark. The C gaps
# carry blue haze (brighter sky) but their polygons are drawn conservatively
# inside the ribs, so they can afford a looser threshold.
WINDOWS = [
    # A: open sky left of the first arch, above the promenade roofline.
    ([(0, 0), (298, 0), (284, 110), (270, 240), (257, 368), (0, 368)], 24),
    # B: between arch 1 and arch 2.
    ([(568, 0), (765, 0), (752, 75), (738, 150), (724, 225), (712, 300), (706, 372),
      (596, 372), (586, 250), (576, 120)], 34),   # floor stops above the rooftop kit                 # dense stars at its top-right lift the floor
    # C1-C4: gaps between the curved ribs right of arch 2 (left to right).
    ([(910, 0), (1000, 0), (973, 79), (957, 158), (939, 237), (926, 316), (915, 395),
      (886, 395), (887, 158), (892, 79)], 45),
    ([(1102, 0), (1184, 0), (1155, 105), (1108, 211), (1081, 316), (1068, 395),
      (1044, 395), (1055, 316), (1063, 211), (1076, 105)], 45),
    ([(1250, 0), (1313, 0), (1282, 79), (1250, 158), (1224, 237), (1203, 316), (1192, 369),
      (1171, 369), (1176, 290), (1197, 211), (1224, 132), (1239, 53)], 45),
    ([(1318, 32), (1382, 21), (1374, 79), (1353, 148), (1334, 211), (1313, 290),
      (1300, 369), (1276, 369), (1287, 290), (1303, 211), (1311, 132)], 45),
]
FLOOR_SEED = (768, 1000)   # a pixel that is certainly not sky (the deck)


def sky_mask(plate):
    lum = plate.convert("L")
    base = np.asarray(star_removed(lum), dtype=np.float32)
    limit = Image.new("L", plate.size, 0)            # per-pixel threshold, 0 = never sky
    draw = ImageDraw.Draw(limit)
    for poly, darkness in WINDOWS:
        draw.polygon(poly, fill=darkness)
    limit = np.asarray(limit, dtype=np.float32)
    dark = Image.fromarray(((base <= limit) & (limit > 0)).astype(np.uint8) * 255)
    # Opening drops thin dark rib seams inside a window.
    return solidify(dark.filter(ImageFilter.MinFilter(7)).filter(ImageFilter.MaxFilter(7)),
                    FLOOR_SEED)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="write a mask-over-plate visualisation here")
    args = ap.parse_args()

    out = NEWCON.anim
    plate = Image.open(NEWCON.plate).convert("RGB")
    mask = sky_mask(plate)
    out.mkdir(parents=True, exist_ok=True)
    mask.save(out / "sky_mask.png", optimize=True)
    sky_fill(plate, mask).save(out / "sky_fill.png", optimize=True)

    stars = PaintedStars(plate, mask)
    rng = np.random.default_rng(RNG_SEED)
    # Split the painted population across two parallax depths: most stars
    # small and far, a third slightly larger and near.
    star_tile(stars, 0.65, (0.32, 0.55), rng).save(out / "stars_far.png", optimize=True)
    star_tile(stars, 0.35, (0.4, 0.75), rng).save(out / "stars_near.png", optimize=True)
    print(f"sky {np.mean(np.asarray(mask) > 127):.1%} of plate; painted stars "
          f"{stars.density * 1e4:.1f}/10k px, contrast median "
          f"{np.median(stars.contrast) * 255:.0f}")

    if args.debug:
        vis = np.asarray(plate, dtype=np.float32)
        m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
        vis = vis * (1 - 0.6 * m) + np.array([0, 170, 255]) * 0.6 * m
        Path(args.debug).parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(vis.astype(np.uint8)).crop((0, 0, 1536, 470)).save(args.debug)


if __name__ == "__main__":
    main()

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
    uv run tools/newcon_concourse/bake_sky.py [--debug build/newcon_concourse/sky_debug.png]
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

REPO = Path(__file__).resolve().parents[2]
PLATE = REPO / "assets/concourse/newcon/concourse_bg.png"
OUT = REPO / "assets/concourse/newcon/anim"

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
STAR_TILE = 512
RNG_SEED = 515


def star_removed(lum):
    return lum.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MaxFilter(5))


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


def solidify(sky, outside_seed):
    """Binary L sky (255) -> finished mask. Bright stars survive
    star_removed() and punch not-sky holes; a hole is any not-sky pixel that
    `outside_seed` (certainly not sky) cannot reach. Then blur + re-threshold
    rounds off square-kernel stair-steps, and a light feather gives a ~1 px
    antialiased edge."""
    sky = sky.copy()
    ImageDraw.floodfill(sky, outside_seed, 128)
    sky = Image.fromarray(np.where(np.asarray(sky) == 128, 0, 255).astype(np.uint8))
    rounded = np.asarray(sky.filter(ImageFilter.GaussianBlur(2.0))) >= 128
    return (Image.fromarray(rounded.astype(np.uint8) * 255)
            .filter(ImageFilter.GaussianBlur(0.8)))


def sky_fill(plate, mask):
    """Starless sky colour, extrapolated past the mask edge so bilinear
    stretching never pulls rib colour into the windows."""
    rgb = np.asarray(plate.convert("RGB"), dtype=np.float32)
    clean = np.dstack([np.asarray(star_removed(Image.fromarray(rgb[..., c].astype(np.uint8))),
                                  dtype=np.float32) for c in range(3)])
    w = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
    small = (plate.width // 4, plate.height // 4)
    blur = ImageFilter.GaussianBlur(24)

    def down(a):
        return np.asarray(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
                          .resize(small, Image.BOX).filter(blur), dtype=np.float32)
    num = np.dstack([down(clean[..., c] * w[..., 0]) for c in range(3)])
    den = down(w[..., 0] * 255.0)[..., None] / 255.0
    fill = num / np.maximum(den, 1e-3)
    fallback = np.array([6.0, 8.0, 14.0])                  # deep space blue-black
    fill = np.where(den > 0.02, fill, fallback)
    return Image.fromarray(np.clip(fill, 0, 255).astype(np.uint8))


class PaintedStars:
    """The painting's own star population: density per sky pixel (one per
    local maximum), and each star's contrast over the sky and its colour.
    New stars are drawn from this, so they match the paint, not a guess."""

    def __init__(self, plate, mask):
        grey = plate.convert("L")
        lum = np.asarray(grey, dtype=np.float32)
        base = np.asarray(star_removed(grey), dtype=np.float32)
        local_max = lum >= np.asarray(grey.filter(ImageFilter.MaxFilter(3)), dtype=np.float32)
        sky = np.asarray(mask) > 200
        peaks = (lum - base > 18) & sky & local_max
        rgb = np.asarray(plate, dtype=np.float32)[peaks]
        self.density = peaks.sum() / max(sky.sum(), 1)
        self.contrast = (lum - base)[peaks] / 255.0
        self.tint = rgb / np.maximum(rgb.max(axis=1, keepdims=True), 1.0)


def star_tile(stars, fraction, sigma_range, rng, gain=1.3, radius=3):
    """Tileable star field: gaussian dots wrapped at the tile edges. `gain`
    compensates for sub-pixel centres spreading a star's peak; `radius` (px)
    bounds each dot, so keep it >= 2.5 sigma for wide glows."""
    n = int(STAR_TILE * STAR_TILE * stars.density * fraction)
    acc = np.zeros((STAR_TILE, STAR_TILE, 3), dtype=np.float32)
    yy, xx = np.mgrid[-radius:radius + 1, -radius:radius + 1]
    for _ in range(n):
        x, y = rng.uniform(0, STAR_TILE, 2)
        sigma = rng.uniform(*sigma_range)
        pick = rng.integers(0, stars.contrast.size)
        ix, iy = int(x), int(y)
        g = np.exp(-(((xx - (x - ix)) ** 2 + (yy - (y - iy)) ** 2) / (2 * sigma ** 2)))
        rows, cols = (iy + yy) % STAR_TILE, (ix + xx) % STAR_TILE
        acc[rows, cols] += (g * stars.contrast[pick] * gain)[..., None] * stars.tint[pick]
    a = np.clip(acc.max(axis=2), 0.0, 1.0)
    rgb = np.where(a[..., None] > 0, acc / np.maximum(a[..., None], 1e-6), 0.0)
    return Image.fromarray((np.dstack([np.clip(rgb, 0, 1), a]) * 255).astype(np.uint8), "RGBA")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="write a mask-over-plate visualisation here")
    args = ap.parse_args()

    plate = Image.open(PLATE).convert("RGB")
    mask = sky_mask(plate)
    OUT.mkdir(parents=True, exist_ok=True)
    mask.save(OUT / "sky_mask.png", optimize=True)
    sky_fill(plate, mask).save(OUT / "sky_fill.png", optimize=True)

    stars = PaintedStars(plate, mask)
    rng = np.random.default_rng(RNG_SEED)
    # Split the painted population across two parallax depths: most stars
    # small and far, a third slightly larger and near.
    star_tile(stars, 0.65, (0.32, 0.55), rng).save(OUT / "stars_far.png", optimize=True)
    star_tile(stars, 0.35, (0.4, 0.75), rng).save(OUT / "stars_near.png", optimize=True)
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

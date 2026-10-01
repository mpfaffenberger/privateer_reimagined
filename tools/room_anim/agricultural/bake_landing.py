#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "scipy"]
# ///
"""Find the dusk sky over the Agricultural landing pad in every composite (#583).

The landing pad sits by a lake under a violet dusk sky with two moons, a
brick tower and two docking pylons against it. There are 18 per-hull
composites (assets/concourse/agricultural/landing_ships/<hull>.png), each
framed differently. For every composite this writes to
assets/concourse/agricultural/anim/landing/:

    <hull>_mask.png   L8, 255 = open sky (moons included)
    <hull>_fill.png   the painted sky at half resolution, moons and all,
                      extrapolated past the mask edge (sky.half_fill)
    anchors.json      {"<hull>": [cx, cy, r]}: tarsus's fixed point carried by
                      the silhouettes' registration (r fixed)
    <layer>.{json,png}
                      aircraft (render_landing.py), under the plate and
                      anchored, so the tower and pylons occlude them on
                      every hull.

Usage (from the repo root):
    uv run tools/room_anim/agricultural/bake_landing.py [--debug build/room_anim/agricultural/skies.png]
    uv run tools/room_anim/agricultural/bake_landing.py --layers-only    # after render_landing.py
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent)]         # this base's modules, then shared ones
from bake_layer import bake_passes  # noqa: E402
from base import paths  # noqa: E402
from sky import contact_sheet, half_fill  # noqa: E402

AGRI = paths("agricultural")
COMPOSITES = AGRI.room / "landing_ships"
OUT = AGRI.anim / "landing"
BUILD = AGRI.build / "landing"
TIMING = AGRI.tools / "landing_layers.json"
CANVAS = (1536, 1024)

# The sky runs from dusky violet-blue at the top to salmon pink at the
# horizon, with pink clouds, two moons and hazy far hills. Green is its
# weakest channel everywhere: blue leads it up high, red leads it low down
# (with blue close behind). The pylons and tower are neutral grey or brick
# (blue well under green), fields and trees are green, and shade is dark.
# Colour alone leaks, though: the tower's windows mirror the sky. The sky
# and clouds are soft, the buildings are all edges, so sky = pixels with
# that violet/pink balance AND little local texture, flooded from the top
# edge, then grown back up to EDGE_GROW px (by colour alone) to the
# silhouettes the texture blur shaved off. The far shore and the lake
# (which mirrors the sky) are cut off below CANON_FLOOR (moved with each
# hull's anchor): the aircraft stay well above it.
SKY_LUM = 50                # shade on the pylons is darker than this
BLUE_LEAD = 20              # violet-blue sky: blue this far above green
PINK_LEAD = (25, -5)        # pink sky/clouds: red this far above green, blue at least this
TEXTURE_SIGMA = 4.0         # px: blur of the luma gradient magnitude
TEXTURE_MAX = 20.0          # open sky 4-7, cloud banks 13-15 (median); building faces 23+
EDGE_GROW = 10              # px grown back toward the silhouettes
# Anchors: the aircraft paths are aimed at tarsus's silhouettes (flight
# past the pylons and behind the tower), so every hull's layers follow ITS
# silhouettes: the translation that best lines up the sky mask's outline
# with tarsus's (FFT cross-correlation at half resolution). Not the moons:
# each hull composite was repainted separately and its moons wander (300 px
# on paradigm, while the tower moves 10). Translation only, r fixed: the
# hulls are framed at nearly the same zoom.
CANONICAL = "tarsus"
CANON_ANCHOR = (768.0, 512.0)   # any fixed point in tarsus's frame
CANON_FLOOR = 470               # tarsus: below this nothing is sky (the lake mirrors it)
REG_ROWS = 660                  # outline rows used: the silhouettes against the sky
MAX_SHIFT = 400                 # px: the hulls' framings differ by far less
ANCHOR_R = 100.0


def skyish(rgb):
    r, g, b = (rgb[..., c].astype(np.int32) for c in range(3))
    lum = (r * 299 + g * 587 + b * 114) // 1000
    violet = b - g >= BLUE_LEAD
    pink = (r - g >= PINK_LEAD[0]) & (b - g >= PINK_LEAD[1])
    return (lum >= SKY_LUM) & (violet | pink)


def smooth(rgb):
    lum = rgb.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)
    grad = np.hypot(ndimage.sobel(lum, 0), ndimage.sobel(lum, 1))
    return ndimage.gaussian_filter(grad, TEXTURE_SIGMA) < TEXTURE_MAX


def flood_from_top(cand):
    labels, _ = ndimage.label(cand)
    top = np.unique(labels[0][labels[0] > 0])
    return np.isin(labels, top)


def sky_region(rgb):
    colour = skyish(rgb)
    sky = flood_from_top(colour & smooth(rgb))
    sky = ndimage.binary_dilation(sky, iterations=EDGE_GROW, mask=colour)
    return ndimage.binary_fill_holes(sky)


def outline(sky):
    """Half-res, softened edge of the sky mask (the silhouettes' skyline)."""
    h, w = sky.shape[0] // 2, sky.shape[1] // 2
    half = sky[:h * 2, :w * 2].reshape(h, 2, w, 2).mean(axis=(1, 3)) > 0.5
    edge = (half ^ ndimage.binary_erosion(half, border_value=1)).astype(np.float32)   # not the frame
    edge[REG_ROWS // 2:] = 0.0
    return ndimage.gaussian_filter(edge, 1.5)


def register(edge, ref):
    """Shift (dx, dy) in full-res px that moves `ref`'s outline onto `edge`'s."""
    h, w = edge.shape
    corr = np.fft.irfft2(np.fft.rfft2(edge, (2 * h, 2 * w)) *
                         np.conj(np.fft.rfft2(ref, (2 * h, 2 * w))), (2 * h, 2 * w))
    lim = MAX_SHIFT // 2
    dy = np.r_[0:lim + 1, -lim:0] % (2 * h)
    dx = np.r_[0:lim + 1, -lim:0] % (2 * w)
    win = corr[np.ix_(dy, dx)]
    iy, ix = np.unravel_index(int(np.argmax(win)), win.shape)
    sy, sx = int(np.r_[0:lim + 1, -lim:0][iy]), int(np.r_[0:lim + 1, -lim:0][ix])
    return 2.0 * sx, 2.0 * sy


def finish_mask(sky, dy):
    """Cut the lake off below the canonical floor (moved by dy); soften."""
    sky = sky.copy()
    sky[max(0, int(CANON_FLOOR + dy)):] = False
    return Image.fromarray(sky.astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.7))


def bake_skies(debug):
    OUT.mkdir(parents=True, exist_ok=True)
    plates = {p.stem: Image.open(p).convert("RGB") for p in sorted(COMPOSITES.glob("*.png"))}
    skies = {hull: sky_region(np.asarray(plate)) for hull, plate in plates.items()}
    ref = outline(skies[CANONICAL])
    anchors, thumbs = {}, []
    for hull, plate in plates.items():
        dx, dy = register(outline(skies[hull]), ref)
        anchor = [CANON_ANCHOR[0] + dx, CANON_ANCHOR[1] + dy, ANCHOR_R]
        mask = finish_mask(skies[hull], dy)
        mask.save(OUT / f"{hull}_mask.png", optimize=True)
        half_fill(plate, mask).save(OUT / f"{hull}_fill.png", optimize=True)
        anchors[hull] = anchor
        print(f"{hull:<11} shift ({dx:+5.0f},{dy:+5.0f})  sky {np.mean(np.asarray(mask) > 127):.1%}")
        if debug:
            vis = np.asarray(plate, dtype=np.float32)
            m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
            vis = vis * (1 - 0.6 * m) + np.array([255, 200, 0]) * 0.6 * m
            thumb = Image.fromarray(vis.astype(np.uint8))
            d = ImageDraw.Draw(thumb)
            ref_line = np.argwhere(ref > 0.2) * 2 + [dy, dx]      # tarsus's skyline, shifted
            d.point([(int(x), int(y)) for y, x in ref_line[::3]], fill=(255, 0, 0))
            d.text((12, 12), hull, fill=(255, 255, 255))
            thumbs.append(thumb.resize((384, 256)))
    (OUT / "anchors.json").write_text(json.dumps(anchors, indent=1) + "\n")
    if debug:
        contact_sheet(thumbs, debug)


def bake_layers():
    """Rendered sky passes -> under-plate, anchored sprite sheets."""
    bake_passes(TIMING, BUILD, OUT, CANVAS)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="contact sheet: every sky, with tarsus's skyline registered on it")
    ap.add_argument("--layers-only", action="store_true", help="just bake the sky layers")
    args = ap.parse_args()
    if not args.layers_only:
        bake_skies(args.debug)
    if TIMING.exists():
        bake_layers()


if __name__ == "__main__":
    main()

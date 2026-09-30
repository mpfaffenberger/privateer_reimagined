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
    anchors.json      {"<hull>": [cx, cy, r]}: the big moon's centre (r fixed)
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

# The sky is a saturated blue-to-violet-to-pink gradient, and the moons are
# bluish too (shaded on most hulls, lit pink on gladius). Green stays low in
# all of it, while the brick tower, pylons, hulls and the dark far shore have
# no blue lead over green. So sky = pixels well bluer than green, flooded
# from the top edge: the far shore is a dark band all the way across, so the
# lake, which mirrors the sky's colour, is never reached.
SKY_BLUE = 90               # blue channel at least this (dralthi's top: ~120 +- dither)
SKY_BLUE_LEAD = 45          # and this far above green
MOON_DIFF = 30              # moon: this far (RGB distance) from its row's sky
MIN_MOON_PX = 800           # the big moon is thousands of px on every hull
# Anchors are translation only: layers follow the big moon's centre. Its
# detected size is not a reliable zoom (a shaded limb melts into the sky),
# so r is fixed and anchored layers are never rescaled.
ANCHOR_R = 100.0


def skyish(rgb):
    g, b = rgb[..., 1].astype(np.int16), rgb[..., 2].astype(np.int16)
    return (b >= SKY_BLUE) & (b - g >= SKY_BLUE_LEAD)


def find_sky(plate):
    """-> (mask L image, [cx, cy, r] of the big moon)."""
    rgb = np.asarray(plate)
    cand = skyish(rgb)
    labels, _ = ndimage.label(cand)
    top = np.unique(labels[0][labels[0] > 0])
    sky = np.isin(labels, top)
    # The moons sit inside the sky; the fill carries them, so they stay sky.
    sky = ndimage.binary_fill_holes(sky)
    # Big moon: the largest blob inside the sky that stands out from its row.
    px = rgb.astype(np.float32)
    ref = np.array([np.median(px[y][sky[y]], axis=0) if sky[y].sum() > 20 else np.zeros(3)
                    for y in range(sky.shape[0])], np.float32)[:, None, :]
    moon = sky & (np.linalg.norm(px - ref, axis=2) > MOON_DIFF)
    moon = ndimage.binary_opening(moon, iterations=2)
    mlabels, n = ndimage.label(moon)
    if n == 0:
        raise ValueError("no moon found")
    sizes = ndimage.sum(moon, mlabels, range(1, n + 1))
    big = int(np.argmax(sizes)) + 1
    if sizes[big - 1] < MIN_MOON_PX:
        raise ValueError(f"moon too small ({sizes[big - 1]:.0f} px)")
    ys, xs = np.nonzero(ndimage.binary_fill_holes(mlabels == big))
    anchor = [round(float(xs.mean()), 1), round(float(ys.mean()), 1), ANCHOR_R]
    img = Image.fromarray(sky.astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.7))
    return img, anchor



def bake_skies(debug):
    OUT.mkdir(parents=True, exist_ok=True)
    anchors, thumbs = {}, []
    for path in sorted(COMPOSITES.glob("*.png")):
        plate = Image.open(path).convert("RGB")
        mask, anchor = find_sky(plate)
        hull = path.stem
        mask.save(OUT / f"{hull}_mask.png", optimize=True)
        half_fill(plate, mask).save(OUT / f"{hull}_fill.png", optimize=True)
        anchors[hull] = anchor
        print(f"{hull:<11} moon {anchor}  sky {np.mean(np.asarray(mask) > 127):.1%}")
        if debug:
            vis = np.asarray(plate, dtype=np.float32)
            m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
            vis = vis * (1 - 0.6 * m) + np.array([255, 200, 0]) * 0.6 * m
            thumb = Image.fromarray(vis.astype(np.uint8))
            d = ImageDraw.Draw(thumb)
            cx, cy, _ = anchor
            d.ellipse([cx - 12, cy - 12, cx + 12, cy + 12], outline=(255, 0, 0), width=5)
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
    ap.add_argument("--debug", help="contact sheet of every detected sky and moon")
    ap.add_argument("--layers-only", action="store_true", help="just bake the sky layers")
    args = ap.parse_args()
    if not args.layers_only:
        bake_skies(args.debug)
    if TIMING.exists():
        bake_layers()


if __name__ == "__main__":
    main()

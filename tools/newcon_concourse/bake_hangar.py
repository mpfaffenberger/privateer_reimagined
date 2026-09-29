#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Find the hangar mouth in every New Con landing composite (#553).

The landing room shows one of 18 full-frame composites (one per player hull,
assets/concourse/newcon/landing_ships/<ship>.png), each framed differently.
In each, the hangar mouth is a dark, star-filled disc ringed by lit tunnel
walls. For every composite this writes to assets/concourse/newcon/anim/hangar/:

    <ship>_mask.png   L8, 255 = open space seen through the mouth
    <ship>_fill.png   the starless space colour (small, drawn stretched)
    anchors.json      {"<ship>": [cx, cy, r]} mouth centre/radius, plate px
    stars_far.png,    drifting star tiles, drawn from the star population
    stars_near.png    painted in the mouths (pooled over every composite)

The engine draws stars (and far-off ship traffic) behind the masked plate, and
maps mouth-anchored sprite layers onto [cx, cy, r].

Usage (from the repo root):
    uv run tools/newcon_concourse/bake_hangar.py [--debug build/newcon_concourse/mouths.png]
"""
import argparse
import json
import math
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

from bake_sky import RNG_SEED, PaintedStars, sky_fill, solidify, star_removed, star_tile

REPO = Path(__file__).resolve().parents[2]
COMPOSITES = REPO / "assets/concourse/newcon/landing_ships"
OUT = REPO / "assets/concourse/newcon/anim/hangar"

SPACE_DARKNESS = 30      # star-removed luminance at or below this is open space
SEED_REGION = (0.2, 0.05, 0.8, 0.6)   # where to look for the mouth (fractions of W/H)
MIN_MOUTH_FRACTION = 0.01             # sanity: the mouth covers at least 1% of frame
RIM_ROWS = 120           # top rows of the flood used to fit the rim circle
DISC_SLACK = 1.03        # keep flood pixels within this multiple of the fitted radius


def fit_rim(flood):
    """Circle through the left/right edges of the flood's top RIM_ROWS rows
    (Kasa least-squares fit). The top arc of the mouth is clean in every
    composite; lower down, dark hull shadow can join the flood (drayman)."""
    ys, xs = np.nonzero(flood)
    top = ys.min()
    pts = []
    for y in range(top, min(top + RIM_ROWS, flood.shape[0])):
        row = np.nonzero(flood[y])[0]
        if row.size:
            pts += [(row.min(), y), (row.max() + 1, y)]
    p = np.asarray(pts, dtype=np.float64)
    a = np.column_stack([p[:, 0], p[:, 1], np.ones(len(p))])
    b = -(p[:, 0] ** 2 + p[:, 1] ** 2)
    d, e, f = np.linalg.lstsq(a, b, rcond=None)[0]
    cx, cy = -d / 2.0, -e / 2.0
    return cx, cy, math.sqrt(cx * cx + cy * cy - f)


def haze_ramp(mask, base):
    """Soften the mask where painted haze (the light shaft) fades into space.
    A hard threshold cuts the haze's faint tail off in a jagged line; instead
    the mask falls from 1 at the space level S to 0 at SPACE_DARKNESS, so it
    meets the unmasked plate continuously. Over the fill F the plate shows
    a*P + (1-a)*F, at most (T-S)/4 levels darker than paint: invisible."""
    m = np.asarray(mask, dtype=np.float32) / 255.0
    lum = np.asarray(Image.fromarray(base.astype(np.uint8)).filter(ImageFilter.GaussianBlur(2)),
                     dtype=np.float32)
    space = float(np.percentile(lum[m > 0.99], 10))
    ramp = np.clip((SPACE_DARKNESS - lum) / max(SPACE_DARKNESS - space, 1.0), 0.0, 1.0)
    return Image.fromarray(np.round(m * ramp * 255.0).astype(np.uint8))


def find_mouth(plate):
    """-> (mask L image, [cx, cy, r]) for one composite."""
    w, h = plate.size
    base = np.asarray(star_removed(plate.convert("L")), dtype=np.float32)
    dark = Image.fromarray((base <= SPACE_DARKNESS).astype(np.uint8) * 255)
    dark = dark.filter(ImageFilter.MinFilter(7)).filter(ImageFilter.MaxFilter(7))

    # Seed = the darkest spot of a heavily blurred plate inside the region
    # where the mouth can be; flood only its connected dark component.
    blurred = np.asarray(Image.fromarray(base.astype(np.uint8))
                         .filter(ImageFilter.BoxBlur(24)), dtype=np.float32)
    x0, y0, x1, y1 = (int(SEED_REGION[0] * w), int(SEED_REGION[1] * h),
                      int(SEED_REGION[2] * w), int(SEED_REGION[3] * h))
    region = blurred[y0:y1, x0:x1]
    sy, sx = np.unravel_index(np.argmin(region), region.shape)
    seed = (int(x0 + sx), int(y0 + sy))
    if dark.getpixel(seed) != 255:
        raise ValueError(f"darkest seed {seed} is not open space")
    ImageDraw.floodfill(dark, seed, 200)
    flood = np.asarray(dark) == 200
    cx, cy, r = fit_rim(flood)
    yy, xx = np.mgrid[0:h, 0:w]
    inside = (xx - cx) ** 2 + (yy - cy) ** 2 <= (r * DISC_SLACK) ** 2
    mouth = Image.fromarray((flood & inside).astype(np.uint8) * 255)
    mask = haze_ramp(solidify(mouth, (w // 2, h - 1)), base)

    coverage = np.mean(np.asarray(mask) > 127)
    if coverage < MIN_MOUTH_FRACTION:
        raise ValueError(f"mouth too small ({coverage:.2%} of frame)")
    return mask, [round(cx, 1), round(cy, 1), round(r, 1)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="contact sheet of every detected mouth")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    anchors, thumbs, painted = {}, [], []
    for path in sorted(COMPOSITES.glob("*.png")):
        plate = Image.open(path).convert("RGB")
        mask, anchor = find_mouth(plate)
        ship = path.stem
        mask.save(OUT / f"{ship}_mask.png", optimize=True)
        fill = sky_fill(plate, mask)
        fill.resize((fill.width // 2, fill.height // 2), Image.BOX).save(
            OUT / f"{ship}_fill.png", optimize=True)
        anchors[ship] = anchor
        painted.append(PaintedStars(plate, mask))
        print(f"{ship:<11} mouth c=({anchor[0]:.0f},{anchor[1]:.0f}) r={anchor[2]:.0f}  "
              f"covers {np.mean(np.asarray(mask) > 127):.1%}")
        if args.debug:
            vis = np.asarray(plate, dtype=np.float32)
            m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
            vis = vis * (1 - 0.55 * m) + np.array([0, 170, 255]) * 0.55 * m
            thumb = Image.fromarray(vis.astype(np.uint8))
            d = ImageDraw.Draw(thumb)
            cx, cy, r = anchor
            d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 60, 60), width=4)
            d.text((12, 12), ship, fill=(255, 255, 0))
            thumbs.append(thumb.resize((384, 256)))
    (OUT / "anchors.json").write_text(json.dumps(anchors, indent=1) + "\n")

    # One star population for all composites (same painting, re-framed).
    stars = SimpleNamespace(density=float(np.mean([p.density for p in painted])),
                            contrast=np.concatenate([p.contrast for p in painted]),
                            tint=np.concatenate([p.tint for p in painted]))
    rng = np.random.default_rng(RNG_SEED + 553)
    star_tile(stars, 0.65, (0.32, 0.55), rng).save(OUT / "stars_far.png", optimize=True)
    star_tile(stars, 0.35, (0.4, 0.75), rng).save(OUT / "stars_near.png", optimize=True)
    print(f"painted stars {stars.density * 1e4:.1f}/10k px, contrast median "
          f"{np.median(stars.contrast) * 255:.0f}")

    if args.debug and thumbs:
        cols = 6
        sheet = Image.new("RGB", (cols * 384, math.ceil(len(thumbs) / cols) * 256))
        for i, t in enumerate(thumbs):
            sheet.paste(t, ((i % cols) * 384, (i // cols) * 256))
        Path(args.debug).parent.mkdir(parents=True, exist_ok=True)
        sheet.save(args.debug)


if __name__ == "__main__":
    main()

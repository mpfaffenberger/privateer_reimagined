#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "scipy"]
# ///
"""Cut the Pleasure concourse's skylight out of the plate and build drifting stars (#594).

The hall is roofed by a glass pyramid: starry sky shows through its panes
above the dark panelled wall at the back. As on New Con (newcon/bake_sky.py),
that's a flat 2D backdrop behind a painted cut-out, so no Blender is needed.

Outputs (assets/concourse/pleasure/anim/):
    sky_mask.png   L8, 255 = sky: the plate's alpha, so the stars show through.
    sky_fill.png   the painted sky with its stars removed, drawn stretched.
    stars_far.png / stars_near.png   tileable star fields matched to the
                   painted stars; the engine scrolls them at two rates.

Usage (from the repo root):
    uv run tools/room_anim/pleasure/bake_sky.py [--debug build/room_anim/pleasure/sky_debug.png]
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from sky import RNG_SEED, PaintedStars, sky_fill, star_removed, star_tile  # noqa: E402

PLEASURE = paths("pleasure")

# Pane polygons in plate pixels (1536x1024), traced between the rafters and
# inset ~6 px, each with the darkness at or below which its star-removed
# luminance is sky. The rafters here are wide dark beams with lit rims: as
# dark as the sky, so only hand-traced panes keep stars off their faces.
# The rail tangle left of the apex is left painted.
WINDOWS = [
    # The apex pane between the two main rafters, down to the rail above the
    # wall's ledge (below it may be wall top, so it stays painted).
    ([(797, 0), (866, 0), (944, 99), (710, 99)], 32),
    # The right pane behind the glass sheen, between the right rafter's body
    # and the upper and lower rails.
    ([(1012, 52), (1045, 45), (1170, 128), (1170, 142), (1075, 185), (1065, 180)], 48),
]
# Glare streaks and thin mullions inside a pane are barely brighter than the
# sky, so no threshold separates them. Shape does: anything standing RISE
# above the local sky is a star if its blob is small, and glass (stays
# painted) if it spans STRUCTURE_PX or more.
RISE = 5
STRUCTURE_PX = 10
# Most painted stars here are faint (8-18 over the sky: ~49/10k px, against
# ~10/10k at New Con's 18), and the fill removes them, so count them too.
STAR_RISE = 8


def structure(lum, base):
    """Long blobs brighter than the local sky, grown by a pixel so their soft
    edges stay painted."""
    raw = np.asarray(lum, dtype=np.float32)
    blobs, _ = ndimage.label(raw > base + RISE)
    long = [i + 1 for i, box in enumerate(ndimage.find_objects(blobs))
            if max(s.stop - s.start for s in box) >= STRUCTURE_PX]
    return ndimage.binary_dilation(np.isin(blobs, long))


def sky_mask(plate):
    """Sky inside the panes, minus the glass structure, with a ~1 px
    antialiased edge. Stars are sky already (star_removed() ignores them), so
    unlike sky.solidify() there are no holes to fill: filling would paint the
    glare streaks enclosed in a pane out."""
    lum = plate.convert("L")
    base = np.asarray(star_removed(lum), dtype=np.float32)
    limit = Image.new("L", plate.size, 0)            # per-pixel threshold, 0 = never sky
    draw = ImageDraw.Draw(limit)
    for poly, darkness in WINDOWS:
        draw.polygon(poly, fill=darkness)
    limit = np.asarray(limit, dtype=np.float32)
    sky = (base <= limit) & (limit > 0) & ~structure(lum, base)
    return Image.fromarray(sky.astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.8))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="write a mask-over-plate visualisation here")
    args = ap.parse_args()

    out = PLEASURE.anim
    plate = Image.open(PLEASURE.plate).convert("RGB")
    mask = sky_mask(plate)
    out.mkdir(parents=True, exist_ok=True)
    mask.save(out / "sky_mask.png", optimize=True)
    sky_fill(plate, mask).save(out / "sky_fill.png", optimize=True)

    stars = PaintedStars(plate, mask, rise=STAR_RISE)
    rng = np.random.default_rng(RNG_SEED)
    # Two parallax depths from the painted population: most small and far.
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
        Image.fromarray(vis.astype(np.uint8)).crop((540, 0, 1240, 330)).save(args.debug)


if __name__ == "__main__":
    main()

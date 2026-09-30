#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Cut the Military concourse's window out of the plate and fill it with stars (#588).

The plate (assets/concourse/military/concourse_bg.png, 2564x2016) looks out
through a huge gold-latticed window, plus a narrow slot right of the teal
pillar. The sky is painted pure black (max channel 0) and starless, so the
stars are a sparse synthetic field (sky.synthetic_stars), like the mining
landing pad's. The original game drifted a star overlay across this window
(the legacy concourse_stt/stb overlays).

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
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from bake_layer import load_frame, write_sheet  # noqa: E402
from sky import RNG_SEED, sky_fill, solidify, star_tile, synthetic_stars  # noqa: E402

MILITARY = paths("military")
TIMING = MILITARY.tools / "sky_layers.json"

# Coarse window regions (plate px). Anything outside them is never sky,
# however dark: the plate has a black border (left 2 px, bottom 14 px) and
# black outlines everywhere. Inside, the star-removed max channel decides.
WINDOWS = [
    # The big latticed window, down to the sill behind the walkway; the left
    # teal wall and the gold arch are bright, so the threshold drops them.
    [(24, 0), (1575, 0), (1575, 1222), (560, 1222), (24, 900)],
    # The slot between the teal pillar and the arch on the right.
    [(1740, 0), (1930, 0), (1930, 830), (1740, 830)],
]
SKY_MAX = 24               # max channel at or below this is sky (the paint is 0)
# The painting outlines every lattice bar and frame in near-black, which
# reads as sky. Keep OUTLINE px off anything bright, so stars and ships don't
# eat the outlines.
OUTLINE = 3
FLOOR_SEED = (1280, 1800)  # certainly not sky (the walkway)

# Sparse: the legacy overlay's field, at this plate's ~1.7x resolution
# (mining's landing sky is 8e-4 per px at 1536 wide).
STAR_DENSITY = 3.2e-4


def sky_mask(plate):
    peak = Image.fromarray(np.asarray(plate).max(axis=2))
    base = np.asarray(peak.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MaxFilter(5)))
    inside = Image.new("L", plate.size, 0)
    draw = ImageDraw.Draw(inside)
    for poly in WINDOWS:
        draw.polygon(poly, fill=255)
    bright = Image.fromarray(((np.asarray(peak) > SKY_MAX) * 255).astype(np.uint8))
    near_bright = np.asarray(bright.filter(ImageFilter.MaxFilter(2 * OUTLINE + 1))) > 0
    dark = (base <= SKY_MAX) & (np.asarray(inside) > 0) & ~near_bright
    return solidify(Image.fromarray(dark.astype(np.uint8) * 255), FLOOR_SEED)


def bake_sky(debug):
    out = MILITARY.anim
    plate = Image.open(MILITARY.plate).convert("RGB")
    mask = sky_mask(plate)
    out.mkdir(parents=True, exist_ok=True)
    mask.save(out / "sky_mask.png", optimize=True)
    fill = sky_fill(plate, mask)
    fill.resize((fill.width // 4, fill.height // 4), Image.BOX).save(out / "sky_fill.png",
                                                                     optimize=True)
    print(f"sky {np.mean(np.asarray(mask) > 127):.1%} of plate")
    if debug:
        vis = np.asarray(plate, dtype=np.float32)
        m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
        vis = vis * (1 - 0.6 * m) + np.array([0, 170, 255]) * 0.6 * m
        Path(debug).parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(vis.astype(np.uint8)).save(debug)


def bake_stars():
    rng = np.random.default_rng(RNG_SEED + 588)
    stars = synthetic_stars(rng, STAR_DENSITY)
    out = MILITARY.anim
    out.mkdir(parents=True, exist_ok=True)
    # Sigmas ~1.7x mining's: this plate has ~1.7x the pixels per degree.
    star_tile(stars, 0.7, (0.5, 0.9), rng, radius=4).save(out / "stars_far.png", optimize=True)
    star_tile(stars, 0.3, (0.75, 1.35), rng, radius=5).save(out / "stars_near.png", optimize=True)
    print("stars: far + near tiles")


def bake_layers():
    """Straight-alpha sky passes -> under-plate sprite sheets."""
    canvas = Image.open(MILITARY.plate).size
    for layer, t in json.loads(TIMING.read_text()).items():
        if layer.startswith("_"):
            continue
        src = MILITARY.build / layer
        info = json.loads((src / "pass.json").read_text())
        sprites, slots = [], []
        for path in sorted(src.glob("*.png")):
            baked = load_frame(path, max_px=None)          # small and sharp: keep full size
            if baked is not None:
                sprites.append(baked)
                slots.append(int(path.stem) - 1)          # frame N -> slot N-1
        fps = float(info["fps"])
        period_frames = max(int(info["frames"]), int(math.ceil(t["period"] * fps)))
        write_sheet(MILITARY.anim, layer, sprites, slots, canvas, fps, period_frames,
                    int(round(t["offset"] * fps)), under=True)


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

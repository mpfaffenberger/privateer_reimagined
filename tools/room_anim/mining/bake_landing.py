#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Find the open sky above the crater in every mining landing composite (#561).

The mining landing pad is an open crater: each of the 18 per-hull composites
(assets/concourse/mining/landing_ships/<hull>.png, framed differently) shows a
strip of black sky above a jagged rock rim. For every composite this writes
to assets/concourse/mining/anim/landing/:

    <hull>_mask.png   L8, 255 = open sky
    <hull>_fill.png   the sky colour (small, drawn stretched)
    anchors.json      {"<hull>": [cx, cy, r]}: cy is the rim height, see find_sky()
    stars_far.png, stars_near.png
                      star tiles shared by every hull. The painted sky is
                      starless, so these are a deliberate, sparse addition.
    galaxy_flyover.{json,png}
                      the freighter (render_landing.py), under the plate and
                      anchored, so the rim and dish occlude it on every hull.

Usage (from the repo root):
    uv run tools/room_anim/mining/bake_landing.py [--debug build/room_anim/mining/skies.png]
    uv run tools/room_anim/mining/bake_landing.py --stars-only
    uv run tools/room_anim/mining/bake_landing.py --layers-only    # after render_landing.py
"""
import argparse
import json
import math
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from bake_layer import load_frame, write_sheet  # noqa: E402
from sky import RNG_SEED, sky_fill, solidify, star_tile  # noqa: E402

MINING = paths("mining")
COMPOSITES = MINING.room / "landing_ships"
OUT = MINING.anim / "landing"
BUILD = MINING.build / "landing"
TIMING = MINING.tools / "landing_layers.json"
CANVAS = (1536, 1024)

# The sky is painted near-black, but each composite has its own black level
# (max channel: broadsword 0, scout 2, demon 6, tarsus 19) and some are shaded
# (tarsus runs 19-27). The rock's crevices are about as dark (scout: median
# 14 vs its sky's 2), so no brightness threshold separates them: a loose one
# floods down the crevices, a tight one punches holes in a shaded sky.
# Structure does: the rim is a height field. Scanning each column down from
# the top, it's sky until the first run of ROCK_RUN rows brighter than the
# sky level + ROCK_MARGIN. Every crevice lies below that, so none can leak in.
ROCK_MARGIN = 12
ROCK_RUN = 3
# ...except a crevice that is dark all the way up to the rim, where the scan
# drips down it. A rim is continuous, so the drips are narrow downward spikes
# in the rim profile: an opening (running min, then running max) over
# RIM_SMOOTH columns removes them, and keeps wider notches and the dish (a
# narrow UPWARD spike, which an opening leaves alone).
RIM_SMOOTH = 41
SKY_LEVEL_ROWS = 8         # the sky level is measured in the top rows, where
SKY_LEVEL_PERCENTILE = 25  # sky is the majority
MIN_SKY_FRACTION = 0.005   # sanity: the sky covers at least 0.5% of the frame

# A clear sky over an airless rock: sparse, mostly faint, a few bright, and
# a white-to-blue spread with the odd warm one. New Con's painted skies run
# 2-13 stars per 10k px; this sits mid-range.
STAR_DENSITY = 8e-4
STAR_POPULATION = 400
STAR_TINTS = ((1.0, 1.0, 1.0), (0.78, 0.86, 1.0), (0.66, 0.78, 1.0), (1.0, 0.9, 0.75))
STAR_TINT_ODDS = (0.45, 0.3, 0.15, 0.1)


def _open_profile(values, width):
    """1-D morphological opening: removes peaks narrower than `width`."""
    pad = width // 2
    windows = np.lib.stride_tricks.sliding_window_view
    eroded = windows(np.pad(values, pad, mode="edge"), width).min(axis=1)
    return windows(np.pad(eroded, pad, mode="edge"), width).max(axis=1)


def find_sky(plate):
    """-> (mask L image, [cx, cy, r]). The sky is everything above the rim,
    column by column (see ROCK_MARGIN)."""
    w, h = plate.size
    peak = np.asarray(plate, dtype=np.uint8).max(axis=2).astype(np.int16)
    level = float(np.percentile(peak[:SKY_LEVEL_ROWS], SKY_LEVEL_PERCENTILE))
    bright = peak > level + ROCK_MARGIN
    rock = bright[:h - ROCK_RUN + 1].copy()
    for k in range(1, ROCK_RUN):                         # a sustained run, not a speck
        rock &= bright[k:h - ROCK_RUN + 1 + k]
    rim = np.where(rock.any(axis=0), rock.argmax(axis=0), h)   # first rock row per column
    rim = _open_profile(rim, RIM_SMOOTH)
    sky = np.arange(h)[:, None] < rim[None, :]
    mask = solidify(Image.fromarray(sky.astype(np.uint8) * 255), (w // 2, h - 1))
    m = np.asarray(mask) > 127
    coverage = m.mean()
    if coverage < MIN_SKY_FRACTION:
        raise ValueError(f"sky too small ({coverage:.2%} of frame)")
    # The composites differ mainly in how high the rim sits; the strip spans
    # most of the width in all of them, so its width says nothing about zoom.
    # The anchor is therefore vertical only: cy = the rim's median height
    # (robust to a notch), cx/r fixed, so anchored layers slide with the rim
    # and are never rescaled.
    cols = m.any(axis=0)
    rim = h - 1 - np.argmax(m[::-1, cols], axis=0)       # lowest sky row per column
    return mask, [w / 2, round(float(np.median(rim)), 1), w / 2]


def bake_skies(debug):
    OUT.mkdir(parents=True, exist_ok=True)
    anchors, thumbs = {}, []
    for path in sorted(COMPOSITES.glob("*.png")):
        plate = Image.open(path).convert("RGB")
        mask, anchor = find_sky(plate)
        hull = path.stem
        mask.save(OUT / f"{hull}_mask.png", optimize=True)
        fill = sky_fill(plate, mask)
        fill.resize((fill.width // 2, fill.height // 2), Image.BOX).save(
            OUT / f"{hull}_fill.png", optimize=True)
        anchors[hull] = anchor
        print(f"{hull:<11} sky anchor {anchor}  covers {np.mean(np.asarray(mask) > 127):.1%}")
        if debug:
            w = plate.width
            vis = np.asarray(plate, dtype=np.float32)
            m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
            vis = vis * (1 - 0.7 * m) + np.array([0, 170, 255]) * 0.7 * m
            thumb = Image.fromarray(vis.astype(np.uint8))
            d = ImageDraw.Draw(thumb)
            cx, cy, r = anchor
            d.line([0, cy, w, cy], fill=(255, 60, 60), width=5)
            d.text((12, 12), hull, fill=(255, 255, 0))
            thumbs.append(thumb.resize((384, 256)))
    (OUT / "anchors.json").write_text(json.dumps(anchors, indent=1) + "\n")
    if debug and thumbs:
        cols = 6
        sheet = Image.new("RGB", (cols * 384, math.ceil(len(thumbs) / cols) * 256))
        for i, t in enumerate(thumbs):
            sheet.paste(t, ((i % cols) * 384, (i // cols) * 256))
        Path(debug).parent.mkdir(parents=True, exist_ok=True)
        sheet.save(debug)


def bake_stars():
    rng = np.random.default_rng(RNG_SEED + 561)
    stars = SimpleNamespace(
        density=STAR_DENSITY,
        contrast=0.12 + 0.8 * rng.random(STAR_POPULATION) ** 3,        # mostly faint
        tint=np.asarray(STAR_TINTS, np.float32)[
            rng.choice(len(STAR_TINTS), STAR_POPULATION, p=STAR_TINT_ODDS)])
    OUT.mkdir(parents=True, exist_ok=True)
    star_tile(stars, 0.7, (0.32, 0.55), rng).save(OUT / "stars_far.png", optimize=True)
    star_tile(stars, 0.3, (0.45, 0.8), rng).save(OUT / "stars_near.png", optimize=True)
    print("stars: far + near tiles")


def bake_layers():
    """Rendered sky passes -> under-plate, anchored sprite sheets."""
    timing = json.loads(TIMING.read_text())
    for layer, t in timing.items():
        if layer.startswith("_"):
            continue
        src = BUILD / layer
        info = json.loads((src / "pass.json").read_text())
        sprites, slots = [], []
        for path in sorted(src.glob("*.png")):
            baked = load_frame(path)
            if baked is not None:
                sprites.append(baked)
                slots.append(int(path.stem) - 1)          # frame N -> slot N-1
        fps = float(info["fps"])
        period_frames = max(int(info["frames"]), int(math.ceil(t["period"] * fps)))
        write_sheet(OUT, layer, sprites, slots, CANVAS, fps, period_frames,
                    int(round(t["offset"] * fps)), under=True, anchor=info["anchor"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="contact sheet of every detected sky")
    only = ap.add_mutually_exclusive_group()
    only.add_argument("--stars-only", action="store_true", help="just re-make the star tiles")
    only.add_argument("--layers-only", action="store_true", help="just bake the sky layers")
    args = ap.parse_args()
    if not (args.stars_only or args.layers_only):
        bake_skies(args.debug)
    if not args.layers_only:
        bake_stars()
    if not args.stars_only:
        bake_layers()


if __name__ == "__main__":
    main()

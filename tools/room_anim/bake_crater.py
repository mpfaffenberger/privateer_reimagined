#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Find the open sky above the crater in every crater landing composite (#561;
shared in #587 by the mining and pirate bases).

A crater landing pad is an open crater: each of the 18 per-hull composites
(assets/concourse/<base>/landing_ships/<hull>.png, framed differently) shows
a strip of black sky above a jagged rock rim. For every composite this writes
to assets/concourse/<base>/anim/landing/:

    <hull>_mask.png   L8, 255 = open sky
    <hull>_fill.png   the sky colour (small, drawn stretched)
    anchors.json      {"<hull>": [cx, cy, r]}: cy is the rim height, see find_sky()
    stars_far.png, stars_near.png
                      star tiles shared by every hull. The painted sky is
                      starless, so these are a deliberate, sparse addition.
    <layer>.{json,png}
                      each sky layer in <base>/landing_layers.json (rendered by
                      <base>/render_landing.py), under the plate and anchored,
                      so the rim and dish occlude it on every hull.

landing_layers.json's "_star_seed" gives each base its own star field.

Usage (from the repo root):
    uv run tools/room_anim/bake_crater.py --base mining [--debug build/room_anim/mining/skies.png]
    uv run tools/room_anim/bake_crater.py --base pirate --stars-only
    uv run tools/room_anim/bake_crater.py --base pirate --layers-only  # after render_landing.py
"""
import argparse
import json
from types import SimpleNamespace

import numpy as np
from PIL import Image, ImageDraw

from base import paths
from bake_layer import bake_passes
from sky import RNG_SEED, contact_sheet, sky_fill, solidify, star_tile

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


def _sky_level(values, band):
    """The sky's level of `values` (H x W) from its top rows: one number, or
    with `band` px one per column, interpolated between column bands (for a
    sky shaded across the frame)."""
    top = values[:SKY_LEVEL_ROWS]
    overall = float(np.percentile(top, SKY_LEVEL_PERCENTILE))
    if not band:
        return overall
    w = values.shape[1]
    starts = np.arange(0, w, band)
    # A band whose top rows are mostly rock (a wall reaching the frame's top
    # corner) must not lift the level there: at most one margin over overall.
    levels = [min(np.percentile(top[:, s:s + band], SKY_LEVEL_PERCENTILE),
                  overall + ROCK_MARGIN) for s in starts]
    return np.interp(np.arange(w), starts + band / 2, levels)[None, :]


def find_sky(plate, rim_smooth=RIM_SMOOTH, level_band=None, warm_margin=None):
    """-> (mask L image, [cx, cy, r]). The sky is everything above the rim,
    column by column (see ROCK_MARGIN). Options for skies a plain
    brightness margin can't separate (the pirate pad, #587): `level_band`
    measures the sky level per column band, for a sky shaded across the
    frame; `warm_margin` also calls rock anything that much warmer (red
    minus blue) than the sky, for a navy sky over dim brown rock."""
    w, h = plate.size
    rgb = np.asarray(plate, dtype=np.uint8).astype(np.int16)
    peak = rgb.max(axis=2)
    bright = peak > _sky_level(peak, level_band) + ROCK_MARGIN
    if warm_margin is not None:
        warm = rgb[..., 0] - rgb[..., 2]
        bright |= warm > _sky_level(warm, level_band) + warm_margin
    rock = bright[:h - ROCK_RUN + 1].copy()
    for k in range(1, ROCK_RUN):                         # a sustained run, not a speck
        rock &= bright[k:h - ROCK_RUN + 1 + k]
    rim = np.where(rock.any(axis=0), rock.argmax(axis=0), h)   # first rock row per column
    rim = _open_profile(rim, rim_smooth)
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


def landing_paths(base):
    """A crater-style landing pad's inputs and outputs (landing_ships/,
    anim/landing/, build/.../landing/, landing_layers.json with its optional
    "_sky" find_sky() options): what the bake_*() functions take."""
    where = paths(base)
    timing = where.tools / "landing_layers.json"
    return SimpleNamespace(composites=where.room / "landing_ships",
                           out=where.anim / "landing", build=where.build / "landing",
                           timing=timing, sky=json.loads(timing.read_text()).get("_sky", {}))


def bake_skies(where, debug, find=None):
    """Each composite's sky mask, fill and anchor. `find(plate)` -> (mask,
    [cx, cy, r]) replaces find_sky() for a pad whose sky isn't above a rim
    (the Refinery's hangar door, #585)."""
    out = where.out
    out.mkdir(parents=True, exist_ok=True)
    anchors, thumbs = {}, []
    for path in sorted(where.composites.glob("*.png")):
        plate = Image.open(path).convert("RGB")
        mask, anchor = find(plate) if find else find_sky(plate, **where.sky)
        hull = path.stem
        mask.save(out / f"{hull}_mask.png", optimize=True)
        fill = sky_fill(plate, mask)
        fill.resize((fill.width // 2, fill.height // 2), Image.BOX).save(
            out / f"{hull}_fill.png", optimize=True)
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
    (out / "anchors.json").write_text(json.dumps(anchors, indent=1) + "\n")
    if debug:
        contact_sheet(thumbs, debug)


def bake_stars(where):
    out = where.out
    seed = json.loads(where.timing.read_text())["_star_seed"]
    rng = np.random.default_rng(RNG_SEED + seed)
    stars = SimpleNamespace(
        density=STAR_DENSITY,
        contrast=0.12 + 0.8 * rng.random(STAR_POPULATION) ** 3,        # mostly faint
        tint=np.asarray(STAR_TINTS, np.float32)[
            rng.choice(len(STAR_TINTS), STAR_POPULATION, p=STAR_TINT_ODDS)])
    out.mkdir(parents=True, exist_ok=True)
    star_tile(stars, 0.7, (0.32, 0.55), rng).save(out / "stars_far.png", optimize=True)
    star_tile(stars, 0.3, (0.45, 0.8), rng).save(out / "stars_near.png", optimize=True)
    print("stars: far + near tiles")


def bake_layers(where):
    """Rendered sky passes -> under-plate, anchored sprite sheets."""
    bake_passes(where.timing, where.build, where.out, CANVAS)



def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", required=True, help="assets/concourse/<base>")
    ap.add_argument("--debug", help="contact sheet of every detected sky")
    only = ap.add_mutually_exclusive_group()
    only.add_argument("--stars-only", action="store_true", help="just re-make the star tiles")
    only.add_argument("--layers-only", action="store_true", help="just bake the sky layers")
    args = ap.parse_args()
    where = landing_paths(args.base)
    if not (args.stars_only or args.layers_only):
        bake_skies(where, args.debug)
    if not args.layers_only:
        bake_stars(where)
    if not args.stars_only:
        bake_layers(where)


if __name__ == "__main__":
    main()

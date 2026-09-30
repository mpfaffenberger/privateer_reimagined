#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "scipy"]
# ///
"""Find the dusk sky over the sea in every Pleasure landing composite (#595).

The Pleasure landing pad is an open-air pad on a resort world: two towers,
a brick tower block on an island, the sea, and a purple dusk sky with two
moons. Each of the 18 per-hull composites (landing_ships/<hull>.png) is
framed a little differently. For every composite this writes to
assets/concourse/pleasure/anim/landing/:

    <hull>_mask.png   L8, 255 = open sky (moons, towers and the block excluded)
    <hull>_fill.png   the painted sky itself at half size, holes extrapolated
    anchors.json      {"<hull>": [cx, cy, r]}: cy is the sea horizon
    transport_arrival.{json,png}
                      the transport (render_landing.py), under the plate and
                      anchored, so the towers and the block occlude it.

Usage (from the repo root):
    uv run tools/room_anim/pleasure/bake_landing.py [--debug build/room_anim/pleasure/skies.png]
    uv run tools/room_anim/pleasure/bake_landing.py --layers-only    # after render_landing.py
"""
import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bake_layer import load_frame, write_sheet  # noqa: E402
from base import paths  # noqa: E402
from sky import sky_fill  # noqa: E402

PLEASURE = paths("pleasure")
COMPOSITES = PLEASURE.room / "landing_ships"
OUT = PLEASURE.anim / "landing"
BUILD = PLEASURE.build / "landing"
TIMING = PLEASURE.tools / "landing_layers.json"
CANVAS = (1536, 1024)

# The sky and the sea are the same saturated blue-purple, but the sky is a
# smooth gradient and the sea is textured (waves, glints). So sky is taken
# to be blue-purple (see SKY_BLUE; its brightness varies a lot between
# composites) and smooth: within DETAIL of a large-scale median of the plate.
# The moons, the block's brickwork, the tower edges and the sea all fail that.
# Blue over green, not over red: the sunset glow at the horizon is pink
# (dralthi: 196, 113, 224). Drayman's dark top is 27, 32, 98.
SKY_BLUE = 50
SKY_MIN_BLUE = 80
DETAIL = 14
DETAIL_SCALE = 4                # the median runs at 1/4 size...
DETAIL_WINDOW = 15              # ...over 15 px there (60 px of plate)
# The horizon is soft paint, and no single cue finds it everywhere: the sky
# pinkens toward it on most composites but not all, and the far sea is as
# smooth as the sky. Ripples are the safe cue: per open column (sky-blue
# down its first HORIZON_CLEAR rows), the first sustained run of rows whose
# row-to-row change is RIPPLE x the sky's own. Where the far sea is glassy
# that lands a little below the real horizon (up to ~70 px, checked by eye on
# all 18), but never more than a few px above it. render_landing.py keeps
# the ship well above the anchor, so it only ever errs toward open sky, and
# the mask may take in a strip of glassy sea: the fill is the plate's own
# pixels, so that's invisible.
HORIZON_BAND = (360, 700)
HORIZON_CLEAR = 40
RIPPLE = 4.0
MIN_SKY_FRACTION = 0.05


def horizon(rgb, sky_colour):
    """Row of the sea horizon (see HORIZON_BAND): the median over open
    columns of the first rippled run."""
    y0, y1 = HORIZON_BAND
    change = np.abs(rgb[y0:y1] - rgb[y0 + 1:y1 + 1]).sum(axis=2)
    blue = sky_colour[y0:y1] & sky_colour[y0 + 1:y1 + 1]
    rows = []
    for x in range(0, rgb.shape[1], 3):
        if blue[:HORIZON_CLEAR, x].mean() < 0.9:
            continue                                   # a tower or the block is in the way
        t = change[:, x] * blue[:, x]
        hot = t > max(RIPPLE * (np.median(t[:HORIZON_CLEAR]) + 1.0), 6.0)
        run = ndimage.uniform_filter1d(hot.astype(np.float32), 6) > 0.6
        if run.any():
            rows.append(y0 + int(np.argmax(run)))
    if not rows:
        raise ValueError("no open horizon found")
    return float(np.median(rows))


def find_sky(plate):
    """-> (mask L image, [cx, cy, r]). Sky = smooth, blue-purple, above the
    horizon and connected to the top of the frame."""
    rgb = np.asarray(plate, dtype=np.float32)
    w, h = plate.size
    sky_colour = (rgb[..., 2] > SKY_MIN_BLUE) & (rgb[..., 2] - rgb[..., 1] > SKY_BLUE)
    small = plate.resize((w // DETAIL_SCALE, h // DETAIL_SCALE), Image.BOX)
    med = ndimage.median_filter(np.asarray(small, dtype=np.float32),
                                size=(DETAIL_WINDOW, DETAIL_WINDOW, 1))
    model = np.asarray(Image.fromarray(med.astype(np.uint8)).resize((w, h), Image.BILINEAR),
                       dtype=np.float32)
    sky = sky_colour & (np.abs(rgb - model).max(axis=2) < DETAIL)
    sky = ndimage.binary_opening(sky, iterations=2)          # drop specks and seams
    hz = horizon(rgb, sky_colour)
    sky[int(hz) - 1:] = False                                # the sea never leaks in
    labels, _ = ndimage.label(sky)
    sky = np.isin(labels, list(set(np.unique(labels[0])) - {0}))
    sky = ndimage.binary_erosion(sky, iterations=1)          # keep off painted edges
    if sky.mean() < MIN_SKY_FRACTION:
        raise ValueError(f"sky too small ({sky.mean():.2%} of frame)")
    mask = Image.fromarray(sky.astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.8))
    # Framing differs mainly in how high the horizon sits, so the anchor is
    # vertical only (as on the mining pad): layers slide, never rescale.
    return mask, [w / 2, round(hz, 1), w / 2]


def fill(plate, mask):
    """The painted sky itself (it's a smooth gradient, so half size loses
    nothing), with every non-sky pixel extrapolated from around it."""
    m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
    grown = Image.fromarray((m[..., 0] * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(5))
    g = (np.asarray(grown, dtype=np.float32) / 255.0)[..., None]
    rgb = np.asarray(plate, dtype=np.float32)
    ext = np.asarray(sky_fill(plate, mask).resize(plate.size, Image.BILINEAR), dtype=np.float32)
    out = Image.fromarray((rgb * g + ext * (1.0 - g)).astype(np.uint8))
    return out.resize((plate.width // 2, plate.height // 2), Image.BOX)


def bake_skies(debug):
    OUT.mkdir(parents=True, exist_ok=True)
    anchors, thumbs = {}, []
    for path in sorted(COMPOSITES.glob("*.png")):
        plate = Image.open(path).convert("RGB")
        mask, anchor = find_sky(plate)
        hull = path.stem
        mask.save(OUT / f"{hull}_mask.png", optimize=True)
        fill(plate, mask).save(OUT / f"{hull}_fill.png", optimize=True)
        anchors[hull] = anchor
        print(f"{hull:<11} horizon {anchor[1]:6.1f}  sky {np.mean(np.asarray(mask) > 127):.1%}")
        if debug:
            vis = np.asarray(plate, dtype=np.float32)
            m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
            vis = vis * (1 - 0.6 * m) + np.array([0, 255, 120]) * 0.6 * m
            thumb = Image.fromarray(vis.astype(np.uint8))
            d = ImageDraw.Draw(thumb)
            d.line([0, anchor[1], plate.width, anchor[1]], fill=(255, 60, 60), width=4)
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
    ap.add_argument("--layers-only", action="store_true", help="just bake the sky layers")
    args = ap.parse_args()
    if not args.layers_only:
        bake_skies(args.debug)
    if TIMING.exists():
        bake_layers()


if __name__ == "__main__":
    main()

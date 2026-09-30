#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "scipy"]
# ///
"""Chase the red landing lights along the Military landing bay's edge (#589).

The military landing pad is a closed hangar bay: no sky, no mouth, nothing
to fly through. What the original game animated here was its landing
lights: the legacy `landing_lbl` overlay blinked the red lamps set into the
edge of the bay floor. Each of the 17 per-hull composites
(assets/concourse/military/landing_ships/<hull>.png) is painted separately,
so the 3-4 lamps sit somewhere different in each, and the spacing differs
too (no one anchor maps them onto each other). So every composite gets its
own sheet, found and baked from its own paint; the room names them with
{plate} (room_anim_data.cpp for_plate()).

The lamps run a "rabbit" chase from the far end toward the camera: each
flares in turn and fades, lighting a red glow round itself and a spill
along the floor. Between flares they're as painted. The flare is added in
linear light over the composite and encoded with bake_layer.encode(), so
the engine's plain alpha-over reproduces it exactly (see bake_layer.py).

Outputs (assets/concourse/military/anim/landing/):
    <hull>_lights.{png,json}   one looping sheet per composite

Usage (from the repo root):
    uv run tools/room_anim/military/bake_landing.py [--debug build/room_anim/military/lamps.png]
"""
import argparse
import itertools
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from bake_layer import encode, to_linear, to_srgb, write_sheet  # noqa: E402
from sky import contact_sheet  # noqa: E402

MILITARY = paths("military")
COMPOSITES = MILITARY.room / "landing_ships"
OUT = MILITARY.anim / "landing"

# Lamp finding. A lamp is a small red blob below the parked hull's deck
# (y > FLOOR_Y); the ship hulls carry red too (strakha is red all over), so
# only a line of 3+ evenly spaced blobs, sloping down to the right like the
# bay's edge, counts. Gothri's lamps are painted dim and orange: if the
# strict colour test finds no line, a looser one tries again.
FLOOR_Y = 560
REDNESS = ((80, 1.8, 1.8), (60, 1.35, 1.9))    # (min R, R/G, R/B): strict, loose
BLOB_PX = (4, 500)
MERGE_PX = 45                                  # satellites of one lamp merge
SLOPE = (0.25, 0.6)                            # dy/dx of the bay edge
GAP = (140, 380)                               # px between neighbouring lamps
ON_LINE = 14                                   # px off the fitted line

# The chase.
FPS = 12
STEP = 0.35                                    # s between lamps
CYCLE = 3.0                                    # s per chase, with a rest at the end
ATTACK, DECAY = 0.08, 0.14                     # s: flare up, then e-folding fade;
                                               # at most two neighbours glow at once
# The painted lamps range from specks to smudges (drayman's are 10 px across
# with their painted bloom); flares scale with the lamp, within reason.
LAMP_R = (2.5, 5.0)                            # px
LAMP_RED = np.array([1.0, 0.16, 0.06])         # linear
# Glow in linear light, per unit flare, as gaussians scaled by the lamp's
# painted radius: a hot core, a halo, and a spill flattened along the floor
# (seen at a grazing angle).
CORE, HALO, SPILL = (1.2, 1.4), (4.0, 0.3), ((10.0, 3.0), 0.12)   # (sigma(s) in radii, gain)


def _blobs(plate, strictness):
    min_r, rg, rb = strictness
    p = plate.astype(np.int32)
    red = (p[..., 0] > min_r) & (p[..., 0] > rg * p[..., 1]) & (p[..., 0] > rb * p[..., 2])
    red[:FLOOR_Y] = False
    lab, n = ndimage.label(red)
    sizes = ndimage.sum(red, lab, range(1, n + 1))
    found = []
    for i, size in enumerate(sizes, start=1):
        if BLOB_PX[0] <= size <= BLOB_PX[1]:
            cy, cx = ndimage.center_of_mass(red, lab, i)
            found.append((cx, cy, float(size)))
    kept = []
    for b in sorted(found, key=lambda b: -b[2]):
        if all(math.hypot(b[0] - k[0], b[1] - k[1]) > MERGE_PX for k in kept):
            kept.append(b)
    return kept


def _chain(on):
    """Left to right, dropping the smaller of any two lamps closer than GAP."""
    chain = []
    for b in sorted(on):
        if chain and b[0] - chain[-1][0] < GAP[0]:
            if b[2] > chain[-1][2]:
                chain[-1] = b
            continue
        chain.append(b)
    return chain


def find_lamps(plate):
    """-> [(x, y, radius px)] along the bay edge, far (left) to near, or []."""
    for strictness in REDNESS:
        blobs = _blobs(plate, strictness)
        best = None
        for a, b in itertools.combinations(blobs, 2):
            (x0, y0, _), (x1, y1, _) = sorted((a, b))
            if x1 - x0 < GAP[0]:
                continue
            k = (y1 - y0) / (x1 - x0)
            if not SLOPE[0] <= k <= SLOPE[1]:
                continue
            off = [abs(q[1] - (y0 + k * (q[0] - x0))) for q in blobs]
            chain = _chain([q for q, d in zip(blobs, off) if d < ON_LINE])
            gaps = np.diff([q[0] for q in chain])
            if len(chain) < 3 or gaps.max() > GAP[1]:
                continue
            score = (len(chain), -sum(d for d in off if d < ON_LINE))
            if best is None or score > best[0]:
                best = (score, chain)
        if best:
            return [(x, y, min(max(math.sqrt(size / math.pi), LAMP_R[0]), LAMP_R[1]))
                    for x, y, size in best[1]]
    return []


def flare(t):
    """Flare 0..1 of a lamp `t` s after its cue (quick rise, exponential fade)."""
    if t < 0:
        return 0.0
    if t < ATTACK:
        return t / ATTACK
    return math.exp(-(t - ATTACK) / DECAY)


def glow(shape, box, lamp):
    """Linear-light glow of one lamp at flare 1 over the plate crop `box`."""
    x0, y0, x1, y1 = box
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    cx, cy, r = lamp
    dx, dy = xx - cx, yy - cy
    g = np.zeros((y1 - y0, x1 - x0), np.float32)
    for (sx, sy), gain in (((CORE[0], CORE[0]), CORE[1]), ((HALO[0], HALO[0]), HALO[1]),
                           SPILL):
        g += gain * np.exp(-0.5 * ((dx / (sx * r)) ** 2 + (dy / (sy * r)) ** 2))
    return g[..., None] * LAMP_RED


def reach(lamp):
    """Half-size of the box a lamp's glow is visible in (3 sigma of the spill)."""
    return 3.0 * SPILL[0][0] * lamp[2], 3.0 * SPILL[0][1] * lamp[2]


def bake_plate(path):
    plate8 = np.asarray(Image.open(path).convert("RGB"))
    lamps = find_lamps(plate8)
    if not lamps:
        raise SystemExit(f"{path.stem}: no line of landing lamps found")
    plate = plate8.astype(np.float32) / 255.0
    h, w = plate.shape[:2]
    frames = round(CYCLE * FPS)
    sprites, slots = [], []
    for f in range(frames):
        t = f / FPS
        lit = [(lamp, flare(t - i * STEP)) for i, lamp in enumerate(lamps)]
        lit = [(lamp, e) for lamp, e in lit if e > 0.02]
        if not lit:
            continue
        boxes = []
        for lamp, _ in lit:
            rx, ry = reach(lamp)
            boxes.append((lamp[0] - rx, lamp[1] - ry, lamp[0] + rx, lamp[1] + ry))
        box = (max(0, int(min(b[0] for b in boxes))), max(0, int(min(b[1] for b in boxes))),
               min(w, int(max(b[2] for b in boxes)) + 1), min(h, int(max(b[3] for b in boxes)) + 1))
        x0, y0, x1, y1 = box
        crop = plate[y0:y1, x0:x1]
        lin = to_linear(crop)
        for lamp, e in lit:
            lin = lin + e * glow(crop.shape, box, lamp)
        rgba = encode(crop, to_srgb(lin))
        ys, xs = np.nonzero(rgba[..., 3])
        if xs.size == 0:
            continue
        tx0, ty0, tx1, ty1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
        sprites.append((rgba[ty0:ty1, tx0:tx1], [x0 + int(tx0), y0 + int(ty0),
                                                  int(tx1 - tx0), int(ty1 - ty0)]))
        slots.append(f)
    write_sheet(OUT, f"{path.stem}_lights", sprites, slots, (w, h), FPS, frames, 0)
    return lamps


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="contact sheet of the lamps found on every composite")
    args = ap.parse_args()
    thumbs = []
    for path in sorted(COMPOSITES.glob("*.png")):
        lamps = bake_plate(path)
        print(f"{path.stem:<11} {len(lamps)} lamps: "
              + ", ".join(f"({x:.0f}, {y:.0f})" for x, y, _ in lamps))
        if args.debug:
            im = Image.open(path).convert("RGB")
            d = ImageDraw.Draw(im)
            for x, y, r in lamps:
                d.ellipse([x - 4 * r, y - 4 * r, x + 4 * r, y + 4 * r], outline=(0, 255, 255), width=4)
            d.text((12, 12), path.stem, fill=(255, 255, 0))
            thumbs.append(im.resize((384, 256)))
    if args.debug:
        contact_sheet(thumbs, args.debug)


if __name__ == "__main__":
    main()

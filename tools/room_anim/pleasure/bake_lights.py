#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "scipy"]
# ///
"""Relight the Pleasure concourse's painted lamps: marquee chase and a neon stutter (#594).

No Blender: the lamps are already in the painting, so each frame is the plate
with some of its own light taken away (in linear light), encoded against the
plate like any other layer (bake_layer.encode / write_sheet).

    marquee_left / marquee_right   theatre-marquee chase along the bulb rows
                                   of both canopies: every third bulb dims,
                                   and the gap marches toward the back.
    neon_flicker                   the big red neon sign stutters, like a
                                   tired tube, once per loop.

Bulbs are found in the plate (warm, bright cores inside each canopy's
outline) and chained into rows: each bulb's successor is its nearest
neighbour heading for the canopy's vanishing point. A bulb's place in its
row sets its chase phase, so the pattern runs along the rows.

Usage (from the repo root):
    uv run tools/room_anim/pleasure/bake_lights.py [--debug build/room_anim/pleasure/bulbs.png]
"""
import argparse
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bake_layer import bbox, encode, load_rgba, to_linear, to_srgb, write_sheet  # noqa: E402
from base import paths  # noqa: E402

PLEASURE = paths("pleasure")

# Canopy underside outlines (plate px) and the point their bulb rows run to.
CANOPIES = {
    "marquee_left": ([(0, 80), (175, 80), (525, 525), (490, 552), (0, 410)], (600.0, 585.0)),
    "marquee_right": ([(1240, 535), (1536, 440), (1536, 516), (1265, 556)], (800.0, 690.0)),
}
CHASE_FPS = 4.0          # steps per second
CHASE_PHASES = 3         # every third bulb is dimmed
BULB_DIM = 0.25          # linear-light multiplier for a dimmed bulb and its glow
ROW_ANGLE = math.radians(15.0)   # a row's next bulb lies within this of the VP direction

# The red neon sign on the canopy fascia: its outline, and the stutter.
NEON_OUTLINE = [(330, 0), (570, 0), (625, 330), (610, 480), (520, 480), (330, 120)]
NEON_FPS = 12.0
NEON_PERIOD_S = 9.0
# Brightness per frame of the stutter, starting NEON_AT_S into the loop.
# Every other frame of the loop shows the painted (lit) sign.
NEON_STUTTER = [0.15, 1.0, 0.15, 0.1, 0.6, 1.0, 0.3, 0.1, 0.1, 0.5, 0.85]
NEON_AT_S = 6.0


def outline_mask(size, poly):
    img = Image.new("L", size, 0)
    ImageDraw.Draw(img).polygon(poly, fill=255)
    return np.asarray(img) > 0


def find_bulbs(plate, inside):
    """-> [(cx, cy, r)]: warm, near-white cores inside `inside`, fragments
    of one bulb merged."""
    rgb = plate * 255.0
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    lum = 0.299 * r + 0.587 * g + 0.114 * b
    core = (lum > 215) & (r >= g) & (g > b + 10) & inside
    labels, _ = ndimage.label(core)
    blobs = []
    for i, box in enumerate(ndimage.find_objects(labels)):
        blob = labels[box] == i + 1
        if blob.sum() < 4:
            continue
        cy, cx = ndimage.center_of_mass(blob)
        blobs.append((cx + box[1].start, cy + box[0].start, math.sqrt(blob.sum() / math.pi)))
    kept = []
    for cx, cy, rad in sorted(blobs, key=lambda k: -k[2]):
        # Glints on a lamp's housing sit just above its bulb: part of it.
        if all(math.hypot(cx - kx, cy - ky) > 2.2 * kr + 3 for kx, ky, kr in kept):
            kept.append((cx, cy, rad))
    return kept


def chase_phases(bulbs, vp):
    """Chain bulbs into rows running toward `vp`; -> phase per bulb."""
    nxt = {}
    for i, (x, y, rad) in enumerate(bulbs):
        to_vp = math.atan2(vp[1] - y, vp[0] - x)
        best = None
        for j, (x2, y2, _) in enumerate(bulbs):
            dist = math.hypot(x2 - x, y2 - y)
            turn = abs((math.atan2(y2 - y, x2 - x) - to_vp + math.pi) % (2 * math.pi) - math.pi)
            if j != i and dist < max(9 * rad, 30) and turn < ROW_ANGLE and \
                    (best is None or dist < best[0]):
                best = (dist, j)
        if best:
            nxt[i] = best
    taken = {}                       # two bulbs claiming one successor: the nearer wins
    for i, (dist, j) in nxt.items():
        if j not in taken or dist < nxt[taken[j]][0]:
            taken[j] = i
    nxt = {i: j for j, i in taken.items()}
    phase = {}
    for root in (i for i in range(len(bulbs)) if i not in taken):
        i, k = root, 0
        while i is not None and i not in phase:
            phase[i] = k % CHASE_PHASES
            i, k = nxt.get(i), k + 1
    return [phase.get(i, 0) for i in range(len(bulbs))]


def glow_weight(shape, bulb):
    """How much of a pixel's light is this bulb's: its core, and the warm
    cone it throws up onto the canopy above it."""
    cx, cy, rad = bulb
    h, w = shape
    x0, x1 = max(0, int(cx - 4 * rad) - 4), min(w, int(cx + 4 * rad) + 5)
    y0, y1 = max(0, int(cy - 8 * rad) - 4), min(h, int(cy + 3 * rad) + 5)
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    sx = 1.4 * rad + 1.0
    sy = np.where(yy < cy, 3.0 * rad + 2.0, 1.4 * rad + 1.0)
    weight = np.exp(-(((xx - cx) / sx) ** 2 + ((yy - cy) / sy) ** 2))
    return np.s_[y0:y1, x0:x1], np.clip(weight * 1.3, 0.0, 1.0)


def relit(plate_lin, dims):
    """Plate with light removed: `dims` is a per-pixel multiplier (linear)."""
    return to_srgb(plate_lin * dims[..., None])


def sprite(plate, target):
    """Plate-aware RGBA over the whole canvas, trimmed -> (sprite, dst)."""
    rgba = encode(plate, target)
    box = bbox(rgba[..., 3])
    if box is None:
        return None
    x0, y0, x1, y1 = box
    return rgba[y0:y1, x0:x1], [x0, y0, x1 - x0, y1 - y0]


def bake_marquee(plate, name, outline, vp, debug):
    size = (plate.shape[1], plate.shape[0])
    bulbs = find_bulbs(plate, outline_mask(size, outline))
    phases = chase_phases(bulbs, vp)
    plate_lin = to_linear(plate)
    sprites = []
    for step in range(CHASE_PHASES):
        dims = np.ones(plate.shape[:2], dtype=np.float32)
        for bulb, phase in zip(bulbs, phases):
            if phase == step:
                where, weight = glow_weight(plate.shape[:2], bulb)
                dims[where] *= 1.0 - (1.0 - BULB_DIM) * weight
        sprites.append(sprite(plate, relit(plate_lin, dims)))
    write_sheet(PLEASURE.anim, name, sprites, list(range(CHASE_PHASES)), size, CHASE_FPS,
                CHASE_PHASES, 0)
    if debug is not None:
        draw = ImageDraw.Draw(debug)
        for (cx, cy, rad), phase in zip(bulbs, phases):
            colour = [(255, 60, 60), (60, 255, 60), (60, 140, 255)][phase]
            draw.ellipse([cx - rad - 2, cy - rad - 2, cx + rad + 2, cy + rad + 2], outline=colour,
                         width=2)
    print(f"{name}: {len(bulbs)} bulbs")


def neon_weight(plate, outline):
    """0..1 share of each pixel's light that is the red neon: the tubes'
    red, their near-white cores, and the red halo on the fascia."""
    rgb = plate * 255.0
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    red = np.clip((r - np.maximum(g, b) - 10.0) / 60.0, 0.0, 1.0)
    cores = ndimage.binary_dilation(red > 0.5, iterations=3) & (r > 200)
    return np.where(outline, np.maximum(red, cores.astype(np.float32)), 0.0)


def bake_neon(plate):
    size = (plate.shape[1], plate.shape[0])
    weight = neon_weight(plate, outline_mask(size, NEON_OUTLINE))
    plate_lin = to_linear(plate)
    start = round(NEON_AT_S * NEON_FPS)
    sprites, slots = [], []
    for k, level in enumerate(NEON_STUTTER):
        if level >= 1.0:
            continue                    # lit: the painted sign shows
        baked = sprite(plate, relit(plate_lin, 1.0 - (1.0 - level) * weight))
        if baked is not None:
            sprites.append(baked)
            slots.append(start + k)
    write_sheet(PLEASURE.anim, "neon_flicker", sprites, slots, size, NEON_FPS,
                round(NEON_PERIOD_S * NEON_FPS), 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="write the found bulbs, coloured by chase phase, here")
    args = ap.parse_args()
    plate = load_rgba(PLEASURE.plate)[..., :3]
    debug = Image.open(PLEASURE.plate).convert("RGB") if args.debug else None
    for name, (outline, vp) in CANOPIES.items():
        bake_marquee(plate, name, outline, vp, debug)
    bake_neon(plate)
    if debug is not None:
        Path(args.debug).parent.mkdir(parents=True, exist_ok=True)
        debug.save(args.debug)


if __name__ == "__main__":
    main()

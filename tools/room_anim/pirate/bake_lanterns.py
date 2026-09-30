#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Flickering lanterns for the pirate concourse (#586). No Blender: a
lantern's flicker only changes how bright the painting already is around it.

Each lantern is a centre and a glow radius on the plate. Per frame the plate
is scaled in linear light by 1 + (m(t) - 1) * g, where g is a Gaussian
around the lantern (the glass and the rock it lights) and m(t) the flame's
brightness: a gentle wobble with the odd gutter, built from whole cycles per
loop so the loop is seamless. bake_layer.encode() turns each frame into a
minimal-alpha sprite over the plate, so the engine's plain alpha-over
reproduces it exactly.

    uv run tools/room_anim/pirate/bake_lanterns.py
"""
import math
import random
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from bake_layer import bbox, encode, load_rgba, to_linear, to_srgb, write_sheet  # noqa: E402
from base import paths  # noqa: E402

FPS = 12.0
PERIOD_S = 9.0

# name -> [(centre x, centre y, glow sigma px), ...], seed. Plate pixels,
# measured on the painting: the cage lantern hanging top left, and the
# two small lamps further down the left wall.
LANTERNS = {
    "lantern_hanging": ([(288.0, 230.0, 40.0)], 586),
    "lanterns_wall": ([(373.0, 376.0, 14.0), (402.0, 392.0, 12.0)], 5860),
}


def flame(frames, seed):
    """-> m per frame, ~0.72-1.06: a seamless wobble plus two gutters."""
    rng = random.Random(seed)
    waves = [(rng.choice(range(k, k + 4)), rng.uniform(0, 2 * math.pi), amp)
             for k, amp in ((3, 0.035), (11, 0.025), (23, 0.02), (41, 0.012))]
    gutters = [(rng.uniform(0.1, 0.9), rng.uniform(0.18, 0.28)) for _ in range(2)]
    m = []
    for i in range(frames):
        t = i / frames
        v = 1.0 + sum(a * math.sin(2 * math.pi * n * t + ph) for n, ph, a in waves)
        for at, depth in gutters:              # a quick dip and recovery
            d = min(abs(t - at), 1 - abs(t - at)) * PERIOD_S
            v -= depth * math.exp(-(d / 0.18) ** 2)
        m.append(v)
    return m


def bake(where, name, lamps, seed, plate):
    h, w = plate.shape[:2]
    pad = max(3.0 * s for _, _, s in lamps)
    x0 = int(max(0, min(x for x, _, _ in lamps) - pad))
    x1 = int(min(w, max(x for x, _, _ in lamps) + pad))
    y0 = int(max(0, min(y for _, y, _ in lamps) - pad))
    y1 = int(min(h, max(y for _, y, _ in lamps) + pad))
    crop = plate[y0:y1, x0:x1]
    ys, xs = np.mgrid[y0:y1, x0:x1] + 0.5
    glow = np.zeros(crop.shape[:2], dtype=np.float32)
    for cx, cy, sigma in lamps:
        glow = np.maximum(glow, np.exp(-((xs - cx) ** 2 + (ys - cy) ** 2) / (2 * sigma ** 2)))
    lin = to_linear(crop)
    frames = int(round(PERIOD_S * FPS))
    sprites, slots = [], []
    for slot, m in enumerate(flame(frames, seed)):
        rgba = encode(crop, to_srgb(lin * (1.0 + (m - 1.0) * glow[..., None])))
        box = bbox(rgba[..., 3])
        if box is None:
            continue
        bx0, by0, bx1, by1 = box
        sprites.append((rgba[by0:by1, bx0:bx1], [x0 + bx0, y0 + by0, bx1 - bx0, by1 - by0]))
        slots.append(slot)
    write_sheet(where.anim, name, sprites, slots, (w, h), FPS, frames, 0)


def main():
    where = paths("pirate")
    plate = load_rgba(where.plate)[..., :3]
    for name, (lamps, seed) in LANTERNS.items():
        bake(where, name, lamps, seed, plate)


if __name__ == "__main__":
    main()

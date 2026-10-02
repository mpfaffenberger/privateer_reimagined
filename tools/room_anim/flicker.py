"""Lights that flicker or blink, straight from a plate (#586; shared since
#676). No Blender: a light only changes how bright the painting already is
around it.

Each light is a centre and a glow radius on the plate. Per frame the plate
is scaled in linear light by 1 + (m(t) - 1) * g, where g is a Gaussian
around the light (the glass and what it lights) and m(t) its brightness, one
seamless loop of levels: flame() for a lantern (a gentle wobble with the odd
gutter), blink() for a beacon. bake_layer.encode() turns each frame into a
minimal-alpha sprite over the plate, so the engine's plain alpha-over
reproduces it exactly.

Used by pirate/bake_lanterns.py (the concourse) and
newdetroit/bake_bar_lights.py (the bar).
"""
import math
import random

import numpy as np

from bake_layer import bbox, encode, to_linear, to_srgb, write_sheet

FPS = 12.0
PERIOD_S = 9.0          # a flame's loop


def flame(seed):
    """-> m per frame over PERIOD_S, ~0.72-1.06: a seamless wobble plus two
    gutters."""
    frames = int(round(PERIOD_S * FPS))
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


def blink(period_s, on_s, low, start_s=0.0):
    """-> m per frame over `period_s`: a beacon, lit (1.0, as painted) for
    `on_s` from `start_s`, else down at `low`. Each edge eases over a
    frame or two, like a warm filament, not a strobe."""
    frames = int(round(period_s * FPS))
    m = []
    for i in range(frames):
        t = (i / FPS - start_s) % period_s            # seconds since it lit
        on = min(1.0, t / 0.15, max(0.0, (on_s - t) / 0.25))
        m.append(low + (1.0 - low) * max(0.0, on))
    return m


def bake(out_dir, name, lamps, levels, plate):
    """One light layer, <out_dir>/<name>.{json,png}: `lamps` [(centre x,
    centre y, glow sigma), ...] in plate px share one loop of `levels`."""
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
    sprites, slots = [], []
    for slot, m in enumerate(levels):
        rgba = encode(crop, to_srgb(lin * (1.0 + (m - 1.0) * glow[..., None])))
        box = bbox(rgba[..., 3])
        if box is None:
            continue
        bx0, by0, bx1, by1 = box
        sprites.append((rgba[by0:by1, bx0:bx1], [x0 + bx0, y0 + by0, bx1 - bx0, by1 - by0]))
        slots.append(slot)
    write_sheet(out_dir, name, sprites, slots, (w, h), FPS, len(levels), 0)

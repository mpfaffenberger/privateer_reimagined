#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Anchor and bake the New Detroit landing pad's aircars (#591).

The landing pad is 18 per-hull composites (landing_ships/<hull>.png), and
each frames the pad a little differently: up to ~4% zoom and ~40 px shift.
The aircars (render_landing.py) are rendered once, against tarsus, so each
composite needs a similarity (zoom + shift) from tarsus. register() finds it
from the pad and the hangar: for every zoom it phase-correlates the
composite's edges against tarsus's to get the shift, then keeps the zoom
whose aligned edges correlate best. Only the hangar block and the pad's near
rim are scored (PAD): the parked ship (a different hull on every composite;
the Stiletto's wings reach the hangar ribs) and the city (painted afresh
for each) would only add noise.

Writes to assets/concourse/newdetroit/anim/landing/:
    anchors.json         {"<hull>": [cx, cy, r]}: tarsus's centre as seen on
                         that composite, and r = 100 x its zoom
    aircar_*.{json,png}  the passes, anchored, drawn over the plate

Usage (from the repo root):
    uv run tools/room_anim/newdetroit/bake_landing.py [--debug build/room_anim/newdetroit/reg.png]
    uv run tools/room_anim/newdetroit/bake_landing.py --layers-only   # after render_landing.py
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from bake_layer import bake_passes  # noqa: E402
from sky import contact_sheet  # noqa: E402

NEWDETROIT = paths("newdetroit")
COMPOSITES = NEWDETROIT.room / "landing_ships"
OUT = NEWDETROIT.anim / "landing"
BUILD = NEWDETROIT.build / "landing"
TIMING = NEWDETROIT.tools / "landing_layers.json"
CANVAS = (1536, 1024)
REFERENCE = "tarsus"               # the composite render_landing.py frames
ANCHOR_R = 100.0                   # tarsus's anchor radius; only ratios matter

DS = 2                             # register at half resolution
ZOOMS = np.arange(0.88, 1.0801, 0.005)
# x0, y0, x1, y1 (fractions of tarsus): the hangar block, the pad's near rim
PAD = ((0.45, 0.0, 1.0, 0.35), (0.10, 0.72, 0.80, 0.88))


def _edges(path):
    im = Image.open(path).convert("L")
    g = np.asarray(im.resize((im.width // DS, im.height // DS), Image.BOX), np.float32)
    gx, gy = np.zeros_like(g), np.zeros_like(g)
    gx[:, 1:-1], gy[1:-1] = g[:, 2:] - g[:, :-2], g[2:] - g[:-2]
    return np.hypot(gx, gy)


def _zoomed(img, k):
    """img sampled at k (p - c) + c: the composite, zoomed k, in tarsus's frame."""
    h, w = img.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    sx = np.clip(np.round(k * (xs - w / 2) + w / 2).astype(int), 0, w - 1)
    sy = np.clip(np.round(k * (ys - h / 2) + h / 2).astype(int), 0, h - 1)
    return img[sy, sx]


def _shift(ref, img):
    """(dy, dx) with img(p + d) ~ ref(p), by phase correlation."""
    f = np.fft.fft2(img) * np.conj(np.fft.fft2(ref))
    corr = np.fft.ifft2(f / (np.abs(f) + 1e-6)).real
    dy, dx = np.unravel_index(np.argmax(corr), corr.shape)
    h, w = corr.shape
    return (dy - h if dy > h // 2 else dy), (dx - w if dx > w // 2 else dx)


def register(ref, img, keep):
    """-> (zoom k, (tx, ty) px at full res, score): composite point
    q = k (p - c) + c + t for tarsus point p."""
    best = None
    for k in ZOOMS:
        z = _zoomed(img, k)
        dy, dx = _shift(ref * keep, z * keep)
        aligned = np.roll(z, (-dy, -dx), axis=(0, 1))
        m = keep > 0
        score = np.corrcoef(ref[m], aligned[m])[0, 1]
        if best is None or score > best[2]:
            best = (float(k), (float(k * dx * DS), float(k * dy * DS)), float(score))
    return best


def bake_anchors(debug):
    ref = _edges(COMPOSITES / f"{REFERENCE}.png")
    h, w = ref.shape
    keep = np.zeros_like(ref)
    for x0, y0, x1, y1 in PAD:
        keep[int(h * y0):int(h * y1), int(w * x0):int(w * x1)] = 1.0
    anchors, thumbs = {}, []
    cx, cy = CANVAS[0] / 2, CANVAS[1] / 2
    for path in sorted(COMPOSITES.glob("*.png")):
        k, (tx, ty), score = register(ref, _edges(path), keep)
        anchors[path.stem] = [round(cx + tx, 1), round(cy + ty, 1), round(ANCHOR_R * k, 2)]
        print(f"{path.stem:<11} zoom {k:.3f}  shift ({tx:+.0f}, {ty:+.0f}) px  corr {score:.2f}")
        if debug:
            thumbs.append(_overlap(path, k, tx, ty))
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "anchors.json").write_text(json.dumps(anchors, indent=1) + "\n")
    if debug:
        contact_sheet(thumbs, debug)


def _overlap(path, k, tx, ty):
    """Debug: tarsus in red, the composite (mapped into tarsus's frame) in
    cyan. Where the registration holds, the pad and hangar are grey."""
    ref = np.asarray(Image.open(COMPOSITES / f"{REFERENCE}.png").convert("L"), np.float32)
    img = np.asarray(Image.open(path).convert("L"), np.float32)
    h, w = img.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    sx = np.clip(np.round(k * (xs - w / 2) + w / 2 + tx).astype(int), 0, w - 1)
    sy = np.clip(np.round(k * (ys - h / 2) + h / 2 + ty).astype(int), 0, h - 1)
    mapped = img[sy, sx]
    rgb = np.stack([ref, mapped, mapped], axis=2).astype(np.uint8)
    return Image.fromarray(rgb).resize((w // 4, h // 4), Image.BOX)


def bake_layers():
    """Rendered aircar passes -> anchored sprite sheets over the plate."""
    bake_passes(TIMING, BUILD, OUT, CANVAS, under=False,
                anchor=[CANVAS[0] / 2, CANVAS[1] / 2, ANCHOR_R])     # tarsus, by definition



def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="contact sheet of every registration")
    ap.add_argument("--layers-only", action="store_true", help="just bake the aircar layers")
    args = ap.parse_args()
    if not args.layers_only:
        bake_anchors(args.debug)
        if not TIMING.exists() or not all((BUILD / k).exists() for k in
                                          json.loads(TIMING.read_text()) if k[0] != "_"):
            return
    bake_layers()


if __name__ == "__main__":
    main()

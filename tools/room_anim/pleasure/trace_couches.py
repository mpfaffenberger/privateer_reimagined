#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Trace the painted couches' skyline for the Pleasure concourse walkers (#598).

The walkers pass behind three round red couches, so each couch group's
outline on the plate becomes a holdout (scene.py stands it in front of the
walkers' lanes). Nothing walks in front of a couch, so only the top edge
matters: per plate column, the topmost velvet pixel, filled down to the
bottom of the plate.

* The velvet is saturated deep red, and its lit rims trace the couch tops
  and the round back pods crisply against the gold wall and the dim carpet
  behind them (the carpet in front matches too, but it's below every top).
* A top must stand on a solid column down to the couch bases: velvet or a
  pod's near-black face all the way. The arch's red-lit jamb and the bar
  stools beyond it are red too, but gold shows between them.
* A pod's dark face and the seams between its cushions don't match, so the
  skyline is opened (SPIKE_PX: a red-lit sliver of pillar above a pod, a
  bar stool seen through the arch beside the left couch) and
  then closed (CLOSE_PX): notches narrower than that fill, the open floor
  between the couches doesn't.
* Three spans the colours can't read are measured by hand (FIXES, at 8x).
* Each run of columns becomes one polygon, simplified to SIMPLIFY_PX.

The brass side tables and their candles are hand-traced in scene.py: under
a table top there's a post and open floor, not a solid edge to fill down.

    uv run tools/room_anim/pleasure/trace_couches.py --debug build/room_anim/pleasure/couches.png
"""
import argparse
import json
import sys
from itertools import pairwise
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from base import paths  # noqa: E402

PLEASURE = paths("pleasure")
OUT = PLEASURE.tools / "couches.json"
COLUMNS = (458, 1330)       # left couch's outer edge to the front-right couch's
BAND = (740, 829)           # every couch top is in here; the carpet in front isn't
BASE_Y = 835                # a couch is solid from its top down to here
SOLID = 0.9                 # share of that column that must be velvet or near-black
SEAM_PX = 2                 # a gap this narrow in the skyline is a seam, not floor
SPIKE_PX = 11               # removes slivers narrower than this (pods are 40+ px)
CLOSE_PX = 31               # fills pod faces and cushion seams (the widest is ~25 px)
SIMPLIFY_PX = 0.75
# Hand-measured tops, (x, y) polylines that replace the trace over their span.
FIXES = [
    # The left couch's seat ring: bar stools seen through the arch are red
    # and dark above it, and the side table's glow sits on it.
    [(458, 806), (470, 803), (546, 800)],
    # Back pods whose faces are too dark to read as velvet: the centre
    # couch's shadowed right shoulder, the front-right group's whole pod.
    [(962, 768), (978, 768), (982, 771), (984, 776)],
    [(1083, 768), (1086, 764), (1127, 764), (1131, 768)],
]


def velvet(rgb):
    """Smoothed 0..1 share of deep-red velvet around each pixel."""
    r, g, b = (rgb[..., c].astype(np.float32) for c in range(3))
    red = ((r > 2.4 * (g + 4.0)) & (r > 40.0) & (b < 0.9 * r)).astype(np.float32)
    pad = np.pad(red, 1, mode="edge")
    return sum(pad[dy:dy + red.shape[0], dx:dx + red.shape[1]]
               for dy in range(3) for dx in range(3)) / 9.0


def skyline(rgb, share):
    """{x: top y} over COLUMNS: the first velvet pixel in BAND with velvet
    below it (a speck on the wall isn't a couch) and a solid column under
    that down to BASE_Y."""
    solid = (share >= 0.5) | (rgb.max(axis=2) < 45)
    top = {}
    for x in range(*COLUMNS):
        col = share[:, x]
        for y in range(*BAND):
            if (col[y] >= 0.5 and col[y:y + 5].mean() >= 0.5
                    and solid[y:BASE_Y, x].mean() >= SOLID):
                top[x] = y
                break
    for fix in FIXES:
        for (xa, ya), (xb, yb) in pairwise(fix):
            for x in range(xa, xb):
                top[x] = round(ya + (yb - ya) * (x - xa) / (xb - xa))
    return top


def runs(top):
    """Columns of `top` with no gap wider than SEAM_PX, as lists of x. A
    seam column takes the lower of its neighbours' tops (closing raises it
    if the couch is higher around it)."""
    out = []
    for x in sorted(top):
        if out and x - out[-1][-1] <= SEAM_PX + 1:
            prev = out[-1][-1]
            for seam in range(prev + 1, x):
                top[seam] = max(top[prev], top[x])
            out[-1] += list(range(prev + 1, x + 1))
        else:
            out.append([x])
    return out


def _window(vals, width, pick):
    h = width // 2
    return [pick(vals[max(0, i - h):i + h + 1]) for i in range(len(vals))]


def clean(ys):
    """1-D morphology on a skyline (smaller y is higher). Opening (lower,
    then raise) drops spikes narrower than SPIKE_PX; closing (raise, then
    lower) fills notches narrower than CLOSE_PX."""
    opened = _window(_window(ys, SPIKE_PX, max), SPIKE_PX, min)
    return _window(_window(opened, CLOSE_PX, min), CLOSE_PX, max)


def simplify(points, tol=SIMPLIFY_PX):
    """Douglas-Peucker on an open polyline."""
    if len(points) < 3:
        return points
    (x0, y0), (x1, y1) = points[0], points[-1]
    dx, dy = x1 - x0, y1 - y0
    norm = max(np.hypot(dx, dy), 1e-9)
    dist = [abs(dy * (x - x0) - dx * (y - y0)) / norm for x, y in points[1:-1]]
    i = int(np.argmax(dist)) + 1
    if dist[i - 1] <= tol:
        return [points[0], points[-1]]
    return simplify(points[:i + 1], tol)[:-1] + simplify(points[i:], tol)


def polygons(top, bottom):
    """One plate-px polygon per run: the skyline (pixel-edge corners, so a
    column x covers [x, x+1)), closed down to `bottom`."""
    out = []
    for xs in runs(top):
        ys = clean([top[x] for x in xs])
        edge = []
        for x, y in zip(xs, ys):
            edge += [(x, y), (x + 1, y)]
        edge = simplify(edge)
        out.append([(xs[0], bottom), *edge, (xs[-1] + 1, bottom)])
    return out


def debug_sheet(rgb, polys, out_png):
    """The plate with every couch polygon tinted, 2x around the couches."""
    from PIL import ImageDraw
    img = Image.fromarray(rgb).convert("RGBA")
    tint = Image.new("RGBA", img.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(tint)
    for poly in polys:
        draw.polygon(poly, fill=(0, 170, 255, 110), outline=(0, 255, 255, 255))
    img = Image.alpha_composite(img, tint).crop((COLUMNS[0] - 30, 720, COLUMNS[1] + 30, 900))
    img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(out_png)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", type=Path, help="write a tinted overlay of the polygons here")
    args = ap.parse_args()
    rgb = np.asarray(Image.open(PLEASURE.plate).convert("RGB"))
    polys = polygons(skyline(rgb, velvet(rgb)), rgb.shape[0])
    OUT.write_text(json.dumps({
        "_doc": "Couch-group holdout polygons, plate px (x, y); written by trace_couches.py.",
        "couches": [[list(p) for p in poly] for poly in polys],
    }, indent=None).replace("]], [[", "]],\n  [[") + "\n")
    print(f"{OUT.name}: {len(polys)} couch groups, {sum(map(len, polys))} points")
    if args.debug:
        args.debug.parent.mkdir(parents=True, exist_ok=True)
        debug_sheet(rgb, polys, args.debug)


if __name__ == "__main__":
    main()

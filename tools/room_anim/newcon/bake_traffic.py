#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Bake hangar ship traffic into engine sprite sheets (#553).

Unlike the concourse layers these are plain straight-alpha sprites: the ships
fly through open space and the tunnel, touching no painted floor, so there is
nothing plate-aware to encode. Each rendered pass (render_hangar.py) is split
at the mouth plane into two sheets on ONE timeline:

    anim/hangar/<layer>_under.json   out in space: drawn beneath the plate, so
                                     the tunnel rim and parked ship occlude it
    anim/hangar/<layer>_over.json    inside the tunnel: drawn over the plate

Both carry the canonical mouth "anchor", so the engine maps them onto every
composite's own mouth. Over-plate frames must stay above the parked hulls
(tallest: drayman, at cy - 0.23 r); the bake refuses frames that dip below
OVER_FLOOR.

Loop timing (period / phase, seconds) is in hangar_layers.json.

Usage (from the repo root):
    uv run tools/room_anim/newcon/bake_traffic.py --all
"""
import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bake_layer import load_frame, write_sheet  # noqa: E402
from base import paths  # noqa: E402

NEWCON = paths("newcon")
BUILD = NEWCON.build / "hangar"
OUT = NEWCON.anim / "hangar"
TIMING = NEWCON.tools / "hangar_layers.json"
CANVAS = (1536, 1024)
OVER_FLOOR = -0.25       # over-plate sprites end above cy + OVER_FLOOR * r
VISIBLE_ALPHA = 8        # alpha below this is motion-blur haze, ignored by the check


def visible_bottom(path):
    alpha = np.asarray(Image.open(path).convert("RGBA"))[..., 3]
    rows = np.nonzero((alpha >= VISIBLE_ALPHA).any(axis=1))[0]
    return int(rows.max()) + 1 if rows.size else None


def bake(layer, period, offset):
    src = BUILD / layer
    info = json.loads((src / "pass.json").read_text())
    fps, anchor, over = float(info["fps"]), info["anchor"], set(info["over"])
    floor = anchor[1] + OVER_FLOOR * anchor[2]
    sides = {"under": ([], []), "over": ([], [])}
    for path in sorted(src.glob("*.png")):
        frame = int(path.stem)
        side = "over" if frame in over else "under"
        if side == "over":
            bottom = visible_bottom(path)
            if bottom is not None and bottom > floor:
                raise SystemExit(f"{layer} frame {frame}: in-tunnel ship reaches y={bottom}, "
                                 f"below the parked-hull line {floor:.0f}")
        baked = load_frame(path)
        if baked is not None:
            sides[side][0].append(baked)
            sides[side][1].append(frame - 1)          # frame N -> slot N-1
    period_frames = max(int(info["frames"]), int(math.ceil(period * fps)))
    for side, (sprites, slots) in sides.items():
        if sprites:
            write_sheet(OUT, f"{layer}_{side}", sprites, slots, CANVAS, fps, period_frames,
                        int(round(offset * fps)), under=side == "under", anchor=anchor)


def main():
    timing = json.loads(TIMING.read_text())
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("layer", nargs="?", choices=sorted(timing))
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()
    if args.all == bool(args.layer):
        ap.error("give exactly one of LAYER or --all")
    for layer in (sorted(timing) if args.all else [args.layer]):
        bake(layer, timing[layer]["period"], timing[layer]["offset"])


if __name__ == "__main__":
    main()

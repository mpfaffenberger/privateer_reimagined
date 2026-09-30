#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Bake the aircraft outside the Agricultural concourse windows (#582).

render_traffic.py renders each craft as straight-alpha frames. They fly in
front of the painted farmland, so they are drawn OVER the plate, not under it
through the sky mask (the drifting-cloud mask stops above the ridges). Each
frame is
    hazed    blended toward the painted horizon haze by its depth, like the
             painting's own aerial perspective (1 - exp(-depth / HAZE_M)),
    clipped  to the window glass (windows.py), so the frames, the mullion
             and the sill stand in front of it,
then trimmed and packed on one timeline (bake_layer.write_sheet).

Loop timing (period / phase, seconds) is in traffic_layers.json.

Usage (from the repo root):
    uv run tools/room_anim/agricultural/bake_traffic.py --all
"""
import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent)]         # this base's modules, then shared ones
from bake_layer import BIG_FRAME_PX, bbox, shrink, write_sheet  # noqa: E402
from base import paths  # noqa: E402
from windows import glass  # noqa: E402

AGRI = paths("agricultural")
BUILD = AGRI.build / "traffic"
TIMING = AGRI.tools / "traffic_layers.json"
HAZE_M = 4000.0             # m: a craft this far is 63% haze
HAZE_BAND = np.s_[160:172, 650:950]   # plate: the glow just above the far ridges


def bake(layer, period, offset, plate, window):
    src = BUILD / layer
    info = json.loads((src / "pass.json").read_text())
    haze = np.median(plate[HAZE_BAND].reshape(-1, 3), axis=0)
    sprites, slots = [], []
    for path in sorted(src.glob("[0-9]*.png")):
        frame = int(path.stem)
        rgba = np.asarray(Image.open(path).convert("RGBA"), np.float32) / 255.0
        h = 1.0 - math.exp(-info["depth"][frame - 1] / HAZE_M)
        rgb = rgba[..., :3] * (1.0 - h) + haze * h
        alpha = rgba[..., 3] * window
        out = (np.dstack([rgb, alpha]) * 255.0 + 0.5).astype(np.uint8)
        box = bbox(out[..., 3])
        if box is None:
            continue                                   # wholly behind a frame
        x0, y0, x1, y1 = box
        sprite = out[y0:y1, x0:x1]
        if sprite.shape[0] * sprite.shape[1] > BIG_FRAME_PX:
            sprite = shrink(sprite)
        sprites.append((sprite, [x0, y0, x1 - x0, y1 - y0]))
        slots.append(frame - 1)                        # frame N -> slot N-1
    if not sprites:
        raise SystemExit(f"{layer}: every frame is empty")
    fps = float(info["fps"])
    period_frames = max(int(info["frames"]), int(math.ceil(period * fps)))
    write_sheet(AGRI.anim, layer, sprites, slots, (plate.shape[1], plate.shape[0]), fps,
                period_frames, int(round(offset * fps)))


def main():
    timing = {k: v for k, v in json.loads(TIMING.read_text()).items() if not k.startswith("_")}
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("layer", nargs="?", choices=sorted(timing))
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()
    if args.all == bool(args.layer):
        ap.error("give exactly one of LAYER or --all")
    plate = np.asarray(Image.open(AGRI.plate).convert("RGB"), np.float32) / 255.0
    window = glass((plate.shape[1], plate.shape[0]))
    for layer in (sorted(timing) if args.all else [args.layer]):
        bake(layer, timing[layer]["period"], timing[layer]["offset"], plate, window)


if __name__ == "__main__":
    main()

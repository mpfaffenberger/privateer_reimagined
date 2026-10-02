#!/usr/bin/env python3
"""Paint the static salesman out of the Oxford ship dealer (#682).

Usage (from the repo root, before baking the salesman):
    uv run --with scipy --with pillow --with numpy \\
        tools/room_anim/oxford/bake_shipdealer_patch.py
    uv run tools/room_anim/bake_layer.py --base oxford --room shipdealer --all

Writes anim/shipdealer/salesman_patch.{png,json}: one static frame on a
one-slot loop, drawn under the walking salesman (the room's "layers" list
it first; his bake encodes him over it, shipdealer_layers.json "over").
The patch comes from an AI clean-plate edit of a square crop around him and
his shadow (sources/shipdealer_salesman_clean_gen.png: codex_imagegen, the
crop 4.1x as reference, "remove the man and his whole shadow, continue the
planks"). bake_patrons.clean_patch() takes only where it differs strongly
from the plate (him), feathered; it registers to ~4 levels at the edges.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
for path in (HERE, HERE.parent):              # this base's modules, then shared ones
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import bake_patrons  # noqa: E402
from base import paths  # noqa: E402
from bake_layer import write_sheet  # noqa: E402

SHIPDEALER = paths("oxford", "shipdealer")
# bake_patrons.clean_patch()'s patron keys, in plate px (crop) and crop px.
SALESMAN = {
    # Square, like the generated source; down to y 672 for his shadow, which
    # runs from his feet down the screen to ~y 650.
    "crop": [664, 424, 912, 672],
    "clean_gen": "shipdealer_salesman_clean_gen.png",
    "reg": [0, 80, 24, 248],            # the crop's left edge: open planks
    "box": [40, 40, 200, 248],          # him, his raised arm and his shadow
    # His shadow's soft tail: too faint for the difference threshold in
    # places, so it's taken from the clean plate outright.
    "keep": [[70, 150, 160, 240]],
    "front": [],
}


def main():
    plate = Image.open(SHIPDEALER.plate).convert("RGB")
    patch = bake_patrons.clean_patch(SALESMAN, plate, sources=HERE / "sources")
    ys, xs = np.nonzero(patch[..., 3])
    x0, y0 = int(xs.min()), int(ys.min())
    sprite = patch[y0:ys.max() + 1, x0:xs.max() + 1]
    dst = [SALESMAN["crop"][0] + x0, SALESMAN["crop"][1] + y0, sprite.shape[1], sprite.shape[0]]
    # A one-slot loop: on any longer one, slots without a frame draw nothing
    # and the painted salesman would flicker back.
    write_sheet(SHIPDEALER.anim, "salesman_patch", [(sprite, dst)], [0], plate.size, 12.0, 1, 0)


if __name__ == "__main__":
    main()

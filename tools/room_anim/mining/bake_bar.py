"""Bake the mining bar's 3D patron into over-plate layers (#564 spike).

Usage (from the repo root, after render_bar.py):
    uv run tools/room_anim/mining/bake_bar.py
    uv run tools/room_anim/mining/bake_bar.py --preview 0,60,120 --out build/room_anim/mining/bar_fit.png

bar_bg.png stays untouched, so the painted woman in orange is still in the
plate. Two layers go on top of it, drawn in order:
    patron_orange_patch  one static frame on a one-slot loop: her painted out
    patron_orange        her 3D idle (render_bar.py), straight alpha
If the layers are missing, the painting shows as painted.

The patch comes from an AI clean-plate edit of her crop
(sources/bar_orange_clean_gen.png, 1024 px for the 320 px crop). The
generator repaints everything, so only her region is taken: strong
differences from the plate inside her box, closed and hole-filled, plus the
whole seat box (the regenerated bench differs slightly in shape), grown and
feathered. The bar counter's corner is in front of her: nothing below its
edge is ever replaced, and her render is clipped there too.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bake_layer import load_frame, write_sheet  # noqa: E402
from base import paths  # noqa: E402

MINING = paths("mining")
BUILD = MINING.build / "bar"
OUT = MINING.anim / "bar"
LAYER = "patron_orange"
CLEAN_GEN = MINING.tools / "sources" / "bar_orange_clean_gen.png"
CANVAS = (1536, 1024)

CROP = (840, 340, 1160, 660)            # plate px; the clean-plate edit's frame
BOX = (80, 50, 275, 300)                # crop px: her, head to boots
SEAT = (170, 165, 275, 262)             # crop px: the painted seat box, all of it
COUNTER = ((75, 320), (300, 225))       # crop px: the counter's top edge
MAX_REGISTRATION_ERROR = 6.0            # mean |diff| on the wall, 0-255
# The shadow catchers leave a faint alpha across the whole render region
# (soft light falloff): below ~2% it's invisible on this dark plate, but it
# stretched every frame to the full crop (a 4096x6419 atlas). Dropped.
SHADOW_FLOOR = 6


def _below_counter(h, w, origin=(0, 0)):
    """Mask of crop-space pixels in front of (below) the counter's edge.
    `origin` offsets for arrays that start elsewhere in the crop."""
    yy, xx = np.mgrid[:h, :w]
    xx, yy = xx + origin[0], yy + origin[1]
    (x0, y0), (x1, y1) = COUNTER
    return yy >= y0 + (xx - x0) * (y1 - y0) / (x1 - x0) - 2


def clean_patch(plate):
    """-> RGBA patch (crop-sized) that paints her out, feathered alpha."""
    x0, y0, x1, y1 = CROP
    orig = plate.crop(CROP)
    w, h = orig.size
    gen = Image.open(CLEAN_GEN).convert("RGB").resize((w, h), Image.LANCZOS)
    o, g = np.asarray(orig, np.int16), np.asarray(gen, np.int16)
    wall = np.abs(g[20:150, 10:180] - o[20:150, 10:180]).mean()
    if wall > MAX_REGISTRATION_ERROR:
        raise SystemExit(f"clean plate doesn't register with the plate (wall error {wall:.1f})")
    diff = np.abs(g - o).max(axis=2)
    smooth = ndimage.uniform_filter(diff.astype(float), 5)
    yy, xx = np.mgrid[:h, :w]
    inside = (xx >= BOX[0]) & (xx < BOX[2]) & (yy >= BOX[1]) & (yy < BOX[3])
    her = ndimage.binary_opening((smooth > 16) & inside, iterations=1)
    her = ndimage.binary_closing(her, iterations=10)
    lab, n = ndimage.label(her)
    her = lab == (1 + int(np.argmax(ndimage.sum(her, lab, range(1, n + 1)))))
    her = ndimage.binary_fill_holes(her)
    her |= (xx >= SEAT[0]) & (xx < SEAT[2]) & (yy >= SEAT[1]) & (yy < SEAT[3])
    her = ndimage.binary_dilation(her, iterations=5) & ~_below_counter(h, w)
    alpha = ndimage.gaussian_filter(her.astype(float), 3)
    alpha[_below_counter(h, w)] = 0.0
    rgba = np.dstack([np.asarray(gen, np.uint8), (alpha * 255).round().astype(np.uint8)])
    return rgba


def patron_frame(path):
    """-> (sprite RGBA, dst) of one render, clipped at the counter, or None."""
    rgba = np.asarray(Image.open(path).convert("RGBA")).copy()
    x0, y0, x1, y1 = CROP
    front = _below_counter(y1 - y0, x1 - x0)
    rgba[y0:y1, x0:x1, 3][front] = 0
    rgba[..., 3][rgba[..., 3] < SHADOW_FLOOR] = 0
    tmp = BUILD / "_clipped.png"
    Image.fromarray(rgba).save(tmp)
    return load_frame(tmp, max_px=None)        # she's the point: keep her sharp


def bake():
    plate = Image.open(MINING.room / "bar_bg.png").convert("RGB")
    info = json.loads((BUILD / LAYER / "pass.json").read_text())
    fps, frames = float(info["fps"]), int(info["frames"])
    patch = clean_patch(plate)
    ys, xs = np.nonzero(patch[..., 3])
    px0, py0 = int(xs.min()), int(ys.min())
    sprite = patch[py0:ys.max() + 1, px0:xs.max() + 1]
    dst = [CROP[0] + px0, CROP[1] + py0, sprite.shape[1], sprite.shape[0]]
    # A one-slot loop: slots without a frame draw nothing, so on the patron's
    # 230-slot loop the painted woman would flicker back 229 slots in 230.
    write_sheet(OUT, f"{LAYER}_patch", [(sprite, dst)], [0], CANVAS, fps, 1, 0)
    sprites, slots = [], []
    for path in sorted((BUILD / LAYER).glob("[0-9]*.png")):
        baked = patron_frame(path)
        if baked is not None:
            sprites.append(baked)
            slots.append(int(path.stem) - int(info["first"]))
    write_sheet(OUT, LAYER, sprites, slots, CANVAS, fps, frames, 0)


def preview(frames, out):
    """Crops of plate + patch + her at `frames`, beside the painted original."""
    plate = Image.open(MINING.room / "bar_bg.png").convert("RGBA")
    patched = plate.copy()
    patch = Image.fromarray(clean_patch(plate.convert("RGB")))
    patched.alpha_composite(patch, CROP[:2])
    tiles = [plate.crop(CROP)]
    for f in frames:
        tile = patched.copy()
        baked = patron_frame(BUILD / LAYER / f"{f:04d}.png")
        if baked is not None:                  # scaled to dst, as the engine draws it
            sprite, (x, y, w, h) = baked
            tile.alpha_composite(Image.fromarray(sprite).resize((w, h), Image.LANCZOS), (x, y))
        tiles.append(tile.crop(CROP))
    w, h = tiles[0].size
    sheet = Image.new("RGB", (w * len(tiles), h))
    for i, t in enumerate(tiles):
        sheet.paste(t.convert("RGB"), (i * w, 0))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    sheet.resize((sheet.width * 2, h * 2), Image.LANCZOS).save(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--preview", help="comma-separated frames to preview instead of baking")
    ap.add_argument("--out", default=str(MINING.build / "bar_fit.png"))
    args = ap.parse_args()
    if args.preview:
        preview([int(f) for f in args.preview.split(",")], args.out)
    else:
        bake()


if __name__ == "__main__":
    main()

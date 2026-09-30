"""Bake the mining bar's 3D patrons into over-plate layers (#564, #566, #570).

Usage (from the repo root, after render_bar.py):
    uv run --with scipy tools/room_anim/mining/bake_bar.py              # every patron
    uv run --with scipy tools/room_anim/mining/bake_bar.py --patron patron_orange \\
        --preview 0,60,120 --out build/room_anim/mining/bar_fit.png

bar_bg.png stays untouched, so the painted patrons are still in the plate.
Each 3D patron (bar_patrons.json) adds two layers on top of it:
    <patron>_patch  one static frame on a one-slot loop: the painted one out
    <patron>        the 3D idle (render_bar.py), straight alpha
If the layers are missing, the painting shows as painted.

Patrons overlap (the left table), so they're listed back to front and
stacked: each clean plate is made from the plate with every earlier
patron already painted out (`cleaned_plate`), so a nearer patron's patch
never paints a farther one back in. The room draws every patch first, then
every patron in list order.

Each patch comes from an AI clean-plate edit of the patron's crop
(sources/). The generator repaints everything, so only the patron's region is
taken: strong differences from the plate inside their box, closed and
hole-filled, plus any `keep` rects (e.g. a regenerated seat that differs
slightly in shape), grown and feathered. Occluders in front of the patron
(`front`) are never replaced, and the render is clipped at them too.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from bake_layer import load_frame, write_sheet  # noqa: E402
from base import paths  # noqa: E402

MINING = paths("mining")
BUILD = MINING.build / "bar"
OUT = MINING.anim / "bar"
SOURCES = MINING.tools / "sources"
CANVAS = (1536, 1024)
PATRONS = {k: v for k, v in json.loads((HERE / "bar_patrons.json").read_text()).items()
           if not k.startswith("_")}

MAX_REGISTRATION_ERROR = 6.0            # mean |diff| in the `reg` rect, 0-255
# The shadow catchers leave a faint alpha across the whole render region
# (soft light falloff): below ~2% it's invisible on this dark plate, but it
# stretched every frame to the full crop (a 4096x6419 atlas). Dropped.
SHADOW_FLOOR = 6


def front_mask(p, h, w):
    """Crop-space mask of the occluders in front of the patron."""
    yy, xx = np.mgrid[:h, :w]
    mask = np.zeros((h, w), bool)
    for occ in p["front"]:
        if "below_line" in occ:
            (x0, y0), (x1, y1) = occ["below_line"]
            mask |= yy >= y0 + (xx - x0) * (y1 - y0) / (x1 - x0) - 2
        else:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).polygon([tuple(v) for v in occ["polygon"]], fill=255)
            mask |= np.asarray(img) > 0
    return mask


def clean_patch(p, plate):
    """-> RGBA patch (crop-sized) that paints the patron out, feathered alpha."""
    orig = plate.crop(p["crop"])
    w, h = orig.size
    gen = Image.open(SOURCES / p["clean_gen"]).convert("RGB").resize((w, h), Image.LANCZOS)
    o, g = np.asarray(orig, np.int16), np.asarray(gen, np.int16)
    rx0, ry0, rx1, ry1 = p["reg"]
    wall = np.abs(g[ry0:ry1, rx0:rx1] - o[ry0:ry1, rx0:rx1]).mean()
    if wall > MAX_REGISTRATION_ERROR:
        raise SystemExit(f"clean plate doesn't register with the plate (error {wall:.1f})")
    diff = np.abs(g - o).max(axis=2)
    smooth = ndimage.uniform_filter(diff.astype(float), 5)
    yy, xx = np.mgrid[:h, :w]
    bx0, by0, bx1, by1 = p["box"]
    inside = (xx >= bx0) & (xx < bx1) & (yy >= by0) & (yy < by1)
    them = ndimage.binary_opening((smooth > 16) & inside, iterations=1)
    them = ndimage.binary_closing(them, iterations=10)
    lab, n = ndimage.label(them)
    them = lab == (1 + int(np.argmax(ndimage.sum(them, lab, range(1, n + 1)))))
    them = ndimage.binary_fill_holes(them)
    for kx0, ky0, kx1, ky1 in p["keep"]:
        them |= (xx >= kx0) & (xx < kx1) & (yy >= ky0) & (yy < ky1)
    front = front_mask(p, h, w)
    them = ndimage.binary_dilation(them, iterations=5) & ~front
    alpha = ndimage.gaussian_filter(them.astype(float), 3)
    alpha[front] = 0.0
    return np.dstack([np.asarray(gen, np.uint8), (alpha * 255).round().astype(np.uint8)])


def patron_frame(p, path):
    """-> (sprite RGBA, dst) of one render, clipped at the occluders, or None."""
    rgba = np.asarray(Image.open(path).convert("RGBA")).copy()
    x0, y0, x1, y1 = p["crop"]
    rgba[y0:y1, x0:x1, 3][front_mask(p, y1 - y0, x1 - x0)] = 0
    rgba[..., 3][rgba[..., 3] < SHADOW_FLOOR] = 0
    tmp = BUILD / "_clipped.png"
    Image.fromarray(rgba).save(tmp)
    return load_frame(tmp, max_px=None)        # the patron is the point: keep them sharp


def cleaned_plate(name):
    """-> the plate (RGB) with every patron listed before `name` painted out:
    what `name`'s clean-plate source must be made from, and registered to."""
    plate = Image.open(MINING.room / "bar_bg.png").convert("RGBA")
    for other in PATRONS:
        if other == name:
            break
        q = PATRONS[other]
        plate.alpha_composite(Image.fromarray(clean_patch(q, plate.convert("RGB"))),
                              tuple(q["crop"][:2]))
    return plate.convert("RGB")


def bake(name):
    p = PATRONS[name]
    plate = cleaned_plate(name)
    info = json.loads((BUILD / name / "pass.json").read_text())
    fps, frames = float(info["fps"]), int(info["frames"])
    patch = clean_patch(p, plate)
    ys, xs = np.nonzero(patch[..., 3])
    px0, py0 = int(xs.min()), int(ys.min())
    sprite = patch[py0:ys.max() + 1, px0:xs.max() + 1]
    dst = [p["crop"][0] + px0, p["crop"][1] + py0, sprite.shape[1], sprite.shape[0]]
    # A one-slot loop: slots without a frame draw nothing, so on the patron's
    # loop the painted one would flicker back every other slot.
    write_sheet(OUT, f"{name}_patch", [(sprite, dst)], [0], CANVAS, fps, 1, 0)
    sprites, slots = [], []
    for path in sorted((BUILD / name).glob("[0-9]*.png")):
        baked = patron_frame(p, path)
        if baked is not None:
            sprites.append(baked)
            slots.append(int(path.stem) - int(info["first"]))
    write_sheet(OUT, name, sprites, slots, CANVAS, fps, frames, int(p["phase"]))


def preview(name, frames, out):
    """Crops of the room as the engine draws it at `frames` (patches, then the
    patrons up to `name`, back to front), beside the painted original."""
    p = PATRONS[name]
    upto = list(PATRONS)[:list(PATRONS).index(name) + 1]
    patched = Image.open(MINING.room / "bar_bg.png").convert("RGBA")
    tiles = [patched.crop(tuple(p["crop"]))]    # PIL wants a tuple, JSON gives a list
    for other in upto:
        q = PATRONS[other]
        patched.alpha_composite(Image.fromarray(clean_patch(q, patched.convert("RGB"))),
                                tuple(q["crop"][:2]))
    for f in frames:
        tile = patched.copy()
        for other in upto:
            path = BUILD / other / f"{f:04d}.png"
            baked = patron_frame(PATRONS[other], path) if path.exists() else None
            if baked is not None:              # scaled to dst, as the engine draws it
                sprite, (x, y, w, h) = baked
                tile.alpha_composite(Image.fromarray(sprite).resize((w, h), Image.LANCZOS), (x, y))
        tiles.append(tile.crop(tuple(p["crop"])))
    w, h = tiles[0].size
    sheet = Image.new("RGB", (w * len(tiles), h))
    for i, t in enumerate(tiles):
        sheet.paste(t.convert("RGB"), (i * w, 0))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    sheet.resize((sheet.width * 2, h * 2), Image.LANCZOS).save(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--patron", action="append", choices=sorted(PATRONS),
                    help="repeatable; default: every patron")
    ap.add_argument("--preview", help="comma-separated frames to preview instead of baking")
    ap.add_argument("--out", default=str(MINING.build / "bar_fit.png"))
    args = ap.parse_args()
    names = args.patron or list(PATRONS)
    if args.preview:
        if len(names) != 1:
            raise SystemExit("--preview needs exactly one --patron")
        preview(names[0], [int(f) for f in args.preview.split(",")], args.out)
    else:
        for name in names:
            bake(name)


if __name__ == "__main__":
    main()

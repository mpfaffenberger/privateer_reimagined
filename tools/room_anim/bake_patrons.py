"""Bake a room's 3D patrons into over-plate layers (#564, #566, #570; any room since #577).

Usage (from the repo root, after render_patrons.py):
    uv run --with scipy tools/room_anim/bake_patrons.py --room tools/room_anim/mining/bar_patrons.json
    uv run --with scipy tools/room_anim/bake_patrons.py --room <file> --patron patron_orange \\
        --preview 0,60,120 --out build/room_anim/mining/bar_fit.png

The plate stays untouched, so the painted patrons are still in it. Each 3D
patron (the room file, patron_room.py) adds two layers on top of it:
    <patron>_patch  one static frame on a one-slot loop: the painted one out
    <patron>        the 3D idle (render_patrons.py), straight alpha
If the layers are missing, the painting shows as painted. A patron added to an
empty seat (#676: New Detroit's bar is painted empty) has neither a
`clean_gen` nor an `inpaint`: there's nobody to paint out, so no _patch layer.

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
sys.path.insert(0, str(HERE))
import patron_room  # noqa: E402
import still  # noqa: E402
from bake_layer import inset_border, load_frame, write_sheet  # noqa: E402

CANVAS = (1536, 1024)
# The room being baked (patron_room.load), set by main(). One room per process.
ROOM = None

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
    gen = Image.open(ROOM.sources / p["clean_gen"]).convert("RGB").resize((w, h), Image.LANCZOS)
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


def patron_frame(p, src):
    """-> (sprite RGBA, dst) of one render (a path or a plate-sized RGBA
    array), clipped at the occluders, or None."""
    rgba = (np.asarray(Image.open(src).convert("RGBA")) if isinstance(src, Path) else src).copy()
    x0, y0, x1, y1 = p["crop"]
    rgba[y0:y1, x0:x1, 3][front_mask(p, y1 - y0, x1 - x0)] = 0
    # Only the crop renders, and the denoiser sees nothing past it: its edge
    # rows come out as stray specks of alpha, and one speck stretches every
    # sprite to the whole crop (#676: a 2048x7292 atlas for a 70x160 px
    # patron). Dropped like the concourse passes' (keep crops padded).
    region = np.zeros(rgba.shape[:2], bool)
    region[y0:y1, x0:x1] = True
    rgba[..., 3][~inset_border(region)] = 0
    rgba[..., 3][rgba[..., 3] < SHADOW_FLOOR] = 0
    tmp = ROOM.build / "_clipped.png"
    Image.fromarray(rgba).save(tmp)
    # The patron is the point: keep them sharp, unless they're too big for
    # one atlas at full size (`half_size`: the foreground man, 258 frames of
    # ~350x580 px, needs 4096x25799; the limit is 8192).
    return load_frame(tmp, max_px=1 if p.get("half_size") else None)


# A frame repeats the one before it if fewer than SAME_PX pixels differ by
# more than SAME_LEVEL: a held pose renders the same image give or take GPU
# sampling jitter (<= 3 levels, a few stray px). Real motion is far above
# it: her nail fidget changes ~200 px a frame.
SAME_PX, SAME_LEVEL = 24, 6


def load_renders(name, crop):
    """The patron's render frames as plate-sized RGBA. A frame that
    repeats the one before it (SAME_PX) is that same array, so a held pose
    is baked and packed once (the merchant holds his cigar low for 3 s).
    Only the crop is compared and kept (renders are transparent outside)."""
    x0, y0, x1, y1 = crop
    frames, last = [], None
    for path in sorted((ROOM.build / name).glob("[0-9]*.png")):
        img = Image.open(path).convert("RGBA")
        part = np.asarray(img.crop((x0, y0, x1, y1)))
        if last is None or (np.abs(part.astype(np.int16) - last).max(-1) > SAME_LEVEL).sum() >= SAME_PX:
            full = np.zeros((img.height, img.width, 4), np.uint8)
            full[y0:y1, x0:x1] = part
            frames.append(full)
            last = part.astype(np.int16)
        else:
            frames.append(frames[-1])
    return frames


def inpaint_patch(p, plate, renders):
    """-> RGBA patch (crop-sized) for a patron without an AI clean plate
    (#579: the image model was out of reach). The 3D patron sits over the
    painted one, so only the painted pixels (`inpaint`, a plate-px polygon
    around him) that some render frame leaves uncovered are patched. They're
    filled from the room around the polygon (OpenCV Telea, the whole
    polygon unknown so the fill never samples him); the rest shows through."""
    import cv2                          # only this path needs OpenCV
    x0, y0, x1, y1 = p["crop"]
    orig = np.asarray(plate.crop((x0, y0, x1, y1)).convert("RGB"))
    img = Image.new("L", (x1 - x0, y1 - y0), 0)
    ImageDraw.Draw(img).polygon([(x - x0, y - y0) for x, y in p["inpaint"]], fill=255)
    painted = np.asarray(img) > 0
    covered = np.logical_and.reduce([r[y0:y1, x0:x1, 3] > 250 for r in renders])
    hole = ndimage.binary_dilation(painted & ~covered, iterations=2)
    unknown = ndimage.binary_dilation(painted, iterations=2).astype(np.uint8) * 255
    fill = cv2.cvtColor(cv2.inpaint(cv2.cvtColor(orig, cv2.COLOR_RGB2BGR), unknown, 6,
                                    cv2.INPAINT_TELEA), cv2.COLOR_BGR2RGB)
    alpha = ndimage.gaussian_filter(hole.astype(float), 1.0)
    return np.dstack([fill, (alpha * 255).round().astype(np.uint8)])


def has_patch(p):
    """Does `p` replace a painted patron (a clean-plate patch), or sit in an
    empty seat (#676: nothing to paint out)?"""
    return "clean_gen" in p or "inpaint" in p


def cleaned_plate(name):
    """-> the plate (RGB) with every patron listed before `name` painted out:
    what `name`'s clean-plate source must be made from, and registered to."""
    plate = Image.open(ROOM.plate).convert("RGBA")
    for other in ROOM.patrons:
        if other == name:
            break
        q = ROOM.patrons[other]
        if has_patch(q):
            plate.alpha_composite(Image.fromarray(clean_patch(q, plate.convert("RGB"))),
                                  tuple(q["crop"][:2]))
    return plate.convert("RGB")


def bake(name):
    p = ROOM.patrons[name]
    info = json.loads((ROOM.build / name / "pass.json").read_text())
    fps, frames = float(info["fps"]), int(info["frames"])
    renders = load_renders(name, p["crop"]) if "still" in p or "inpaint" in p else None
    if has_patch(p):
        write_patch(name, p, fps, renders)
    if "still" in p:            # one held pose with small moves: still.py
        sprites, slots, frames = still.timeline(p["still"], fps, renders,
                                                lambda rgba: patron_frame(p, rgba), ROOM.sources)
        write_sheet(ROOM.out, name, sprites, slots, CANVAS, fps, frames, int(p["phase"]))
        if "smoke" in p["still"]:        # its own layer, over the patron
            puffs, slots = still.smoke(p["still"]["smoke"], fps, frames)
            write_sheet(ROOM.out, f"{name}_smoke", puffs, slots, CANVAS, fps, frames,
                        int(p["phase"]))
        return
    sprites, slots = [], []
    for path in sorted((ROOM.build / name).glob("[0-9]*.png")):
        baked = patron_frame(p, path)
        if baked is not None:
            sprites.append(baked)
            slots.append(int(path.stem) - int(info["first"]))
    write_sheet(ROOM.out, name, sprites, slots, CANVAS, fps, frames, int(p["phase"]))


def write_patch(name, p, fps, renders):
    """The one-frame clean-plate patch that paints the painted `name` out."""
    plate = cleaned_plate(name)
    patch = inpaint_patch(p, plate, renders) if "inpaint" in p else clean_patch(p, plate)
    ys, xs = np.nonzero(patch[..., 3])
    px0, py0 = int(xs.min()), int(ys.min())
    sprite = patch[py0:ys.max() + 1, px0:xs.max() + 1]
    dst = [p["crop"][0] + px0, p["crop"][1] + py0, sprite.shape[1], sprite.shape[0]]
    # A one-slot loop: slots without a frame draw nothing, so on the patron's
    # loop the painted one would flicker back every other slot.
    write_sheet(ROOM.out, f"{name}_patch", [(sprite, dst)], [0], CANVAS, fps, 1, 0)


def preview(name, frames, out):
    """Crops of the room as the engine draws it at `frames` (patches, then the
    patrons up to `name`, back to front), beside the painted original."""
    patrons = ROOM.patrons
    p = patrons[name]
    upto = list(patrons)[:list(patrons).index(name) + 1]
    patched = Image.open(ROOM.plate).convert("RGBA")
    tiles = [patched.crop(tuple(p["crop"]))]    # PIL wants a tuple, JSON gives a list
    for other in (o for o in upto if has_patch(patrons[o])):
        q = patrons[other]
        patched.alpha_composite(Image.fromarray(clean_patch(q, patched.convert("RGB"))),
                                tuple(q["crop"][:2]))
    for f in frames:
        tile = patched.copy()
        for other in upto:
            path = ROOM.build / other / f"{f:04d}.png"
            baked = patron_frame(patrons[other], path) if path.exists() else None
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
    global ROOM
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--room", required=True, help="room file (patron_room.py)")
    ap.add_argument("--patron", action="append", help="repeatable; default: every patron")
    ap.add_argument("--preview", help="comma-separated frames to preview instead of baking")
    ap.add_argument("--out", help="preview path (default: <room build>_fit.png)")
    args = ap.parse_args()
    ROOM = patron_room.load(args.room)
    unknown = set(args.patron or ()) - set(ROOM.patrons)
    if unknown:
        ap.error(f"unknown patron(s) {sorted(unknown)}; the room has {sorted(ROOM.patrons)}")
    names = args.patron or list(ROOM.patrons)
    if args.preview:
        if len(names) != 1:
            raise SystemExit("--preview needs exactly one --patron")
        out = args.out or f"{ROOM.build}_fit.png"          # the bar: build/room_anim/mining/bar_fit.png
        preview(names[0], [int(f) for f in args.preview.split(",")], out)
    else:
        for name in names:
            bake(name)


if __name__ == "__main__":
    main()

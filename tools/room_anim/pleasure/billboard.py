#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "scipy"]
# ///
"""Turn the Pleasure concourse's ship-rental billboard into a live ad screen (#599).

The billboard hanging on the right is a two-panel video wall (a thin
mullion splits it) showing two painted ships on a starfield. Its right-hand
end is off the plate, so two of its corners are too: the screen is found by
fitting its edges as lines instead.

    billboard_screen   one static sprite: the painted ships painted out of
                       the starfield (a one-slot loop, always on screen).
    billboard_ad       the ad: game ships (render_billboard.py) flying across
                       the wall, warped from the flat ad canvas onto it.

Geometry. Inside the bezel the screen is saturated blue, so its pixels are
the two biggest blue blobs in SCREEN_BOX. Lines fitted to their top, bottom
and left edges and to both sides of the mullion's gap give the panels. The
top and bottom meet at the screen's horizontal vanishing point, the left
edge and mullion at its vertical one; with the principal point at the plate
centre, the two being at right angles gives the focal length, and with it
the panels' true aspect (~1.6). The ad is rendered on a flat canvas of two
such panels side by side (CANVAS_H px tall) and a homography maps it onto
the plate.

Occlusion is all in the mask: the bezel, the mullion and the marquee
canopy below are outside the fitted panels, so they stay painted in front of
the ad and nothing is drawn over the canopy's bulbs.

Outputs:
    assets/concourse/pleasure/anim/billboard_screen.{png,json}
    assets/concourse/pleasure/anim/billboard_ad.{png,json}      (--ad)
    build/room_anim/pleasure/billboard/screen.json  the ad canvas for Blender

Usage (from the repo root):
    uv run tools/room_anim/pleasure/billboard.py [--debug build/room_anim/pleasure/billboard.png]
    blender --background --factory-startup --python tools/room_anim/pleasure/render_billboard.py
    uv run tools/room_anim/pleasure/billboard.py --ad
"""
import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bake_layer import bbox, encode, load_rgba, write_sheet  # noqa: E402
from base import paths  # noqa: E402
from sky import star_removed  # noqa: E402

PLEASURE = paths("pleasure")
BUILD = PLEASURE.build / "billboard"
SCREEN, AD = "billboard_screen", "billboard_ad"

SCREEN_BOX = (1150, 100, 1536, 560)   # x0, y0, x1, y1: the billboard, plate px
LEFT_ROWS = (416, 516)                # rows whose leftmost screen px is the left edge
BOTTOM_COLS = (1265, 1536)            # columns whose lowest screen px is the bottom edge
MULLION_COLS = (1280, 1460)           # where the mullion crosses the rows
MULLION_GAP = (3, 16)                 # its gap between the panels, px
CANVAS_H = 480                        # ad canvas height (~1.7x the nearest panel)
SUPERSAMPLE = 3                       # per axis, for the mask and the warp
AD_PREFILTER = 0.7                    # canvas px: blur before the warp shrinks the ad

# Painting the ships out: the starfield has next to no red (95th percentile
# < 22 away from the stars), the ships' metal has plenty, even where it
# mirrors the blue (5th percentile ~34). Red over SHIP_RED in a blob bigger
# than a star (SHIP_PX) is ship, closed, filled and grown by SHIP_GROW px
# (past the dark tips of its fins). Each hole is then covered with patches
# of the painted starfield from elsewhere on the wall (paint_out).
SHIP_RED = 24
SHIP_PX = 150
SHIP_CLOSE = 8
SHIP_GROW = 6
LOW_SIGMA = 6.0                       # px: the sky tone a patch is shifted to
RELAX_STEPS = 600                     # harmonic fill of that tone across a hole
DONOR_REACH, DONOR_STEP = 240, 3      # px: where to look for a patch of painted sky
DONOR_PATCHES = 4


def screen_blue(rgb):
    r, g, b = (rgb[..., c] * 255.0 for c in range(3))
    return (b > r + 25) & (b > g + 25) & (b > 40)


def screen_pixels(rgb):
    """The two panels: the two biggest blue blobs in SCREEN_BOX, holes filled."""
    x0, y0, x1, y1 = SCREEN_BOX
    box = np.zeros(rgb.shape[:2], bool)
    box[y0:y1, x0:x1] = True
    labels, n = ndimage.label(screen_blue(rgb) & box)
    sizes = ndimage.sum(np.ones_like(labels), labels, index=np.arange(1, n + 1))
    panels = np.isin(labels, np.argsort(sizes)[-2:] + 1)
    return ndimage.binary_fill_holes(panels)


def fit_line(u, v):
    """v = a u + b, refitted without the outliers (ships touching an edge)."""
    u, v = np.asarray(u, float), np.asarray(v, float)
    keep = np.ones(u.size, bool)
    for _ in range(5):
        a, b = np.polyfit(u[keep], v[keep], 1)
        res = np.abs(a * u + b - v)
        keep = res < max(1.5, 2.5 * np.median(res[keep]))
    return a, b


def as_row_line(a, b):
    """y = a x + b -> homogeneous line."""
    return np.array([a, -1.0, b])


def as_col_line(a, b):
    """x = a y + b -> homogeneous line."""
    return np.array([-1.0, a, b])


def meet(p, q):
    h = np.cross(p, q)
    return h[:2] / h[2]


def homography(src, dst):
    """3x3 H with dst ~ H src for four point pairs."""
    rows = []
    for (x, y), (u, v) in zip(src, dst):
        rows.append([x, y, 1, 0, 0, 0, -u * x, -u * y, -u])
        rows.append([0, 0, 0, x, y, 1, -v * x, -v * y, -v])
    h = np.linalg.svd(np.asarray(rows, float))[2][-1]
    return (h / h[-1]).reshape(3, 3)


def apply(h, xy):
    p = np.c_[xy, np.ones(len(xy))] @ h.T
    return p[:, :2] / p[:, 2:]


def panel_aspect(top, bottom, left, mullion, tl, bl, tm):
    """Width / height of the left panel: the vanishing points of its two
    edge directions are at right angles about a camera centred on the plate."""
    c = np.array([768.0, 512.0])
    vh, vv = meet(top, bottom), meet(left, mullion)
    f = math.sqrt(-np.dot(vh - c, vv - c))
    k_inv = np.linalg.inv(np.array([[f, 0, c[0]], [0, f, c[1]], [0, 0, 1]]))
    d1, d2 = (k_inv @ np.r_[v, 1.0] for v in (vh, vv))
    normal = np.cross(d1, d2)
    origin = k_inv @ np.r_[tl, 1.0]

    def on_plane(p):
        ray = k_inv @ np.r_[p, 1.0]
        return ray * (normal @ origin) / (normal @ ray)
    width = np.linalg.norm(on_plane(tm) - on_plane(tl))
    height = np.linalg.norm(on_plane(bl) - on_plane(tl))
    return width / height, f


def fit_screen(rgb):
    """-> dict: canvas->plate homography `H`, `canvas` [w, h], the panels'
    canvas x spans and the focal length the aspect came from."""
    pix = screen_pixels(rgb)
    rows = range(*LEFT_ROWS)
    left = as_col_line(*fit_line(rows, [np.argmax(pix[y]) for y in rows]))
    cols = [x for x in range(SCREEN_BOX[0], SCREEN_BOX[2]) if pix[:, x].any()]
    top = as_row_line(*fit_line(cols, [np.argmax(pix[:, x]) for x in cols]))
    cols = [x for x in range(*BOTTOM_COLS) if pix[:, x].any()]
    bottom = as_row_line(*fit_line(
        cols, [pix.shape[0] - 1 - np.argmax(pix[::-1, x]) for x in cols]))
    gap_rows, gap_l, gap_r = [], [], []
    for y in range(SCREEN_BOX[1], SCREEN_BOX[3]):
        xs = np.nonzero(pix[y, MULLION_COLS[0]:MULLION_COLS[1]])[0]
        for i in np.nonzero(np.diff(xs) > 1)[0]:
            if MULLION_GAP[0] <= xs[i + 1] - xs[i] - 1 <= MULLION_GAP[1]:
                gap_rows.append(y)
                gap_l.append(MULLION_COLS[0] + xs[i] + 1)      # first non-screen px
                gap_r.append(MULLION_COLS[0] + xs[i + 1])      # first screen px again
    mull_l = as_col_line(*fit_line(gap_rows, gap_l))
    mull_r = as_col_line(*fit_line(gap_rows, gap_r))

    tl, bl = meet(left, top), meet(left, bottom)
    tm, bm = meet(mull_l, top), meet(mull_l, bottom)
    aspect, focal = panel_aspect(top, bottom, left, mull_l, tl, bl, tm)
    ch = float(CANVAS_H)
    pw = ch * aspect
    h = homography([(0, 0), (0, ch), (pw, 0), (pw, ch)], [tl, bl, tm, bm])
    right0 = float(apply(np.linalg.inv(h), [meet(mull_r, top)])[0, 0])
    return {"H": h, "canvas": [int(math.ceil(right0 + pw)), CANVAS_H],
            "panels": [[0.0, pw], [right0, right0 + pw]], "focal_px": focal}


def screen_mask(scr, shape):
    """Plate-sized 0..1 coverage of the panels (antialiased by supersampling)."""
    s = SUPERSAMPLE
    x0, y0, x1, y1 = SCREEN_BOX
    ys, xs = np.mgrid[y0:y1:1.0 / s, x0:x1:1.0 / s] + 0.5 / s
    uv = apply(np.linalg.inv(scr["H"]), np.c_[xs.ravel(), ys.ravel()])
    u, v = uv[:, 0], uv[:, 1]
    inside = (v >= 0) & (v <= scr["canvas"][1])
    inside &= np.any([(u >= a) & (u <= b) for a, b in scr["panels"]], axis=0)
    cover = inside.reshape(ys.shape).astype(np.float32)
    cover = cover.reshape(y1 - y0, s, x1 - x0, s).mean(axis=(1, 3))
    mask = np.zeros(shape, np.float32)
    mask[y0:y1, x0:x1] = cover
    return mask


def painted_ships(rgb, mask):
    """Boolean: the painted ships (and their glints) inside the panels. The
    right one runs off the plate, so the plate's edge is carried outward
    (edge padding) rather than eroding it away in the closing."""
    inside = mask > 0.0
    labels, n = ndimage.label(inside & (rgb[..., 0] * 255.0 > SHIP_RED))
    sizes = ndimage.sum(np.ones_like(labels), labels, index=np.arange(1, n + 1))
    ships = np.isin(labels, np.nonzero(sizes >= SHIP_PX)[0] + 1)
    pad = SHIP_CLOSE + 1
    ships = ndimage.binary_closing(np.pad(ships, pad, mode="edge"), iterations=SHIP_CLOSE)
    ships = ndimage.binary_fill_holes(ships)[pad:-pad, pad:-pad]
    return ndimage.binary_dilation(ships, iterations=SHIP_GROW) & inside


def lowpass(rgb, known):
    """The starless sky tone at every px: the `known` sky blurred over
    LOW_SIGMA, and smoothly continued across the holes (a wide blur, then
    relaxed toward the average of its neighbours: a harmonic fill) so no
    seam marks where the painted sky stops."""
    img = Image.fromarray((rgb * 255).round().astype(np.uint8))
    starless = np.dstack([np.asarray(star_removed(c), np.float32) / 255.0 for c in img.split()])
    w = known.astype(np.float32)
    num = np.dstack([ndimage.gaussian_filter(starless[..., c] * w, LOW_SIGMA) for c in range(3)])
    den = ndimage.gaussian_filter(w, LOW_SIGMA)
    low = num / np.maximum(den, 1e-6)[..., None]
    x0, y0, x1, y1 = SCREEN_BOX
    fixed = den[y0:y1, x0:x1] > 0.3
    wide = np.dstack([ndimage.gaussian_filter(num[..., c], 6 * LOW_SIGMA) for c in range(3)])
    wide /= np.maximum(ndimage.gaussian_filter(den, 6 * LOW_SIGMA), 1e-6)[..., None]
    box = np.where(fixed[..., None], low[y0:y1, x0:x1], wide[y0:y1, x0:x1])
    for _ in range(RELAX_STEPS):
        pad = np.pad(box, ((1, 1), (1, 1), (0, 0)), mode="edge")
        avg = (pad[:-2, 1:-1] + pad[2:, 1:-1] + pad[1:-1, :-2] + pad[1:-1, 2:]) / 4.0
        box = np.where(fixed[..., None], box, avg)
    low[y0:y1, x0:x1] = box
    return low


def best_donor(hole, sky):
    """-> (dy, dx): the shift that lands the most of `hole` on
    painted sky, nearest first among equals."""
    ys, xs = np.nonzero(hole)
    h, w = hole.shape
    best = (-1.0, 0, 0, 0)
    for dy in range(-DONOR_REACH, DONOR_REACH + 1, DONOR_STEP):
        for dx in range(-DONOR_REACH, DONOR_REACH + 1, DONOR_STEP):
            sy, sx = ys + dy, xs + dx
            ok = (sy >= 0) & (sy < h) & (sx >= 0) & (sx < w)
            share = np.count_nonzero(sky[sy[ok], sx[ok]]) / ys.size
            if (share, -(dy * dy + dx * dx)) > best[:2]:
                best = (share, -(dy * dy + dx * dx), dy, dx)
    return best[2], best[3]


def paint_out(rgb, ships, sky):
    """Each ship's hole gets patches of the painted starfield moved there
    (best_donor, then again for what the first patch didn't cover, up to
    DONOR_PATCHES), their stars and nebula detail kept and their tone
    shifted to the sky around the hole. Whatever is still left gets that
    tone alone (darker and flat: star_removed() opens the texture away)."""
    low = lowpass(rgb, sky)
    out = rgb.copy()
    labels, n = ndimage.label(ships)
    for i in range(1, n + 1):
        left = labels == i
        out[left] = low[left]
        for _ in range(DONOR_PATCHES):
            if not left.any():
                break
            dy, dx = best_donor(left, sky)
            ys, xs = np.nonzero(left)
            sy, sx = np.clip(ys + dy, 0, rgb.shape[0] - 1), np.clip(xs + dx, 0, rgb.shape[1] - 1)
            good = sky[sy, sx]
            if not good.any():
                break
            ys, xs, sy, sx = ys[good], xs[good], sy[good], sx[good]
            out[ys, xs] += rgb[sy, sx] - low[sy, sx]
            left[ys, xs] = False
            print(f"billboard: ship {i}: {good.sum()} px <- sky at ({dx:+d}, {dy:+d})")
        print(f"billboard: ship {i}: {left.sum()} px left flat")
    return np.clip(out, 0.0, 1.0)


def clean_screen(rgb, mask):
    """-> (target rgb with the painted ships painted out, ship mask)."""
    ships = painted_ships(rgb, mask)
    sky = (mask > 0.99) & ~ships
    filled = paint_out(rgb, ships, sky)
    alpha = ndimage.gaussian_filter(ships.astype(np.float32), 1.0) * mask
    return rgb * (1.0 - alpha[..., None]) + filled * alpha[..., None], ships


def sprite(plate, target):
    rgba = encode(plate, target)
    x0, y0, x1, y1 = bbox(rgba[..., 3])
    return rgba[y0:y1, x0:x1], [x0, y0, x1 - x0, y1 - y0]


def write_canvas(scr):
    """The ad canvas for render_billboard.py."""
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / "screen.json").write_text(json.dumps(
        {"canvas": scr["canvas"]}) + "\n")


def debug_image(clean_rgb, scr, ships, path):
    """The painted-out screen beside the same with the fitted panels (yellow)
    and the painted-out ships (magenta) outlined, at 2x."""
    img = Image.fromarray((clean_rgb * 255).round().astype(np.uint8))
    plain = img.crop(SCREEN_BOX)
    draw = ImageDraw.Draw(img)
    w, h = scr["canvas"]
    for a, b in scr["panels"]:
        quad = apply(scr["H"], [(a, 0), (b, 0), (b, h), (a, h)])
        draw.polygon([tuple(p) for p in quad], outline=(255, 255, 0))
    edge = ships & ~ndimage.binary_erosion(ships)
    arr = np.asarray(img).copy()
    arr[edge] = (255, 0, 255)
    marked = Image.fromarray(arr).crop(SCREEN_BOX)
    sheet = Image.new("RGB", (plain.width * 2, plain.height))
    sheet.paste(plain, (0, 0))
    sheet.paste(marked, (plain.width, 0))
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(path)


def screen(plate):
    scr = fit_screen(plate)
    print(f"billboard: canvas {scr['canvas']}, panels {np.round(scr['panels'], 1).tolist()}, "
          f"f {scr['focal_px']:.0f} px")
    return scr, screen_mask(scr, plate.shape[:2])


def bake_screen(debug=None):
    plate = load_rgba(PLEASURE.plate)[..., :3]
    scr, mask = screen(plate)
    target, ships = clean_screen(plate, mask)
    write_sheet(PLEASURE.anim, SCREEN, [sprite(plate, target)], [0],
                (plate.shape[1], plate.shape[0]), 1.0, 1, 0)
    write_canvas(scr)
    if debug:
        Path(debug).parent.mkdir(parents=True, exist_ok=True)
        debug_image(target, scr, ships, debug)


def warp(scr, mask, rgba):
    """A straight-alpha ad frame on the canvas -> (sprite, dst) on the plate,
    clipped to the panels, or None. Sampled premultiplied (no dark fringe)
    at SUPERSAMPLE^2 points a plate px, after a blur that keeps the far
    panel (~4 canvas px a plate px) from aliasing."""
    box = bbox(rgba[..., 3] > 0)
    if box is None:
        return None
    cx0, cy0, cx1, cy1 = box
    corners = apply(scr["H"], [(cx0, cy0), (cx1, cy0), (cx1, cy1), (cx0, cy1)])
    x0, y0 = np.floor(corners.min(axis=0)).astype(int) - 1
    x1, y1 = np.ceil(corners.max(axis=0)).astype(int) + 1
    x0, y0 = max(x0, SCREEN_BOX[0]), max(y0, SCREEN_BOX[1])
    x1, y1 = min(x1, SCREEN_BOX[2]), min(y1, SCREEN_BOX[3])
    if x0 >= x1 or y0 >= y1:
        return None
    s = SUPERSAMPLE
    ys, xs = np.mgrid[y0:y1:1.0 / s, x0:x1:1.0 / s] + 0.5 / s
    uv = apply(np.linalg.inv(scr["H"]), np.c_[xs.ravel(), ys.ravel()]) - 0.5   # texel centres
    f = rgba.astype(np.float32) / 255.0
    pre = np.dstack([f[..., :3] * f[..., 3:], f[..., 3]])
    out = np.empty((y1 - y0, x1 - x0, 4), np.float32)
    for c in range(4):
        smooth = ndimage.gaussian_filter(pre[..., c], AD_PREFILTER)
        dense = ndimage.map_coordinates(smooth, [uv[:, 1], uv[:, 0]], order=1, cval=0.0)
        out[..., c] = dense.reshape(ys.shape).reshape(y1 - y0, s, x1 - x0, s).mean(axis=(1, 3))
    out *= mask[y0:y1, x0:x1, None]
    a = out[..., 3:]
    rgb = np.where(a > 0, out[..., :3] / np.maximum(a, 1e-6), 0.0)
    rgba8 = (np.clip(np.dstack([rgb, a]), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    rgba8[rgba8[..., 3] == 0] = 0
    tight = bbox(rgba8[..., 3])
    if tight is None:
        return None
    tx0, ty0, tx1, ty1 = tight
    return rgba8[ty0:ty1, tx0:tx1], [int(x0 + tx0), int(y0 + ty0), tx1 - tx0, ty1 - ty0]


def bake_ad():
    plate = load_rgba(PLEASURE.plate)[..., :3]
    scr, mask = screen(plate)
    src = BUILD / AD
    info = json.loads((src / "pass.json").read_text())   # written by render_billboard.py
    sprites, slots = [], []
    for path in sorted(src.glob("[0-9]*.png")):
        baked = warp(scr, mask, np.asarray(Image.open(path).convert("RGBA")))
        if baked is not None:
            sprites.append(baked)
            slots.append(int(path.stem) - 1)                  # frame N -> slot N-1
    if not sprites:
        raise SystemExit(f"{AD}: every frame is empty; render_billboard.py first")
    write_sheet(PLEASURE.anim, AD, sprites, slots, (plate.shape[1], plate.shape[0]),
                float(info["fps"]), int(info["frames"]), 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="write the fitted panels and painted-out ships here")
    ap.add_argument("--ad", action="store_true",
                    help="bake the rendered ad (render_billboard.py) instead of the screen")
    args = ap.parse_args()
    if args.ad:
        bake_ad()
    else:
        bake_screen(args.debug)


if __name__ == "__main__":
    main()

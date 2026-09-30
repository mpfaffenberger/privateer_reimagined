#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "scipy", "opencv-python-headless"]
# ///
"""Find the dusk sky and its moons in every Oxford landing composite (#593).

The Oxford landing pad is a spaceport at dusk: 18 per-hull composites
(assets/concourse/oxford/landing_ships/<hull>.png), each framed and zoomed a
little differently, under a purple-to-orange sky with a big moon and a small
one. For every composite this writes to assets/concourse/oxford/anim/landing/:

    <hull>_mask.png   L8, 255 = open sky the ship may show through
    <hull>_fill.png   that sky, the plate's own pixels at half size
    anchors.json      {"<hull>": [cx, cy, r]}: the big moon (find_moons)
    <layer>.{json,png}
                      ship passes (render_landing.py), under the plate and
                      anchored, so hangars, hills and the castle hide them

Usage (from the repo root):
    uv run tools/room_anim/oxford/bake_landing.py [--debug build/room_anim/oxford/skies.png]
    uv run tools/room_anim/oxford/bake_landing.py --layers-only    # after render_landing.py
"""
import argparse
import json
import math
import sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage as ndi

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from bake_layer import load_frame, write_sheet  # noqa: E402

OXFORD = paths("oxford")
COMPOSITES = OXFORD.room / "landing_ships"
OUT = OXFORD.anim / "landing"
BUILD = OXFORD.build / "landing"
TIMING = OXFORD.tools / "landing_layers.json"
CANVAS = (1536, 1024)

# The sky is a smooth painted gradient; everything else (skyline, trees, the
# castle, hangars) has edges. Sky = the edge-free region connected to the top
# edge. The composites differ in sharpness (drone is soft: at 6 its skyline
# leaks and the "sky" floods the tarmac), so the threshold steps down until
# the flood stops above the ground.
EDGE_STEPS = (6.0, 4.5, 3.0, 2.0)   # max-channel gradient after a 1.5 px blur
EDGE_GROW = 2                       # px: closes pinholes in the skyline
SKY_FLOOR = 0.8                     # the flood must end above 80% of the height

# The moons. Inside a good sky almost every edge pixel lies on a moon's limb,
# so circles are fitted to them (RANSAC). The two moons are a painted pair
# (small one lower right, ~half the size) and are chosen as the pair: one
# circle alone may be a hangar arch, and the crisp small moon out-votes the
# big one's soft limb.
MOON_R = (50.0, 260.0)
MOON_TRIES, MOON_CANDIDATES = 3000, 8
PAIR_RATIO = (0.35, 0.75)           # small r / big r
PAIR_OFFSET = ((0.6, 1.6), (0.5, 1.3))  # small centre - big centre, in big r
MOON_MARGIN = 8.0                   # px kept opaque around each moon: they stay painted


def _gradient(rgb):
    f = cv2.GaussianBlur(rgb.astype(np.float32), (0, 0), 1.5)
    gy = np.abs(np.diff(f, axis=0, append=f[-1:])).max(axis=2)
    gx = np.abs(np.diff(f, axis=1, append=f[:, -1:])).max(axis=2)
    return np.maximum(gx, gy)


def find_sky(rgb):
    """-> (bool flood, edge threshold used): edge-free pixels connected to the
    top row, skyline pinholes closed by EDGE_GROW."""
    h = rgb.shape[0]
    grad = _gradient(rgb)
    for step in EDGE_STEPS:
        edge = ndi.binary_dilation(grad > step, iterations=EDGE_GROW)
        lab, _ = ndi.label(~edge)
        sky = np.isin(lab, np.unique(lab[0][lab[0] > 0]))
        sky = ndi.binary_dilation(sky, iterations=EDGE_GROW) & ~(~edge & ~sky)
        rows = np.nonzero(sky.any(axis=1))[0]
        if rows.size and rows.max() < SKY_FLOOR * h:
            return sky, step
    raise ValueError("no sky: the flood reached the ground at every edge threshold")


def _fill_from_ground(sky):
    """Holes in `sky` are what the ground (bottom edge) can't reach, as in
    sky.solidify(): a moon cut by the top edge (orion) is still a hole."""
    padded = np.pad(sky, ((1, 0), (1, 1)), constant_values=True)
    return ndi.binary_fill_holes(padded)[1:, 1:-1]


def _circles(points, rng, r_range=MOON_R, candidates=MOON_CANDIDATES):
    """Up to `candidates` circles by sequential RANSAC: (cx, cy, r, votes)."""
    found = []
    for _ in range(candidates):
        if len(points) < 30:
            break
        best = (0, 0.0, 0.0, 0.0)
        for _ in range(MOON_TRIES):
            a, b, c = points[rng.choice(len(points), 3, replace=False)]
            m = 2.0 * np.array([b - a, c - a])
            if abs(np.linalg.det(m)) < 1e-6:
                continue
            cx, cy = np.linalg.solve(m, [b @ b - a @ a, c @ c - a @ a])
            r = math.hypot(*(a - (cx, cy)))
            if not r_range[0] <= r <= r_range[1]:
                continue
            votes = int((np.abs(np.hypot(*(points - (cx, cy)).T) - r) < 2.0).sum())
            if votes > best[0]:
                best = (votes, cx, cy, r)
        votes, cx, cy, r = best
        if not votes:
            break
        found.append((cx, cy, r, votes))
        points = points[np.abs(np.hypot(*(points - (cx, cy)).T) - r) > 4.0]
    return found


def _support(circle, sky):
    """Votes per px of the circle's arc that lies in the frame's sky."""
    cx, cy, r, votes = circle
    h, w = sky.shape
    th = np.linspace(0.0, 2.0 * math.pi, 360, endpoint=False)
    x, y = (cx + r * np.cos(th)).astype(int), (cy + r * np.sin(th)).astype(int)
    seen = (x >= 0) & (x < w) & (y >= 0) & (y < h)
    seen[seen] = sky[y[seen], x[seen]]
    return votes / max(seen.mean() * 2.0 * math.pi * r, 1.0)


def find_moons(rgb, sky, seed, retries=4):
    """-> (big, small) moons as (cx, cy, r); small may be None. RANSAC can
    miss a soft limb (orion's big moon is also cut by the top edge), so a
    miss retries with the next seeds; each hull's result is deterministic."""
    for k in range(retries):
        try:
            return _find_moons(rgb, sky, seed + 1000 * k)
        except ValueError:
            if k == retries - 1:
                raise


def _find_moons(rgb, sky, seed):
    # The limbs are edges, so they're outside the flood: fill its holes first.
    sky = _fill_from_ground(sky)
    inner = ndi.binary_erosion(sky, iterations=10)          # not the skyline
    ys, xs = np.nonzero((_gradient(rgb) > EDGE_STEPS[0]) & inner)
    points = np.stack([xs, ys], 1).astype(float)
    found = [(*c[:3], _support(c, sky)) for c in _circles(points, np.random.default_rng(seed))]
    # The moons hang high (big moon centres run y 96-203): a pair lower down
    # is hangar arches (paradigm).
    high = sky.shape[0] / 3
    best, pair = -1.0, None
    for big in [c for c in found if c[1] < high]:
        for small in found:
            ratio = small[2] / big[2]
            ox, oy = (small[0] - big[0]) / big[2], (small[1] - big[1]) / big[2]
            if (small is not big and PAIR_RATIO[0] < ratio < PAIR_RATIO[1] and
                    PAIR_OFFSET[0][0] < ox < PAIR_OFFSET[0][1] and
                    PAIR_OFFSET[1][0] < oy < PAIR_OFFSET[1][1] and big[3] > 0.4 and
                    small[3] > 0.12):
                score = min(big[3], 1.5) + min(small[3], 1.5)
                if score > best:
                    best, pair = score, (big[:3], small[:3])
    if pair:
        return pair
    # No partner found (paradigm): a lone, strongly supported, big circle high
    # in the frame is the big moon.
    lone = [c for c in found if c[3] > 1.0 and 100.0 <= c[2] <= 200.0 and c[1] < high]
    if not lone:
        raise ValueError("no moon found")
    big = max(lone, key=lambda c: c[3])[:3]
    return big, _small_moon_near(points, big, np.random.default_rng(seed))


def _small_moon_near(points, big, rng):
    """The small moon, searched for only where the pair puts it (its limb
    was out-voted by hangar arches in the frame-wide search), or None."""
    cx, cy, r = big
    px = cx + r * sum(PAIR_OFFSET[0]) / 2
    py = cy + r * sum(PAIR_OFFSET[1]) / 2
    near = points[(np.hypot(*(points - (px, py)).T) < r) &
                  (np.abs(np.hypot(*(points - (cx, cy)).T) - r) > 4.0)]   # not the big limb
    found = _circles(near, rng, (r * PAIR_RATIO[0], r * PAIR_RATIO[1]), candidates=1)
    return found[0][:3] if found and found[0][3] >= 0.3 * math.pi * found[0][2] else None


def sky_mask(sky, moons):
    """The flood with its holes filled, minus the moons (+MOON_MARGIN),
    rounded and feathered like sky.solidify(). The holes are painted clouds
    (drone) and specks: kilometres off, so a ship must pass IN FRONT of
    them, and the fill (the plate's own pixels) redraws them beneath it.
    Only the moons, and what reaches the ground, stay over the ship."""
    mask = _fill_from_ground(sky)
    yy, xx = np.mgrid[0:sky.shape[0], 0:sky.shape[1]]
    for moon in moons:
        if moon is not None:
            cx, cy, r = moon
            mask &= (xx - cx) ** 2 + (yy - cy) ** 2 > (r + MOON_MARGIN) ** 2
    img = Image.fromarray(mask.astype(np.uint8) * 255)
    rounded = np.asarray(img.filter(ImageFilter.GaussianBlur(2.0))) >= 128
    return Image.fromarray(rounded.astype(np.uint8) * 255).filter(ImageFilter.GaussianBlur(0.8))


def sky_fill(rgb, mask):
    """The plate's own sky at half size (box-averaged inside the mask, so no
    blur where it shows), extrapolated outward so the feathered edge and the
    bilinear stretch never pull in skyline or moon colour."""
    w = np.asarray(mask, np.float32) / 255.0
    h2, w2 = rgb.shape[0] // 2, rgb.shape[1] // 2

    def half(a):
        return a[:h2 * 2, :w2 * 2].reshape(h2, 2, w2, 2, *a.shape[2:]).mean(axis=(1, 3))
    num, den = half(rgb.astype(np.float32) * w[..., None]), half(w)[..., None]
    fill = np.where(den > 0.5, num / np.maximum(den, 1e-6), 0.0)
    for sigma in (2.0, 8.0, 32.0, 128.0):                  # grow outward, near first
        blur_n = np.dstack([ndi.gaussian_filter(num[..., c], sigma) for c in range(3)])
        blur_d = ndi.gaussian_filter(den[..., 0], sigma)[..., None]
        fill = np.where(den > 0.5, fill,
                        np.where(blur_d > 1e-3, blur_n / np.maximum(blur_d, 1e-6), fill))
        den = np.maximum(den, np.where(blur_d > 1e-3, 1.0, 0.0))
    return Image.fromarray(np.clip(fill, 0, 255).astype(np.uint8))


def bake_skies(debug):
    OUT.mkdir(parents=True, exist_ok=True)
    anchors, thumbs = {}, []
    for i, path in enumerate(sorted(COMPOSITES.glob("*.png"))):
        hull = path.stem
        rgb = np.asarray(Image.open(path).convert("RGB"))
        sky, step = find_sky(rgb)
        big, small = find_moons(rgb, sky, seed=593 + i)
        mask = sky_mask(sky, (big, small))
        mask.save(OUT / f"{hull}_mask.png", optimize=True)
        sky_fill(rgb, mask).save(OUT / f"{hull}_fill.png", optimize=True)
        anchors[hull] = [round(float(v), 1) for v in big]
        cover = np.mean(np.asarray(mask) > 127)
        print(f"{hull:<11} edge {step}  moon {anchors[hull]}  "
              f"small {'-' if small is None else [round(float(v)) for v in small]}  sky {cover:.1%}")
        if debug:
            m = (np.asarray(mask, np.float32) / 255.0)[..., None]
            vis = rgb * (1 - 0.5 * m) + np.array([0, 170, 255]) * 0.5 * m
            thumb = Image.fromarray(vis.astype(np.uint8))
            d = ImageDraw.Draw(thumb)
            for moon, colour in ((big, (255, 60, 60)), (small, (60, 255, 60))):
                if moon is not None:
                    cx, cy, r = moon
                    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=colour, width=5)
            d.text((12, 12), hull, fill=(255, 255, 0))
            thumbs.append(thumb.resize((512, 341)))
    (OUT / "anchors.json").write_text(json.dumps(anchors, indent=1) + "\n")
    if debug and thumbs:
        cols = 6
        sheet = Image.new("RGB", (cols * 512, math.ceil(len(thumbs) / cols) * 341))
        for i, t in enumerate(thumbs):
            sheet.paste(t, ((i % cols) * 512, (i // cols) * 341))
        Path(debug).parent.mkdir(parents=True, exist_ok=True)
        sheet.save(debug)


def bake_layers():
    """Rendered sky passes -> under-plate, anchored sprite sheets (as mining's)."""
    for layer, t in json.loads(TIMING.read_text()).items():
        if layer.startswith("_"):
            continue
        src = BUILD / layer
        info = json.loads((src / "pass.json").read_text())
        sprites, slots = [], []
        for path in sorted(src.glob("*.png")):
            baked = load_frame(path)
            if baked is not None:
                sprites.append(baked)
                slots.append(int(path.stem) - 1)          # frame N -> slot N-1
        fps = float(info["fps"])
        period_frames = max(int(info["frames"]), int(math.ceil(t["period"] * fps)))
        write_sheet(OUT, layer, sprites, slots, CANVAS, fps, period_frames,
                    int(round(t["offset"] * fps)), under=True, anchor=info["anchor"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="contact sheet of every detected sky and moon")
    ap.add_argument("--layers-only", action="store_true", help="just bake the sky layers")
    args = ap.parse_args()
    if not args.layers_only:
        bake_skies(args.debug)
    if TIMING.exists():
        bake_layers()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "scipy"]
# ///
"""Shimmer on the sea and beacons on the towers at the Pleasure landing pad (#606).

The original game's Pleasure landing pad had two small ambient beats beside
the ship traffic (#595): `wtr`, a water shimmer on the sea, and `blt`, tiny
blinking lights. Both come back as light added to the painting, made in 2D
like the military bay's lamps (no Blender):

- **Shimmer.** The painted ripple crests catch the sunset in waves rolling
  toward the camera, and glints flare up on them, drift a few px with the
  wind and fade, all over the open sea.
- **Beacons.** A red obstruction light on each tower's top (the left roof's
  peak, the right tower's tilted panel); the two flash in turn.

The 18 composites aren't one painting slid up and down: the towers' tops sit
up to ~60 px apart relative to the horizon, and every parked hull covers a
different stretch of sea. One anchored sheet can't land on all of them, so
each composite gets its own two sheets, found in and encoded against its own
paint (bake_layer.encode(): the engine's plain alpha-over reproduces the
added light exactly). The room names them with {plate}, like the military
bay's lamps (room_anim_data.cpp for_plate()).

Reads the sky masks and anchors bake_landing.py writes (run it first).
Outputs (assets/concourse/pleasure/anim/landing/):
    <hull>_shimmer.{png,json}   the sea's shimmer and glints, one loop per composite
    <hull>_beacons.{png,json}   the towers' beacons, one loop per composite

Usage (from the repo root):
    uv run tools/room_anim/pleasure/bake_landing_ambient.py \\
        [--debug build/room_anim/pleasure/ambient.png]
"""
import argparse
import json
import math
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage, signal

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from bake_layer import encode, to_linear, to_srgb, write_sheet  # noqa: E402
from sky import contact_sheet  # noqa: E402

PLEASURE = paths("pleasure")
COMPOSITES = PLEASURE.room / "landing_ships"
OUT = PLEASURE.anim / "landing"

# The sea: blue-purple like the sky, below bake_landing.py's horizon anchor
# and outside the sky mask. Where the far sea is glassy the anchor sits up to
# ~70 px below the painted horizon, but glassy sea has no ripples to light,
# and above the anchor the sky's unmasked slivers (along the towers' edges,
# over the islands) would pass for sea. The islands (brown), hulls (grey) and
# towers (dark) fail the colour test. The pad's puddles and some hulls' livery
# pass it, so only stretches of sea that reach up to within SEA_REACH px of
# the anchor count: the open sea runs back to the horizon, they don't.
SEA_BLUE = 35                   # blue over green, 8-bit
SEA_MIN_BLUE = 70
SEA_REACH = 60                  # px below the anchor; puddles start 100+ down
SEA_MIN_PX = 1500
SEA_INSET = 3                   # px kept off the sea's painted edges
SEA_EDGE = 3                    # px: the mask's soft edge, which fades light off the sea

# Crests: the painted ripple highlights, brighter than their CREST_WINDOW px
# neighbourhood by CREST (strength 0) to CREST + CREST_SPAN (strength 1).
# Both beats follow the paint's own waves. A shoreline (the sea against an
# island's dark reflection) passes that test all along its length, so crests
# fade out where the paint's large-scale slope (over SHORE_SCALE px) nears
# SHORE_SLOPE: ripples are fine detail, shores aren't.
CREST = 6
CREST_SPAN = 20
CREST_WINDOW = 9
SHORE_SCALE = 3.0               # px, gaussian sigma
SHORE_SLOPE = 5.0               # 8-bit luminance per px

# The shimmer: every crest brightens and dims with its strength, in waves
# rolling toward the camera (SHIMMER_ROLL px apart) through a smooth random
# phase, so it twinkles rather than marching in bands. SHIMMER_CYCLES per
# loop keeps it seamless.
SHIMMER_GAIN = 0.35             # linear light at a full-strength crest's peak
SHIMMER_CYCLES = 3
SHIMMER_ROLL = 24.0             # px
SHIMMER_GRAIN = 6.0             # px, the phase noise's blur
SHIMMER_SHARP = 3.0             # exponent on the (0..1) wave: brief peaks
SHIMMER_SCATTER = 1.5           # cycles of phase noise (one sigma)

# Glints. Each one flares (sin^2) over its life while drifting with the
# wind. Far glints are specks, near ones short streaks flattened by the
# grazing angle: sizes scale with depth, 0 at the sea's top row to 1 at its
# bottom. The loop is seamless: a glint's clock wraps at LOOP.
LOOP = 4.0                      # s, the shimmer's and the glints'
SEA_FPS = 8                     # every frame spans the whole sea: keep the atlas modest
GLINT_LIFE = (0.35, 0.8)        # s
GLINT_DENSITY = 6.0             # alive at once, per 10k px of sea
GLINT_DRIFT = 6.0               # px/s, to the right
GLINT_SIGMA = ((0.8, 0.5), (3.0, 1.0))    # (x, y) px: far, near
GLINT_GAIN = (0.5, 1.3)         # linear-light peak, random per glint
SEA_RGB = np.array([1.0, 0.78, 0.92])     # the sunset's pink-white, linear

# Beacons sit on each tower's top: the left tower's roof peak and the right
# one's tilted panel, picked by eye on tarsus. Every composite frames the
# towers differently (up to ~60 px apart relative to the horizon, and the
# roof's slope changes), and the sky mask is no help (moons and painted specks
# break its silhouettes), so each top is found by matching a patch of
# tarsus's blue channel (dark tower on blue sky, whatever the sky's tint)
# within SEARCH px.
CANONICAL = "tarsus"
TOWER_TOPS = ((100, 196), (1450, 221))   # tarsus px: left roof peak, right panel corner
PATCH = (40, 30)                # half-size of the matched patch, px
SEARCH = 130                    # px either way
MIN_MATCH = 0.6                 # normalised cross-correlation
BEACON_CYCLE = 2.5              # s; the right beacon flashes half a cycle after the left
BEACON_FPS = 12                 # the flare's rise is 0.08 s
ATTACK, DECAY = 0.08, 0.22      # s: flare up, then e-folding fade
BEACON_RED = np.array([1.0, 0.1, 0.04])   # linear
# Glow per unit flare, as gaussians: a hot core, a halo, a faint wide bloom.
BEACON_GLOW = ((2.0, 2.5), (6.0, 0.8), (16.0, 0.18))   # (sigma px, gain)
BEACON_REACH = 40               # px, the box a beacon's glow is visible in (2.5 bloom sigmas)


def _load(hull):
    plate = np.asarray(Image.open(COMPOSITES / f"{hull}.png").convert("RGB"))
    sky = np.asarray(Image.open(OUT / f"{hull}_mask.png")) > 127
    return plate, sky


def find_sea(plate, sky, horizon):
    """-> bool mask of the open sea (no hull, island, tower or pad on it)."""
    rgb = plate.astype(np.int32)
    sea = (rgb[..., 2] > SEA_MIN_BLUE) & (rgb[..., 2] - rgb[..., 1] > SEA_BLUE)
    sea &= ~ndimage.binary_dilation(sky, iterations=4)
    sea[:math.ceil(horizon)] = False
    sea = ndimage.binary_opening(sea, iterations=2)
    labels, _ = ndimage.label(sea)
    keep = [i + 1 for i, (rows, _) in enumerate(ndimage.find_objects(labels))
            if rows.start <= horizon + SEA_REACH
            and (labels[rows] == i + 1).sum() >= SEA_MIN_PX]
    sea = ndimage.binary_erosion(np.isin(labels, keep), iterations=SEA_INSET)
    if not sea.any():
        raise ValueError("no open sea found")
    return sea


def find_crests(plate, sea):
    """-> crest strength 0..1 (float32), zero off the sea."""
    lum = plate.astype(np.float32).mean(axis=2)
    rise = lum - ndimage.uniform_filter(lum, CREST_WINDOW)
    shore = ndimage.gaussian_gradient_magnitude(lum, SHORE_SCALE) / SHORE_SLOPE
    return (np.clip((rise - CREST) / CREST_SPAN, 0.0, 1.0)
            * np.clip(1.0 - shore, 0.0, 1.0) * sea)


def find_beacons(plate, reference):
    """-> [(x, y)] on each tower's top, left first: TOWER_TOPS on `reference`
    (the canonical composite's RGB) matched into `plate`."""
    blue, ref = (np.asarray(p[..., 2], dtype=np.float64) for p in (plate, reference))
    h, w = blue.shape
    px, py = PATCH
    beacons = []
    for x, y in TOWER_TOPS:
        t = ref[y - py:y + py, x - px:x + px]
        t = t - t.mean()
        x0, y0 = max(0, x - px - SEARCH), max(0, y - py - SEARCH)
        win = blue[y0:min(h, y + py + SEARCH), x0:min(w, x + px + SEARCH)]
        ones = np.ones_like(t)
        num = signal.fftconvolve(win, t[::-1, ::-1], mode="valid")
        s = signal.fftconvolve(win, ones, mode="valid")
        s2 = signal.fftconvolve(win * win, ones, mode="valid")
        var = np.maximum(s2 - s * s / t.size, 1e-6)
        ncc = num / np.sqrt(var * (t * t).sum())
        iy, ix = np.unravel_index(np.argmax(ncc), ncc.shape)
        if ncc[iy, ix] < MIN_MATCH:
            raise ValueError(f"tower top near {(x, y)} not found (best {ncc[iy, ix]:.2f})")
        beacons.append((float(x0 + ix + px), float(y0 + iy + py)))
    return beacons


def _box(xs, ys, pad, shape):
    """Plate crop (x0, y0, x1, y1) around the points `xs`, `ys`, `pad` px out."""
    h, w = shape[:2]
    return (max(0, int(np.min(xs)) - pad), max(0, int(np.min(ys)) - pad),
            min(w, int(np.max(xs)) + pad + 1), min(h, int(np.max(ys)) + pad + 1))


def _sheet(hull, kind, plate8, glow_at, fps, frames, box):
    """Encode `glow_at(t s) -> linear light over the crop box` (None = dark)
    over the plate for each of `frames` loop slots at `fps`, each sprite
    cropped to what it changes."""
    plate = plate8.astype(np.float32) / 255.0
    x0, y0, x1, y1 = box
    crop = plate[y0:y1, x0:x1]
    lin = to_linear(crop)
    sprites, slots = [], []
    for f in range(frames):
        light = glow_at(f / fps)
        if light is None:
            continue
        rgba = encode(crop, to_srgb(lin + light))
        ys, xs = np.nonzero(rgba[..., 3])
        if xs.size == 0:
            continue
        tx0, ty0, tx1, ty1 = xs.min(), ys.min(), xs.max() + 1, ys.max() + 1
        sprites.append((rgba[ty0:ty1, tx0:tx1],
                        [x0 + int(tx0), y0 + int(ty0), int(tx1 - tx0), int(ty1 - ty0)]))
        slots.append(f)
    h, w = plate.shape[:2]
    write_sheet(OUT, f"{hull}_{kind}", sprites, slots, (w, h), fps, frames, 0)


def _glints(rng, sea, crests):
    """-> (x, y, depth 0..1, start s, life s, gain) for one loop's glints."""
    ys = np.nonzero(sea.any(axis=1))[0]
    top, bottom = ys.min(), ys.max()
    sites = np.argwhere(crests > 0) if (crests > 0).any() else np.argwhere(sea)
    alive = GLINT_DENSITY * sea.sum() / 10_000.0
    count = max(1, round(alive * LOOP / np.mean(GLINT_LIFE)))
    pick = sites[rng.integers(len(sites), size=count)]
    depth = (pick[:, 0] - top) / max(1, bottom - top)
    return np.column_stack([pick[:, 1], pick[:, 0], depth,
                            rng.uniform(0.0, LOOP, count),
                            rng.uniform(*GLINT_LIFE, count),
                            rng.uniform(*GLINT_GAIN, count)])


def _phase(rng, shape):
    """Shimmer phase per pixel: smooth noise plus waves rolling down the screen."""
    noise = ndimage.gaussian_filter(rng.standard_normal(shape), SHIMMER_GRAIN)
    noise = (noise - noise.mean()) / max(noise.std(), 1e-9)
    rows = np.arange(shape[0], dtype=np.float64)[:, None] / SHIMMER_ROLL
    return 2.0 * math.pi * (SHIMMER_SCATTER * noise + rows)


def _glint(light, x, y, depth, env):
    """Add one glint at (x, y) in `light`'s pixels, peak `env`."""
    (fx, fy), (nx, ny) = GLINT_SIGMA
    sx, sy = fx + (nx - fx) * depth, fy + (ny - fy) * depth
    h, w = light.shape
    gx0, gx1 = max(0, int(x - 3 * sx)), min(w, int(x + 3 * sx) + 2)
    gy0, gy1 = max(0, int(y - 3 * sy)), min(h, int(y + 3 * sy) + 2)
    yy, xx = np.mgrid[gy0:gy1, gx0:gx1].astype(np.float32)
    light[gy0:gy1, gx0:gx1] += env * np.exp(-0.5 * (((xx - x) / sx) ** 2 + ((yy - y) / sy) ** 2))


def bake_shimmer(hull, plate8, sea, crests):
    rng = np.random.default_rng(zlib.crc32(hull.encode()))
    glints = _glints(rng, sea, crests)
    ys, xs = np.nonzero(sea)
    box = _box(xs, ys, SEA_EDGE, sea.shape)
    x0, y0, x1, y1 = box
    soft = ndimage.gaussian_filter(sea.astype(np.float32), SEA_EDGE / 3)[y0:y1, x0:x1]
    strength = crests[y0:y1, x0:x1]
    phase = _phase(rng, strength.shape)

    def glow_at(t):
        wave = (0.5 + 0.5 * np.sin(phase - 2.0 * math.pi * SHIMMER_CYCLES * t / LOOP))
        light = (SHIMMER_GAIN * strength * wave ** SHIMMER_SHARP).astype(np.float32)
        for x, y, depth, start, life, gain in glints:
            u = ((t - start) % LOOP) / life
            if u < 1.0:
                _glint(light, x + GLINT_DRIFT * u * life - x0, y - y0, depth,
                       gain * math.sin(math.pi * u) ** 2)
        return (light * soft)[..., None] * SEA_RGB

    _sheet(hull, "shimmer", plate8, glow_at, SEA_FPS, round(LOOP * SEA_FPS), box)
    return glints


def flare(t):
    """Flare 0..1 of a beacon `t` s after its cue (quick rise, exponential fade)."""
    if t < 0:
        return 0.0
    if t < ATTACK:
        return t / ATTACK
    return math.exp(-(t - ATTACK) / DECAY)


def bake_beacons(hull, plate8, beacons):
    box = _box([x for x, _ in beacons], [y for _, y in beacons], BEACON_REACH, plate8.shape)
    x0, y0, x1, y1 = box

    def glow_at(t):
        lit = [(b, flare((t - i * BEACON_CYCLE / len(beacons)) % BEACON_CYCLE))
               for i, b in enumerate(beacons)]
        lit = [(b, e) for b, e in lit if e > 0.02]
        if not lit:
            return None
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        light = np.zeros((y1 - y0, x1 - x0), np.float32)
        for (bx, by), e in lit:
            d2 = (xx - bx) ** 2 + (yy - by) ** 2
            for sigma, gain in BEACON_GLOW:
                light += e * gain * np.exp(-0.5 * d2 / sigma ** 2)
        return light[..., None] * BEACON_RED

    _sheet(hull, "beacons", plate8, glow_at, BEACON_FPS, round(BEACON_CYCLE * BEACON_FPS), box)


def _debug_thumb(hull, plate8, sea, glints, beacons):
    vis = plate8.astype(np.float32)
    vis[sea] = vis[sea] * 0.5 + np.array([0, 255, 120]) * 0.5
    im = Image.fromarray(vis.astype(np.uint8))
    d = ImageDraw.Draw(im)
    for x, y, *_ in glints:
        d.point((x, y), fill=(255, 255, 255))
    for x, y in beacons:
        d.ellipse([x - 14, y - 14, x + 14, y + 14], outline=(255, 40, 40), width=4)
    d.text((12, 12), hull, fill=(255, 255, 0))
    return im.resize((384, 256))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="contact sheet: sea (green), glint sites, beacons (red)")
    ap.add_argument("--only", help="bake just this composite")
    args = ap.parse_args()
    anchors = json.loads((OUT / "anchors.json").read_text())
    reference = _load(CANONICAL)[0]
    thumbs = []
    for path in sorted(COMPOSITES.glob("*.png")):
        hull = path.stem
        if args.only and hull != args.only:
            continue
        plate8, sky = _load(hull)
        sea = find_sea(plate8, sky, anchors[hull][1])
        crests = find_crests(plate8, sea)
        beacons = find_beacons(plate8, reference)
        glints = bake_shimmer(hull, plate8, sea, crests)
        bake_beacons(hull, plate8, beacons)
        print(f"{hull:<11} sea {sea.sum():6d} px, {int((crests > 0).sum()):5d} crests, "
              f"{len(glints)} glints; beacons "
              + ", ".join(f"({x:.0f}, {y:.0f})" for x, y in beacons))
        if args.debug:
            thumbs.append(_debug_thumb(hull, plate8, sea, glints, beacons))
    if args.debug:
        contact_sheet(thumbs, args.debug, cols=3)


if __name__ == "__main__":
    main()

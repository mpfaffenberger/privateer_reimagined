#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Drifting dusk clouds through the Agricultural concourse windows (#582).

The original game's agricultural concourse scrolled a band of cloud streaks
across its window view (the legacy `concourse_wtr` overlay). The painting's
windows show a dusk sky of long horizontal cloud streaks over farmland, so
the painted streaks themselves drift.

The engine's sky can only lay tiles over a static fill, with plain alpha-over,
so the painting is split into
    fill   the sky with its cloud streaks lifted out: a horizontal upper
           envelope of the painted sky (the glow between the streaks), with
           the painted stars put back so they stay put,
    tiles  the streaks, as darkening: per row one colour (the row's darkest
           cloud tone) whose alpha pulls the envelope down to the painting.
           Rows never move (vertical velocity 0), so a per-row colour is
           allowed; darkening is a ratio, so the same streak reads the same
           over the brighter right-hand pane.
At t=0 fill + tiles reproduce the painting; as the tiles scroll the streaks
drift. Two tiles give parallax: the higher streaks (overhead, nearer) move
faster than the low ones by the horizon.

Tile columns map to plate columns as x mod TILE_W, so the painted streaks of
the centre and right panes (x 620..1480) are laid side by side, and gaps
(mullion, frame edges) are filled by mirroring their neighbours.

Outputs (assets/concourse/agricultural/anim/):
    sky_mask.png     L8, 255 = drifting sky (ramped out toward the top, where
                     the painted stars are, and above the far ridges)
    sky_fill.png     the envelope, full resolution (the vertical glow
                     gradient is steep; a stretched quarter-res fill blurs it)
    clouds_high.png / clouds_low.png   the streak tiles

Usage (from the repo root):
    uv run tools/room_anim/agricultural/bake_sky.py [--debug build/room_anim/agricultural/sky_debug.png]
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent)]         # this base's modules, then shared ones
from base import paths  # noqa: E402
from sky import star_removed  # noqa: E402
from windows import glass  # noqa: E402

AGRI = paths("agricultural")

FRAME_LUM = 42          # star-removed luminance below this is window frame
# Where the drift fades out: in from the dark, starry top of the sky, and
# out above the horizon, measured on the plate. The far pale ridges in the
# right pane rise to y ~152; the centre pane is clean down to the hills and
# the farm dome at y ~180.
TOP = (45.0, 90.0)                  # mask 0 above y 45, 1 below y 90
HORIZON = [(590, 176), (1096, 176), (1118, 150), (1300, 150), (1360, 154), (1470, 158)]
HORIZON_FEATHER = 14.0              # px of fade above the horizon line

TILE_W, TILE_H = 860, 256           # tile x = plate x mod 860 covers x 620..1480
ENV_WIN = 31                        # px: running max for the envelope
ENV_SIGMA = 12.0                    # px: then a horizontal blur
SPLIT_Y, SPLIT_FEATHER = 118.0, 24.0  # high/low cloud tiles cross-fade here
STAR_SKY_LUM = 90                   # painted stars sit in sky darker than this
SPAN_MIN = 0.06                     # envelope - dark tone below this: no streak


def luma(rgb):
    return rgb @ np.array([0.299, 0.587, 0.114], np.float32)


def hblur(a, weight, sigma):
    """Horizontal, weight-normalised gaussian blur of (H, W[, C]) arrays."""
    r = int(3 * sigma)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2).astype(np.float32)

    def conv(x):
        pad = np.pad(x, ((0, 0), (r, r)) + ((0, 0),) * (x.ndim - 2))
        return sum(k[i] * pad[:, i:i + x.shape[1]] for i in range(k.size))
    den = conv(weight)
    if a.ndim == 3:
        weight, den = weight[..., None], den[..., None]
    return conv(a * weight) / np.maximum(den, 1e-6), den[..., 0] if a.ndim == 3 else den


def running_max(a, valid, width):
    """Horizontal running max of (H, W, C) over `valid` pixels only."""
    pad = width // 2
    x = np.where(valid[..., None], a, -1.0)
    x = np.pad(x, ((0, 0), (pad, pad), (0, 0)), constant_values=-1.0)
    win = np.lib.stride_tricks.sliding_window_view(x, width, axis=1)
    return win.max(axis=-1)


def glass_mask(plate):
    """Binary window glass (windows.py), less anything as dark as a frame."""
    lumr = np.asarray(star_removed(plate.convert("L")), np.float32)
    clear = (glass(plate.size) > 0.5) & (lumr >= FRAME_LUM)
    # Opening drops speckle along the frame bevels.
    img = Image.fromarray(clear.astype(np.uint8) * 255)
    return np.asarray(img.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MaxFilter(5))) > 0


def drift_profile(h, w):
    """How much of each pixel drifts, by height: 0 at the starry top and
    at the horizon, 1 in the cloud band between."""
    y = np.arange(h, dtype=np.float32)[:, None]
    top = np.clip((y - TOP[0]) / (TOP[1] - TOP[0]), 0.0, 1.0)
    hx, hy = zip(*HORIZON)
    horizon = np.interp(np.arange(w), hx, hy).astype(np.float32)[None, :]
    return top * np.clip((horizon - y) / HORIZON_FEATHER, 0.0, 1.0)


def painted_stars(plate):
    grey = plate.convert("L")
    lum = np.asarray(grey, np.float32)
    base = np.asarray(star_removed(grey), np.float32)
    peak = lum >= np.asarray(grey.filter(ImageFilter.MaxFilter(3)), np.float32)
    # Only in the dark upper sky: bright specks in the glow band are cloud
    # texture and drift with it.
    stars = Image.fromarray(((lum - base > 18) & peak & (base < STAR_SKY_LUM)).astype(np.uint8)
                            * 255)
    return np.asarray(stars.filter(ImageFilter.MaxFilter(5))) > 0


def fill_gaps(row, valid):
    """Fill invalid runs of a cyclic 1-D row by cross-fading copies of the
    valid samples on either side, shifted by the run length (a shift keeps
    a slanted streak's slant; a mirror image drew an X at every gap)."""
    n = row.size
    if valid.all() or not valid.any():
        return np.where(valid, row, 0.0)
    out = row.copy()
    start = int(np.argmax(valid & ~np.roll(valid, -1))) + 1   # first gap after a valid run
    i = 0
    while i < n:
        j = (start + i) % n
        if valid[j]:
            i += 1
            continue
        run = 0
        while run < n and not valid[(j + run) % n]:
            run += 1
        for k in range(run):
            left = row[(j + k - run) % n] if valid[(j + k - run) % n] else row[(j - 1) % n]
            right = row[(j + k + run) % n] if valid[(j + k + run) % n] else row[(j + run) % n]
            wr = (k + 0.5) / run
            out[(j + k) % n] = (1 - wr) * left + wr * right
        i += run
    return out


def bake(plate):
    rgb = np.asarray(plate, np.float32) / 255.0
    h, w = rgb.shape[:2]
    clear = glass_mask(plate)
    profile = drift_profile(h, w)
    stars = painted_stars(plate) & clear

    # Streaks without stars: the stars stay painted in the fill.
    smooth = np.dstack([np.asarray(star_removed(Image.fromarray(np.asarray(plate)[..., c])),
                                   np.float32) / 255.0 for c in range(3)])
    clean = np.where(stars[..., None], smooth, rgb)

    env, weight = hblur(running_max(clean, clear, ENV_WIN), clear.astype(np.float32), ENV_SIGMA)
    env = np.maximum(env, np.where(clear[..., None], clean, 0.0))

    # Per row: the darkest cloud tone across both panes.
    lum_clean = luma(clean)
    dark = np.zeros((h, 3), np.float32)
    for y in range(TILE_H):
        sel = clear[y] & (profile[y] > 0)
        if sel.sum() > 20:
            order = np.argsort(lum_clean[y, sel])
            dark[y] = clean[y, sel][order[: max(1, order.size // 30)]].mean(axis=0) * 0.9
    lum_env, lum_dark = luma(env), luma(dark)[:, None]
    span = lum_env - lum_dark
    alpha = np.clip((lum_env - lum_clean) / np.maximum(span, 1e-6), 0.0, 1.0)
    # Where the envelope is barely above the dark tone (the dim upper sky)
    # the ratio is brush noise; fade it out rather than drift grain.
    alpha *= np.clip((span - SPAN_MIN) / SPAN_MIN, 0.0, 1.0)

    # Tile: plate columns 620..1480 laid out as x mod TILE_W.
    inner = np.asarray(Image.fromarray(clear.astype(np.uint8) * 255)
                       .filter(ImageFilter.MinFilter(7))) > 0     # clear of frame bevels
    xs = np.arange(620, 620 + TILE_W)
    cols = xs % TILE_W
    tile_a = np.zeros((TILE_H, TILE_W), np.float32)
    for y in range(TILE_H):
        row, valid = np.zeros(TILE_W, np.float32), np.zeros(TILE_W, bool)
        row[cols], valid[cols] = alpha[y, xs], inner[y, xs] & (profile[y, xs] > 0)
        tile_a[y] = fill_gaps(row, valid)
    y = np.arange(TILE_H, dtype=np.float32)[:, None]
    high = np.clip((SPLIT_Y + SPLIT_FEATHER / 2 - y) / SPLIT_FEATHER, 0.0, 1.0)
    colour = np.repeat(dark[:TILE_H, None, :], TILE_W, axis=1)

    def tile(a):
        return Image.fromarray((np.dstack([colour, a]) * 255 + 0.5).astype(np.uint8), "RGBA")

    mask_img = Image.fromarray((clear * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8))
    mask = np.asarray(mask_img, np.float32) / 255.0 * profile
    fill = np.where((weight > 0.05)[..., None], env, 0.0)
    # Painted stars stay put in the fill, brightened so that at t=0 the
    # streaks passing over them darken them back to the painting.
    a = alpha[..., None]
    star_fill = (rgb - dark[:, None, :] * a) / np.maximum(1.0 - a, 1e-3)
    fill = np.where(stars[..., None], star_fill, fill)
    return {
        "sky_mask": Image.fromarray((mask * 255 + 0.5).astype(np.uint8)),
        "sky_fill": Image.fromarray((np.clip(fill, 0, 1) * 255 + 0.5).astype(np.uint8)),
        "clouds_high": tile(tile_a * high),
        "clouds_low": tile(tile_a * (1.0 - high)),
    }, mask, alpha


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="write a mask + streak-alpha visualisation here")
    args = ap.parse_args()
    plate = Image.open(AGRI.plate).convert("RGB")
    images, mask, alpha = bake(plate)
    AGRI.anim.mkdir(parents=True, exist_ok=True)
    for name, img in images.items():
        img.save(AGRI.anim / f"{name}.png", optimize=True)
    print(f"drifting sky {np.mean(mask > 0.5):.1%} of plate; streak alpha median "
          f"{np.median(alpha[mask > 0.5]):.2f}")
    if args.debug:
        vis = np.asarray(plate, np.float32)
        m = mask[..., None]
        vis = vis * (1 - 0.5 * m) + np.array([0, 170, 255]) * 0.5 * m
        top = Image.fromarray(vis.astype(np.uint8)).crop((0, 0, 1536, 260))
        streaks = Image.fromarray((alpha * mask * 255).astype(np.uint8)).crop((0, 0, 1536, 260))
        sheet = Image.new("RGB", (1536, 520))
        sheet.paste(top, (0, 0))
        sheet.paste(streaks.convert("RGB"), (0, 260))
        Path(args.debug).parent.mkdir(parents=True, exist_ok=True)
        sheet.save(args.debug)


if __name__ == "__main__":
    main()

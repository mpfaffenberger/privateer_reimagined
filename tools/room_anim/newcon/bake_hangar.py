#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Find the hangar mouth in every New Con landing composite (#553).

The landing room shows one of 18 full-frame composites (one per player hull,
assets/concourse/newcon/landing_ships/<ship>.png), each framed differently.
In each, the hangar mouth is a dark, star-filled disc ringed by lit tunnel
walls. For every composite this writes to assets/concourse/newcon/anim/hangar/:

    <ship>_mask.png   L8, 255 = open space seen through the mouth
    <ship>_fill.png   the starless space colour (small, drawn stretched)
    anchors.json      {"<ship>": [cx, cy, r]} mouth centre/radius, plate px
    <ship>_stars_far.png, _near.png
                      star tiles drawn from the stars painted in that mouth

The engine draws stars (and far-off ship traffic) behind the masked plate, and
maps mouth-anchored sprite layers onto [cx, cy, r].

Usage (from the repo root):
    uv run tools/room_anim/newcon/bake_hangar.py [--debug build/room_anim/newcon/mouths.png]
    uv run tools/room_anim/newcon/bake_hangar.py --stars-only     # just the star tiles
"""
import argparse
import json
import math
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from base import paths  # noqa: E402
from sky import RNG_SEED, PaintedStars, sky_fill, solidify, star_removed, star_tile  # noqa: E402

NEWCON = paths("newcon")
COMPOSITES = NEWCON.room / "landing_ships"
OUT = NEWCON.anim / "hangar"

SPACE_DARKNESS = 30      # star-removed luminance at or below this is open space
SEED_REGION = (0.2, 0.05, 0.8, 0.6)   # where to look for the mouth (fractions of W/H)
MIN_MOUTH_FRACTION = 0.01             # sanity: the mouth covers at least 1% of frame
RIM_ROWS = 120           # top rows of the flood used to fit the rim circle
DISC_SLACK = 1.03        # keep flood pixels within this multiple of the fitted radius
HAZE_OPEN = 11           # px; stars smaller than this never count as haze
# Tile stars are sub-pixel dots, so many faint ones fall under PaintedStars'
# detection threshold: at equal density the animated field MEASURES sparser
# than the paint, by an amount that depends on each painting's brightness
# mix. So bake_stars() closes the loop per composite: build the composite,
# measure it like the paint, and correct the density until they agree.
STAR_DENSITY_GAIN = 4.0          # first guess
CALIBRATION_STEPS = 4
CALIBRATION_TOLERANCE = 0.1      # stop within 10% of the painted density
MIN_PAINTED_STARS = 8            # fewer than this: borrow brightness/colour from the pool
# Deliberate deviation from the paint: a couple of mouths are painted almost
# starless (drone 0.3/10k px), where a spinning field would be invisible.
MIN_STAR_DENSITY = 2.0e-4
# The paintings' few big, glowing blue stars: anything whose brightest
# channel is painted at least this far above the sky is ALSO redrawn as a
# soft glow in its own "hero" tile (the point tiles keep their calibrated
# density), so it reads as the painting's hero stars do.
HERO_CONTRAST = 150.0 / 255.0
HERO_SIGMA = (1.4, 2.0)          # ~5 px glows, like the painted ones
HERO_CORE_SIGMA = (0.45, 0.65)
HERO_GAIN = 1.1


def fit_rim(flood):
    """Circle through the left/right edges of the flood's top RIM_ROWS rows
    (Kasa least-squares fit). The top arc of the mouth is clean in every
    composite; lower down, dark hull shadow can join the flood (drayman)."""
    ys, xs = np.nonzero(flood)
    top = ys.min()
    pts = []
    for y in range(top, min(top + RIM_ROWS, flood.shape[0])):
        row = np.nonzero(flood[y])[0]
        if row.size:
            pts += [(row.min(), y), (row.max() + 1, y)]
    p = np.asarray(pts, dtype=np.float64)
    a = np.column_stack([p[:, 0], p[:, 1], np.ones(len(p))])
    b = -(p[:, 0] ** 2 + p[:, 1] ** 2)
    d, e, f = np.linalg.lstsq(a, b, rcond=None)[0]
    cx, cy = -d / 2.0, -e / 2.0
    return cx, cy, math.sqrt(cx * cx + cy * cy - f)


def haze_ramp(mask, base):
    """Soften the mask where painted haze (the light shaft) fades into space.
    A hard threshold cuts the haze's faint tail off in a jagged line; instead
    the mask falls from 1 at the space level S to 0 at SPACE_DARKNESS, so it
    meets the unmasked plate continuously. Over the fill F the plate shows
    a*P + (1-a)*F, at most (T-S)/4 levels darker than paint: invisible."""
    m = np.asarray(mask, dtype=np.float32) / 255.0
    # Haze is broad; stars are not. An 11 px opening removes even the big
    # painted stars (which survive star_removed's 5 px), so they aren't
    # mistaken for haze and left showing, frozen, through the mask.
    lum = np.asarray(Image.fromarray(base.astype(np.uint8)).filter(ImageFilter.MinFilter(HAZE_OPEN))
                     .filter(ImageFilter.MaxFilter(HAZE_OPEN)).filter(ImageFilter.GaussianBlur(2)),
                     dtype=np.float32)
    space = float(np.percentile(lum[m > 0.99], 10))
    ramp = np.clip((SPACE_DARKNESS - lum) / max(SPACE_DARKNESS - space, 1.0), 0.0, 1.0)
    return Image.fromarray(np.round(m * ramp * 255.0).astype(np.uint8))


def find_mouth(plate):
    """-> (mask L image, [cx, cy, r]) for one composite."""
    w, h = plate.size
    base = np.asarray(star_removed(plate.convert("L")), dtype=np.float32)
    dark = Image.fromarray((base <= SPACE_DARKNESS).astype(np.uint8) * 255)
    dark = dark.filter(ImageFilter.MinFilter(7)).filter(ImageFilter.MaxFilter(7))

    # Seed = the darkest spot of a heavily blurred plate inside the region
    # where the mouth can be; flood only its connected dark component.
    blurred = np.asarray(Image.fromarray(base.astype(np.uint8))
                         .filter(ImageFilter.BoxBlur(24)), dtype=np.float32)
    x0, y0, x1, y1 = (int(SEED_REGION[0] * w), int(SEED_REGION[1] * h),
                      int(SEED_REGION[2] * w), int(SEED_REGION[3] * h))
    region = blurred[y0:y1, x0:x1]
    sy, sx = np.unravel_index(np.argmin(region), region.shape)
    seed = (int(x0 + sx), int(y0 + sy))
    if dark.getpixel(seed) != 255:
        raise ValueError(f"darkest seed {seed} is not open space")
    ImageDraw.floodfill(dark, seed, 200)
    flood = np.asarray(dark) == 200
    cx, cy, r = fit_rim(flood)
    yy, xx = np.mgrid[0:h, 0:w]
    inside = (xx - cx) ** 2 + (yy - cy) ** 2 <= (r * DISC_SLACK) ** 2
    mouth = Image.fromarray((flood & inside).astype(np.uint8) * 255)
    mask = haze_ramp(solidify(mouth, (w // 2, h - 1)), base)

    coverage = np.mean(np.asarray(mask) > 127)
    if coverage < MIN_MOUTH_FRACTION:
        raise ValueError(f"mouth too small ({coverage:.2%} of frame)")
    return mask, [round(cx, 1), round(cy, 1), round(r, 1)]


def bake_mouths(debug):
    OUT.mkdir(parents=True, exist_ok=True)
    anchors, thumbs = {}, []
    for path in sorted(COMPOSITES.glob("*.png")):
        plate = Image.open(path).convert("RGB")
        mask, anchor = find_mouth(plate)
        ship = path.stem
        mask.save(OUT / f"{ship}_mask.png", optimize=True)
        fill = sky_fill(plate, mask)
        fill.resize((fill.width // 2, fill.height // 2), Image.BOX).save(
            OUT / f"{ship}_fill.png", optimize=True)
        anchors[ship] = anchor
        print(f"{ship:<11} mouth c=({anchor[0]:.0f},{anchor[1]:.0f}) r={anchor[2]:.0f}  "
              f"covers {np.mean(np.asarray(mask) > 127):.1%}")
        if debug:
            vis = np.asarray(plate, dtype=np.float32)
            m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
            vis = vis * (1 - 0.55 * m) + np.array([0, 170, 255]) * 0.55 * m
            thumb = Image.fromarray(vis.astype(np.uint8))
            d = ImageDraw.Draw(thumb)
            cx, cy, r = anchor
            d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 60, 60), width=4)
            d.text((12, 12), ship, fill=(255, 255, 0))
            thumbs.append(thumb.resize((384, 256)))
    (OUT / "anchors.json").write_text(json.dumps(anchors, indent=1) + "\n")

    if debug and thumbs:
        cols = 6
        sheet = Image.new("RGB", (cols * 384, math.ceil(len(thumbs) / cols) * 256))
        for i, t in enumerate(thumbs):
            sheet.paste(t, ((i % cols) * 384, (i // cols) * 256))
        Path(debug).parent.mkdir(parents=True, exist_ok=True)
        sheet.save(debug)


def painted_heroes(plate, mask):
    """The painting's few big, vivid stars. Judged by the brightest CHANNEL
    over the sky, not luma: luma barely counts blue, so a blazing
    (80, 90, 255) star scores only ~106 and would pass as an ordinary one.
    They are two-tone, a near-white core in a deep blue glow, so each gets a
    core tint (light added at the centre) and a halo tint (light added in
    the ring around it); one averaged tint washes out to pale cyan."""
    rgb = np.asarray(plate, dtype=np.float32)
    base = np.dstack([np.asarray(star_removed(Image.fromarray(rgb[..., c].astype(np.uint8))),
                                 dtype=np.float32) for c in range(3)])
    peak = rgb.max(axis=2)
    local_max = peak >= np.asarray(Image.fromarray(peak.astype(np.uint8))
                                   .filter(ImageFilter.MaxFilter(3)), dtype=np.float32)
    sky = np.asarray(mask) > 200
    rise = peak - base.max(axis=2)
    peaks = (rise >= HERO_CONTRAST * 255.0) & local_max & sky
    excess = np.clip(rgb - base, 0.0, None)
    ring = np.ones((9, 9), dtype=bool)
    ring[3:6, 3:6] = False                            # the 3x3 core is the core

    def tint(light):
        return light / max(float(light.max()), 1e-6)

    core, halo = [], []
    for y, x in zip(*np.nonzero(peaks)):
        if not (4 <= y < rgb.shape[0] - 4 and 4 <= x < rgb.shape[1] - 4):
            continue
        patch = excess[y - 4:y + 5, x - 4:x + 5]
        core.append(tint(patch[3:6, 3:6].sum(axis=(0, 1))))
        halo.append(tint(patch[ring].sum(axis=0)))
    n = len(core)
    return SimpleNamespace(density=n / max(sky.sum(), 1), contrast=rise[peaks][:n] / 255.0,
                           core=np.asarray(core, np.float32).reshape(-1, 3),
                           halo=np.asarray(halo, np.float32).reshape(-1, 3))


def hero_tile(heroes, seed):
    """Two-tone glows: a wide halo-tinted gaussian with a tight core-tinted
    one over it. Same seed for both passes, so positions, picks and sigma
    draws match and every core sits dead-centre in its halo."""
    def layer(tints, sigma, gain):
        stars = SimpleNamespace(density=heroes.density, contrast=heroes.contrast, tint=tints)
        return np.asarray(star_tile(stars, 1.0, sigma, np.random.default_rng(seed),
                                    gain=gain, radius=5), dtype=np.float32) / 255.0
    glow = layer(heroes.halo, HERO_SIGMA, HERO_GAIN)
    core = layer(heroes.core, HERO_CORE_SIGMA, 1.3)
    a = core[..., 3:4] + glow[..., 3:4] * (1.0 - core[..., 3:4])
    rgb = np.where(a > 0, (core[..., :3] * core[..., 3:4] +
                           glow[..., :3] * glow[..., 3:4] * (1.0 - core[..., 3:4])) /
                   np.maximum(a, 1e-6), 0.0)
    return Image.fromarray((np.dstack([rgb, a]) * 255.0 + 0.5).astype(np.uint8), "RGBA")


def bake_stars():
    """Per-composite star tiles (<ship>_stars_{far,near}.png) drawn from the
    stars painted in THAT composite's mouth: the paintings differ (tarsus
    ~13 stars/10k px, drayman ~2). Brightness and colour fall back to the
    pooled population when a mouth has too few painted stars to sample."""
    paths = sorted(COMPOSITES.glob("*.png"))
    painted = {p.stem: PaintedStars(Image.open(p).convert("RGB"),
                                    Image.open(OUT / f"{p.stem}_mask.png")) for p in paths}
    pooled = SimpleNamespace(contrast=np.concatenate([s.contrast for s in painted.values()]),
                             tint=np.concatenate([s.tint for s in painted.values()]))
    for i, (ship, s) in enumerate(painted.items()):
        plate = Image.open(COMPOSITES / f"{ship}.png").convert("RGB")
        mask = Image.open(OUT / f"{ship}_mask.png")
        fill = Image.open(OUT / f"{ship}_fill.png").convert("RGB")
        source = s if s.contrast.size >= MIN_PAINTED_STARS else pooled
        heroes = painted_heroes(plate, mask)
        target = max(s.density, MIN_STAR_DENSITY)
        density = target * STAR_DENSITY_GAIN
        for _ in range(CALIBRATION_STEPS):
            stars = SimpleNamespace(density=density, contrast=source.contrast, tint=source.tint)
            rng = np.random.default_rng(RNG_SEED + 553 + i)
            tiles = (star_tile(stars, 0.65, (0.32, 0.55), rng),
                     star_tile(stars, 0.35, (0.4, 0.75), rng))
            measured = PaintedStars(starfield(plate, mask, fill, tiles), mask).density
            if measured <= 0 or abs(measured / target - 1.0) <= CALIBRATION_TOLERANCE:
                break
            density *= target / measured
        tiles += (hero_tile(heroes, RNG_SEED + 1553 + i),)
        for tile, depth in zip(tiles, ("far", "near", "hero")):
            tile.save(OUT / f"{ship}_stars_{depth}.png", optimize=True)
        print(f"{ship:<11} painted {s.density * 1e4:5.1f}, animated {measured * 1e4:5.1f} "
              f"stars/10k px, {heroes.contrast.size} hero"
              f"{'' if source is s else ' (pooled look)'}")


def starfield(plate, mask, fill, tiles):
    """The composite as the engine draws it at t=0 (texel = pixel): fill,
    star tiles over it, then the plate with the mask as alpha."""
    w, h = plate.size
    sky = np.asarray(fill.resize((w, h), Image.BILINEAR), dtype=np.float32) / 255.0
    for tile in tiles:
        t = np.asarray(tile, dtype=np.float32) / 255.0
        t = np.tile(t, (h // t.shape[0] + 1, w // t.shape[1] + 1, 1))[:h, :w]
        sky = t[..., :3] * t[..., 3:4] + sky * (1.0 - t[..., 3:4])
    m = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
    out = np.asarray(plate, dtype=np.float32) / 255.0 * (1.0 - m) + sky * m
    return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--debug", help="contact sheet of every detected mouth")
    ap.add_argument("--stars-only", action="store_true",
                    help="re-make the star tiles from the existing masks")
    args = ap.parse_args()
    if not args.stars_only:
        bake_mouths(args.debug)
    bake_stars()


if __name__ == "__main__":
    main()

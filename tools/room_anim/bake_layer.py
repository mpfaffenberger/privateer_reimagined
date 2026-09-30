#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Bake rendered passes into a plate-aware sprite atlas for the engine (#515).

The engine composites layers with plain straight-alpha "over" onto the static
painting. Because the painting under a sprite never changes, any target
image T can be reproduced exactly as c*a + P*(1-a) using the smallest alpha
that keeps c in [0,1]. So reflections, glow and shadows on the *painted*
floor need no special blend modes:

    T = m*A + (1-m)*F               A beauty, B empty deck, m actor coverage
    F = P + (A - B)  where the actor adds light (reflections, glow)
    F = P * A/B      where it takes light away (shadows: a fraction of the
                     light, so the proxy deck's albedo need not match paint)
    (in linear light, with the render's exposure undone: light adds linearly,
    and passes are rendered dark so nothing clips; only where A was rendered:
    render.py borders each frame to the actor and its floor footprint)
    then, in display sRGB (how the engine blends), per channel d = T-P:
    a = d/(1-P) if d>0,  -d/P if d<0;   c = P + d/a

Each frame is trimmed to its non-zero alpha and shelf-packed into one atlas.
Big frames (near the camera, motion-blurred, soft anyway) are stored at half
resolution; the manifest's src/dst rects let the engine scale them back up.

Loop timing (period / phase) comes from tools/room_anim/<base>/layers.json.

Usage (from the repo root; --base defaults to newcon):
    uv run tools/room_anim/bake_layer.py --base newcon --all
    uv run tools/room_anim/bake_layer.py car_receding
    uv run tools/room_anim/bake_layer.py car_receding --period 30   # try a timing
"""
import argparse
import json
import math

import numpy as np
from PIL import Image

from base import paths

NOISE = 5.0 / 255.0     # visible plate change below this is noise or invisible light
SOURCE_LSB = 3.0 / 255.0  # A-B below this in the 8-bit render is quantisation, not light
BIG_FRAME_PX = 40_000   # frames larger than this are stored at half resolution
ATLAS_W = 1024          # atlas width to try first; doubled until the height fits
MAX_ATLAS_W = 4096
# Keep atlases well inside GPU texture limits (16384 on D3D11/Metal): PIL
# (the preview) will happily read a 1024x22586 atlas the engine can't load.
MAX_ATLAS_H = 8192
PAD = 1


def load_rgba(path):
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32) / 255.0


def to_linear(srgb):
    return np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)


def to_srgb(linear):
    lin = np.clip(linear, 0.0, 1.0)
    return np.where(lin <= 0.0031308, lin * 12.92, 1.055 * lin ** (1.0 / 2.4) - 0.055)


def target(plate, beauty, rendered, empty, mask, gain):
    """Display-sRGB target over `plate`. `gain` = 2**-exposure_ev undoes the
    render exposure so actor and light come back at their true brightness."""
    a_lin, b_lin, p_lin = to_linear(beauty) * gain, to_linear(empty) * gain, to_linear(plate)
    delta = a_lin - b_lin
    lit = np.where(delta > 0.0, p_lin + delta, p_lin * a_lin / np.maximum(b_lin, 1e-4))
    real = np.abs(beauty - empty).max(axis=2) >= SOURCE_LSB
    visible = np.abs(to_srgb(lit) - plate).max(axis=2) > NOISE
    floor = np.where((rendered & real & visible)[..., None], lit, p_lin)
    m = mask[..., None]
    return to_srgb(m * a_lin + (1.0 - m) * floor)


def encode(plate, tgt):
    """Minimal-alpha straight RGBA that reproduces `tgt` over `plate` (8-bit)."""
    d = tgt - plate
    eps = 1e-6
    need = np.where(d > 0, d / np.maximum(1.0 - plate, eps), -d / np.maximum(plate, eps))
    a = np.clip(need.max(axis=2), 0.0, 1.0)
    a8 = np.ceil(a * 255.0 - 1e-4)                          # round alpha UP: c stays in range
    a8[np.abs(d).max(axis=2) * 255.0 < 0.5] = 0.0           # invisible change -> transparent
    af = np.maximum(a8 / 255.0, eps)[..., None]
    c = np.clip(plate + d / af, 0.0, 1.0)
    rgba = np.dstack([np.round(c * 255.0), a8]).astype(np.uint8)
    rgba[a8 == 0] = 0
    return rgba


def bbox(nonzero):
    ys, xs = np.nonzero(nonzero)
    if xs.size == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1


def shrink(rgba):
    """Half-size straight-alpha RGBA, resampled premultiplied (no dark fringe)."""
    f = rgba.astype(np.float32) / 255.0
    pre = np.dstack([f[..., :3] * f[..., 3:4], f[..., 3]])
    size = (max(1, rgba.shape[1] // 2), max(1, rgba.shape[0] // 2))
    small = np.dstack([np.asarray(Image.fromarray(pre[..., c]).resize(size, Image.BOX))
                       for c in range(4)])
    a = small[..., 3:4]
    rgb = np.where(a > 0, small[..., :3] / np.maximum(a, 1e-6), 0.0)
    return (np.clip(np.dstack([rgb, a]), 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def load_frame(path, max_px=BIG_FRAME_PX):
    """-> (sprite RGBA, dst [x, y, w, h]) trimmed to its alpha, or None.
    For straight-alpha passes with no plate to encode against (ships in
    open sky), the counterpart of bake_frame(). Sprites over `max_px` are
    stored at half size (the engine scales them back to dst); pass None to
    keep a hero layer sharp."""
    rgba = np.asarray(Image.open(path).convert("RGBA"))
    box = bbox(rgba[..., 3])
    if box is None:
        return None
    x0, y0, x1, y1 = box
    sprite = rgba[y0:y1, x0:x1]
    dst = [x0, y0, x1 - x0, y1 - y0]
    if max_px and sprite.shape[0] * sprite.shape[1] > max_px:
        sprite = shrink(sprite)
    return sprite, dst


def bake_frame(plate, empty, beauty_path, mask_path, gain):
    """One rendered frame -> (sprite RGBA, dst [x,y,w,h] in plate px) or None."""
    beauty = load_rgba(beauty_path)
    rendered = beauty[..., 3] > 0.5
    box = bbox(rendered)                       # only the render border can change
    if box is None:
        return None
    x0, y0, x1, y1 = box
    crop = np.s_[y0:y1, x0:x1]
    rgba = encode(plate[crop], target(plate[crop], beauty[crop][..., :3], rendered[crop],
                                      empty[crop], load_rgba(mask_path)[crop][..., 3], gain))
    tight = bbox(rgba[..., 3])
    if tight is None:
        return None
    tx0, ty0, tx1, ty1 = tight
    sprite = rgba[ty0:ty1, tx0:tx1]
    dst = [x0 + tx0, y0 + ty0, tx1 - tx0, ty1 - ty0]
    if sprite.shape[0] * sprite.shape[1] > BIG_FRAME_PX:
        sprite = shrink(sprite)
    return sprite, dst


def shelf_pack(sizes, width):
    """Place (w,h) rects left-to-right in rows, tallest first. Returns xy, height."""
    order = sorted(range(len(sizes)), key=lambda i: -sizes[i][1])
    pos, x, y, row_h = [None] * len(sizes), 0, 0, 0
    for i in order:
        w, h = sizes[i]
        if x + w + PAD > width:
            x, y, row_h = 0, y + row_h + PAD, 0
        pos[i] = (x, y)
        x, row_h = x + w + PAD, max(row_h, h)
    return pos, y + row_h


def main():
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--base", default="newcon", help="assets/concourse/<base>")
    where = paths(pre.parse_known_args()[0].base)
    timing = {k: v for k, v in json.loads(where.timing.read_text()).items()
              if not k.startswith("_")}
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], parents=[pre])
    ap.add_argument("layer", nargs="?", choices=sorted(timing))
    ap.add_argument("--all", action="store_true", help="bake every layer in layers.json")
    ap.add_argument("--period", type=float,
                    help="override: loop length in seconds (>= the pass; rest is a gap)")
    ap.add_argument("--offset", type=float, help="override: phase offset in seconds")
    args = ap.parse_args()
    if args.all == bool(args.layer):
        ap.error("give exactly one of LAYER or --all")
    for layer in (sorted(timing) if args.all else [args.layer]):
        cfg = timing[layer]
        bake(where, layer,
             cfg["period"] if args.period is None else args.period,
             cfg["offset"] if args.offset is None else args.offset)


def bake(where, layer, period, offset):
    src = where.build / layer
    info = json.loads((src / "pass.json").read_text())   # written by render.render_passes
    if "exposure_ev" not in info:
        raise SystemExit(f"{layer}: pass.json predates exposure_ev; re-render the layer")
    fps, gain = float(info["fps"]), 2.0 ** -float(info["exposure_ev"])
    plate = load_rgba(where.plate)[..., :3]
    empty = load_rgba(src / "empty.png")[..., :3]
    sprites, slots = [], []
    for beauty_path in sorted((src / "beauty").glob("*.png")):
        baked = bake_frame(plate, empty, beauty_path, src / "mask" / beauty_path.name, gain)
        if baked is not None:
            sprites.append(baked)
            slots.append(int(beauty_path.stem) - 1)   # frame N -> slot N-1; gaps stay blank
    if not sprites:
        raise SystemExit(f"{layer}: every frame is empty")
    period_frames = max(int(info["frames"]), int(math.ceil(period * fps)))
    write_sheet(where.anim, layer, sprites, slots, (plate.shape[1], plate.shape[0]), fps,
                period_frames, int(round(offset * fps)))


def write_sheet(out_dir, name, sprites, slots, canvas, fps, period_frames, offset_frames,
                **extra):
    """Pack (sprite RGBA, dst rect) frames into <name>.png and write the
    <name>.json manifest room_anim_data.h reads. `extra` adds manifest keys
    (e.g. "under", "anchor")."""
    sizes = [(s.shape[1], s.shape[0]) for s, _ in sprites]
    width = ATLAS_W
    pos, height = shelf_pack(sizes, width)
    while height > MAX_ATLAS_H and width < MAX_ATLAS_W:
        width *= 2
        pos, height = shelf_pack(sizes, width)
    if height > MAX_ATLAS_H:
        raise SystemExit(f"{name}: {len(sizes)} sprites need a {width}x{height} atlas, over "
                         f"the {MAX_ATLAS_H} px limit; shorten the pass or shrink frames")
    atlas = np.zeros((height, width, 4), dtype=np.uint8)
    frames = []
    for (img, dst), (x, y), slot in zip(sprites, pos, slots):
        atlas[y:y + img.shape[0], x:x + img.shape[1]] = img
        frames.append({"slot": slot, "src": [x, y, img.shape[1], img.shape[0]], "dst": dst})

    out_dir.mkdir(parents=True, exist_ok=True)
    Image.fromarray(atlas).save(out_dir / f"{name}.png", optimize=True)
    manifest = {"atlas": f"{name}.png", "canvas": list(canvas), "fps": fps,
                "period_frames": period_frames, "offset_frames": offset_frames,
                **extra, "frames": frames}
    (out_dir / f"{name}.json").write_text(json.dumps(manifest, separators=(",", ":")) + "\n")
    print(f"{name}: {len(frames)} sprites, atlas {width}x{height}, "
          f"loop {period_frames} frames @ {fps:g} fps")


if __name__ == "__main__":
    main()

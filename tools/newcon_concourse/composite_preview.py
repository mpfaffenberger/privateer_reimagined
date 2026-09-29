#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "imageio", "imageio-ffmpeg"]
# ///
"""Preview an animated base room exactly as the engine draws it (#515).

Reads the room straight from concourse.json (single source of truth) and
follows src/room_anim.cpp: sky fill stretched, star tiles scrolled at their
velocity (plate px/s, wrapping), the plate on top with the sky mask as alpha,
then each sprite layer's frame for timeline slot
(floor(t*fps) + offset_frames) mod period_frames, straight-alpha "over".
Nothing here reads Blender output, so it checks what ships.

Usage (from the repo root):
    uv run tools/newcon_concourse/composite_preview.py --seconds 12 \
        --out build/newcon_concourse/preview.mp4
    ... --out preview.gif --scale 0.5 --fps 12     # GIF for the PR
    ... --at 3.5 --out still.png                   # one frame
    ... --only anim/walker_toward.json             # isolate one layer
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[2]
ROOM_DIR = REPO / "assets/concourse/newcon"


def rgba(path):
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32) / 255.0


def over(dst, src):
    a = src[..., 3:4]
    dst[...] = src[..., :3] * a + dst * (1.0 - a)


class Sky:
    def __init__(self, cfg, size):
        w, h = size
        self.fill = np.asarray(Image.open(ROOM_DIR / cfg["fill"]).convert("RGB")
                               .resize(size, Image.BILINEAR), dtype=np.float32) / 255.0
        self.mask = np.asarray(Image.open(ROOM_DIR / cfg["mask"]).convert("L"),
                               dtype=np.float32) / 255.0
        self.stars = []
        for s in cfg.get("stars", []):
            tile = Image.open(ROOM_DIR / s["tile"]).convert("RGBA")
            reps = (h // tile.height + 2, w // tile.width + 2, 1)
            tiled = Image.fromarray(np.tile(np.asarray(tile), reps))
            self.stars.append((tiled, tile.size, s["velocity"]))

    def draw(self, t, size):
        canvas = self.fill.copy()
        for tiled, (tw, th), (vx, vy) in self.stars:
            # Screen x shows texel x - v*t (mod tile): sample from +(tile - s).
            sx, sy = tw - (vx * t) % tw, th - (vy * t) % th
            layer = tiled.transform(size, Image.AFFINE, (1, 0, sx, 0, 1, sy),
                                    resample=Image.BILINEAR)
            over(canvas, np.asarray(layer, dtype=np.float32) / 255.0)
        return canvas


class Layer:
    def __init__(self, manifest):
        path = ROOM_DIR / manifest
        self.meta = json.loads(path.read_text())
        self.atlas = Image.open(path.parent / self.meta["atlas"]).convert("RGBA")
        self.by_slot = {f["slot"]: f for f in self.meta["frames"]}

    def draw(self, canvas, t):
        m = self.meta
        slot = (int(np.floor(t * m["fps"])) + m["offset_frames"]) % m["period_frames"]
        frame = self.by_slot.get(slot)
        if frame is None:
            return
        sx, sy, sw, sh = frame["src"]
        dx, dy, dw, dh = frame["dst"]
        spr = self.atlas.crop((sx, sy, sx + sw, sy + sh))
        if (sw, sh) != (dw, dh):                     # half-res frame: linear upscale
            spr = spr.resize((dw, dh), Image.BILINEAR)
        over(canvas[dy:dy + dh, dx:dx + dw], np.asarray(spr, dtype=np.float32) / 255.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--room", default="concourse")
    ap.add_argument("--only", action="append", help="draw only these layer manifests")
    ap.add_argument("--no-sky", action="store_true")
    ap.add_argument("--out", required=True, help=".mp4, .gif, or .png with --at")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--fps", type=float, default=24.0, help="preview frame rate")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--crop", type=int, nargs=4, metavar=("X", "Y", "W", "H"),
                    help="plate-pixel crop (for close-up review)")
    ap.add_argument("--at", type=float, help="write a single still at this time")
    args = ap.parse_args()

    room = json.loads((ROOM_DIR / "concourse.json").read_text())["rooms"][args.room]
    plate = rgba(ROOM_DIR / room["background"])
    size = (plate.shape[1], plate.shape[0])
    sky = Sky(room["sky"], size) if "sky" in room and not args.no_sky else None
    if sky is not None:
        plate[..., 3] = 1.0 - sky.mask
    layers = [Layer(m) for m in (args.only or room.get("layers", []))]

    def frame(t):
        canvas = sky.draw(t, size) if sky else np.zeros_like(plate[..., :3])
        over(canvas, plate)
        for layer in layers:
            layer.draw(canvas, t)
        im = Image.fromarray((np.clip(canvas, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8))
        if args.crop:
            x, y, w, h = args.crop
            im = im.crop((x, y, x + w, y + h))
        if args.scale != 1.0:
            im = im.resize((round(im.width * args.scale) // 2 * 2,
                            round(im.height * args.scale) // 2 * 2), Image.LANCZOS)
        return im

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if args.at is not None:
        frame(args.at).save(out)
        return
    times = [args.start + i / args.fps for i in range(int(args.seconds * args.fps))]
    if out.suffix == ".mp4":
        import imageio.v2 as imageio
        with imageio.get_writer(out, fps=args.fps, codec="libx264", quality=8,
                                macro_block_size=2) as writer:
            for t in times:
                writer.append_data(np.asarray(frame(t)))
        return
    images = [frame(t) for t in times]
    # One shared palette so the static plate doesn't shimmer frame to frame.
    palette = images[len(images) // 2].quantize(colors=255, method=Image.Quantize.MEDIANCUT)
    images = [im.quantize(palette=palette, dither=Image.Dither.NONE) for im in images]
    images[0].save(out, save_all=True, append_images=images[1:], loop=0,
                   duration=int(round(1000.0 / args.fps)))


if __name__ == "__main__":
    main()

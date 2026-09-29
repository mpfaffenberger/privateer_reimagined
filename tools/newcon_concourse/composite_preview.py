#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy", "imageio", "imageio-ffmpeg"]
# ///
"""Preview an animated base room exactly as the engine draws it (#515).

Reads the room straight from concourse.json (single source of truth) and
follows src/room_anim.cpp: sky fill stretched, star tiles scrolled at their
velocity (plate px/s, wrapping) and spun at their spin (deg/s clockwise about
the anchor, else the plate centre), the plate on top with the sky mask as alpha,
then each sprite layer's frame for timeline slot
(floor(t*fps) + offset_frames) mod period_frames, straight-alpha "over".
"under" layers go between the sky and the plate; anchored layers are
remapped onto the plate's anchor (room_anim_data.cpp place()).
With --plate NAME the room's per-plate "composite" animation is used over
landing_ships/NAME.png, with {plate} -> NAME (#553).
Nothing here reads Blender output, so it checks what ships.

Usage (from the repo root):
    uv run tools/newcon_concourse/composite_preview.py --seconds 12 \
        --out build/newcon_concourse/preview.mp4
    ... --out preview.gif --scale 0.5 --fps 12     # GIF for the PR
    ... --at 3.5 --out still.png                   # one frame
    ... --only anim/walker_toward.json             # isolate one layer
    ... --room landing --plate tarsus --out hangar.mp4   # a hangar composite
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


def sample_repeat(tile, qx, qy):
    """Bilinear, wrapping sample of `tile` (H x W x C) at texel coords q,
    like the engine's LINEAR + REPEAT sampler (texel centres at +0.5)."""
    th, tw = tile.shape[:2]
    qx, qy = qx - 0.5, qy - 0.5
    x0, y0 = np.floor(qx), np.floor(qy)
    fx, fy = (qx - x0)[:, None], (qy - y0)[:, None]
    x0, y0 = x0.astype(np.int64) % tw, y0.astype(np.int64) % th
    x1, y1 = (x0 + 1) % tw, (y0 + 1) % th
    return (tile[y0, x0] * (1 - fx) * (1 - fy) + tile[y0, x1] * fx * (1 - fy) +
            tile[y1, x0] * (1 - fx) * fy + tile[y1, x1] * fx * fy)


class Sky:
    def __init__(self, cfg, size, centre):
        self.fill = np.asarray(Image.open(ROOM_DIR / cfg["fill"]).convert("RGB")
                               .resize(size, Image.BILINEAR), dtype=np.float32) / 255.0
        self.mask = np.asarray(Image.open(ROOM_DIR / cfg["mask"]).convert("L"),
                               dtype=np.float32) / 255.0
        # Only sky pixels can show stars; the plate hides the rest.
        self.ys, self.xs = np.nonzero(self.mask > 0)
        self.px, self.py = self.xs + 0.5, self.ys + 0.5          # pixel centres
        self.centre = centre
        self.stars = [(np.asarray(Image.open(ROOM_DIR / s["tile"]).convert("RGBA"),
                                  dtype=np.float32) / 255.0,
                       s["velocity"], float(s.get("spin", 0.0)))
                      for s in cfg.get("stars", [])]

    def draw(self, t):
        canvas = self.fill.copy()
        cx, cy = self.centre
        dx, dy = self.px - cx, self.py - cy
        for tile, (vx, vy), spin in self.stars:
            # Screen p shows texel c + R(-a)(p - c) - v*t (room_anim_data star_uvs).
            a = np.radians((spin * t) % 360.0)
            qx = cx + np.cos(a) * dx + np.sin(a) * dy - vx * t
            qy = cy - np.sin(a) * dx + np.cos(a) * dy - vy * t
            src = sample_repeat(tile, qx, qy)
            alpha = src[:, 3:4]
            dst = canvas[self.ys, self.xs]
            canvas[self.ys, self.xs] = src[:, :3] * alpha + dst * (1.0 - alpha)
        return canvas


class Layer:
    def __init__(self, manifest, anchor=None):
        path = ROOM_DIR / manifest
        self.meta = json.loads(path.read_text())
        self.atlas = Image.open(path.parent / self.meta["atlas"]).convert("RGBA")
        self.by_slot = {f["slot"]: f for f in self.meta["frames"]}
        self.under = bool(self.meta.get("under", False))
        self.anchor = anchor if "anchor" in self.meta else None

    def place(self, dst):
        if self.anchor is None:
            return dst
        (fx, fy, fr), (tx, ty, tr) = self.meta["anchor"], self.anchor
        s = tr / fr
        return [tx + (dst[0] - fx) * s, ty + (dst[1] - fy) * s, dst[2] * s, dst[3] * s]

    def draw(self, canvas, t):
        m = self.meta
        slot = (int(np.floor(t * m["fps"])) + m["offset_frames"]) % m["period_frames"]
        frame = self.by_slot.get(slot)
        if frame is None:
            return
        sx, sy, sw, sh = frame["src"]
        dx, dy, dw, dh = (int(round(v)) for v in self.place(frame["dst"]))
        if dw < 1 or dh < 1:
            return
        spr = self.atlas.crop((sx, sy, sx + sw, sy + sh))
        if (sw, sh) != (dw, dh):                     # half-res or anchored: linear
            spr = spr.resize((dw, dh), Image.BILINEAR)
        spr = np.asarray(spr, dtype=np.float32) / 255.0
        h, w = canvas.shape[:2]                      # clip to the plate
        x0, y0, x1, y1 = max(dx, 0), max(dy, 0), min(dx + dw, w), min(dy + dh, h)
        if x0 < x1 and y0 < y1:
            over(canvas[y0:y1, x0:x1], spr[y0 - dy:y1 - dy, x0 - dx:x1 - dx])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--room", default="concourse")
    ap.add_argument("--plate", help="per-plate composite (e.g. tarsus) for --room landing")
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
    anchor = None
    if args.plate:
        room = json.loads(json.dumps(room["composite"]).replace("{plate}", args.plate))
        room["background"] = f"landing_ships/{args.plate}.png"
        if "anchors" in room:
            anchor = json.loads((ROOM_DIR / room["anchors"]).read_text()).get(args.plate)
    plate = rgba(ROOM_DIR / room["background"])
    size = (plate.shape[1], plate.shape[0])
    centre = anchor[:2] if anchor else (size[0] / 2, size[1] / 2)
    sky = Sky(room["sky"], size, centre) if "sky" in room and not args.no_sky else None
    if sky is not None:
        plate[..., 3] = 1.0 - sky.mask
    layers = [Layer(m, anchor) for m in (args.only or room.get("layers", []))]

    def frame(t):
        canvas = sky.draw(t) if sky else np.zeros_like(plate[..., :3])
        for layer in (lay for lay in layers if lay.under):
            layer.draw(canvas, t)
        over(canvas, plate)
        for layer in (lay for lay in layers if not lay.under):
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
    # MAXCOVERAGE, not MEDIANCUT: median cut drops the thin red guide stripe.
    palette = images[len(images) // 2].quantize(colors=255, method=Image.Quantize.MAXCOVERAGE)
    images = [im.quantize(palette=palette, dither=Image.Dither.NONE) for im in images]
    images[0].save(out, save_all=True, append_images=images[1:], loop=0,
                   duration=int(round(1000.0 / args.fps)))


if __name__ == "__main__":
    main()

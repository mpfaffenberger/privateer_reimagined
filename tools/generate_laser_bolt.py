#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# generate_laser_bolt.py — synthesize the laser bolt PNG procedurally.
#
# Vanilla Privateer has NO laser bolt sprite in DATA/APPEARNC/LASER.IFF (it
# only has a SKEL chunk — the bolt was drawn procedurally as a red ray in the
# original engine). All the OTHER 7 guns are animated spheres extracted from
# their APPEARNC files; the laser is the odd one out.
#
# To keep our render pipeline uniform (every gun = one billboard sprite set),
# we synthesize a PNG that matches the vanilla aesthetic: a small red bolt,
# length ~4x sphere radius, height ~1 diameter, with a hot core. Mirrors the
# classic Wing Commander "red dash" laser look.
#
# Output: gog_extracted/bolt_sprites/laser_NN.png (gitignored, local-only).
#
# Usage:
#   python3 tools/generate_laser_bolt.py [--frames 1] [--radius 10]
# -----------------------------------------------------------------------------
import argparse
import math
import os

try:
    from PIL import Image
except ImportError:
    import sys
    sys.exit("[laser-bolt] needs Pillow: pip install pillow")


def make_bolt(radius: int, length_radii: float, frame: int, frames: int) -> Image.Image:
    """One laser bolt frame. Red ray, hot core, tapered (capsule) ends.

    length_radii: total length in sphere radii (e.g. 4.0 -> 4x radius long).
    A subtle per-frame flicker (length + brightness) gives a live spark feel
    when frames > 1, matching the animated nature of the other bolts.
    """
    flicker = 1.0
    if frames > 1:
        # gentle +/-8% length wobble + brightness breathe, cyclic
        phase = (frame / max(frames - 1, 1)) * math.pi * 2
        flicker = 1.0 + 0.08 * math.sin(phase)

    length = max(1, int(radius * length_radii * flicker))
    height = max(1, radius * 2)
    pad = 3
    W, H = length + pad * 2, height + pad * 2
    cx, cy = W / 2.0, H / 2.0

    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = img.load()

    # Width profile: a near-constant line with just slightly rounded
    # ends. The old sin^0.7 lemon profile made a fat-bellied oval; a
    # uniform thickness reads as the classic thin laser ray. Only the
    # last ~10% at each tip tapers, and even then gently.
    def width_at(t: float) -> float:
        if t < 0.1:
            return t / 0.1
        if t > 0.9:
            return (1.0 - t) / 0.1
        return 1.0

    bright = 1.0
    if frames > 1:
        bright = 0.85 + 0.15 * (0.5 + 0.5 * math.sin(
            (frame / max(frames - 1, 1)) * math.pi * 2))

    for y in range(H):
        for x in range(W):
            t = (x - pad) / max(length, 1)        # 0..1 along the bolt axis
            if t < 0.0 or t > 1.0:
                continue
            half = width_at(t) * (height / 2.0)
            dy = abs(y - cy)
            if dy > half:
                continue
            edge = dy / half if half > 1e-6 else 0.0   # 0 center, 1 edge
            alpha = int(255 * math.exp(-(edge * 1.8) ** 2))
            # Core intensity: peaks on the centerline and where the bolt is
            # widest (middle), falls off at the tips and edges.
            core = math.exp(-(edge * 2.6) ** 2) * (0.55 + 0.45 * width_at(t))
            # Pure red ray with a brighter red (not yellow-white) core.
            # Stays red across the whole bolt; core just pushes brightness.
            r = min(255, int((235 + 20 * core) * bright))
            g = min(255, int((18 + 35 * core) * bright))
            b = min(255, int((12 + 22 * core) * bright))
            if alpha > 0:
                px[x, y] = (r, g, b, alpha)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="gog_extracted/bolt_sprites")
    ap.add_argument("--radius", type=int, default=2,
                    help="half-thickness in px (how thin the line is)")
    ap.add_argument("--length-radii", type=float, default=6.0,
                    help="bolt length in sphere radii (3-5)")
    ap.add_argument("--frames", type=int, default=1,
                    help="1 = static; >1 = flicker animation")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    for f in range(args.frames):
        img = make_bolt(args.radius, args.length_radii, f, args.frames)
        name = f"laser_{f:02d}.png"
        img.save(os.path.join(args.out, name))
        print(f"[laser-bolt] frame {f}/{args.frames} "
              f"{img.width}x{img.height} -> {name}")


if __name__ == "__main__":
    main()

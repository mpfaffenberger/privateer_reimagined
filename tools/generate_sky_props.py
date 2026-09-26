#!/usr/bin/env python3
"""Generate the far-field pixel sky props (true alpha, no checkerboard).

Outputs 256x256 PNGs (64x64 art, nearest-neighbor x4) under assets/sky/props/.
Re-run from the repo root:

    python3 tools/generate_sky_props.py
"""

from __future__ import annotations

import math
import os
from PIL import Image

OUT_DIR = os.path.join("assets", "sky", "props")
N = 64
SCALE = 4


def clamp(v: float) -> int:
    return max(0, min(255, int(round(v))))


class Canvas:
    def __init__(self, n: int = N) -> None:
        self.n = n
        self.px = [[(0, 0, 0, 0) for _ in range(n)] for _ in range(n)]

    def blend(self, x: int, y: int, r: float, g: float, b: float, a: float) -> None:
        if a <= 0 or not (0 <= x < self.n and 0 <= y < self.n):
            return
        a = min(255.0, a)
        dr, dg, db, da = self.px[y][x]
        sa = a / 255.0
        da_n = da / 255.0
        out_a = sa + da_n * (1.0 - sa)
        if out_a <= 1e-6:
            self.px[y][x] = (0, 0, 0, 0)
            return

        def ch(s: float, d: int) -> int:
            return clamp((s * sa + d * da_n * (1.0 - sa)) / out_a)

        self.px[y][x] = (ch(r, dr), ch(g, dg), ch(b, db), clamp(out_a * 255.0))

    def disc(self, cx: float, cy: float, rad: float, color: tuple[int, int, int],
             alpha: float, falloff: float = 1.15) -> None:
        if rad <= 0 or alpha <= 0:
            return
        reach = int(math.ceil(rad)) + 1
        x0 = max(0, int(math.floor(cx)) - reach)
        y0 = max(0, int(math.floor(cy)) - reach)
        x1 = min(self.n - 1, int(math.ceil(cx)) + reach)
        y1 = min(self.n - 1, int(math.ceil(cy)) + reach)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
                if d > rad:
                    continue
                t = 1.0 - (d / rad) ** falloff
                self.blend(x, y, color[0], color[1], color[2], alpha * t)

    def save(self, path: str) -> None:
        im = Image.new("RGBA", (self.n, self.n))
        im.putdata([self.px[y][x] for y in range(self.n) for x in range(self.n)])
        im = im.resize((self.n * SCALE, self.n * SCALE), Image.Resampling.NEAREST)
        im.save(path)


def spiral_pink() -> Canvas:
    c = Canvas()
    # Soft outer glow so the arms feather into the nebula.
    c.disc(32, 32, 26, (120, 40, 90), 28, falloff=1.6)
    for arm in (0.0, math.pi):
        for i in range(0, 220):
            t = i / 220.0
            theta = arm + t * 4.4
            rad = 3.0 + t * 22.0
            x = 32.0 + math.cos(theta) * rad
            y = 32.0 + math.sin(theta) * rad * 0.92
            thick = 3.4 * (1.0 - t) + 1.15
            hot = 1.0 - t * 0.55
            col = (
                clamp(255 * hot + 40),
                clamp(90 + 150 * (1.0 - t)),
                clamp(180 + 70 * hot),
            )
            c.disc(x, y, thick, col, 210 * (1.0 - t * 0.35), falloff=1.05)
            # Star knots along the arm.
            if i % 18 == 0:
                c.disc(x, y, 1.15, (255, 245, 255), 255, falloff=0.6)
    c.disc(32, 32, 5.2, (255, 220, 245), 255, falloff=0.8)
    c.disc(32, 32, 2.2, (255, 255, 255), 255, falloff=0.5)
    # A few field stars so it reads as a galaxy, not a sticker.
    for sx, sy, a in (
        (14, 18, 180), (50, 16, 140), (12, 44, 120),
        (52, 46, 200), (20, 52, 110), (46, 12, 160),
    ):
        c.disc(sx, sy, 0.7, (255, 230, 250), a, falloff=0.4)
    return c


def elliptical_amber() -> Canvas:
    c = Canvas()
    c.disc(32, 33, 24, (90, 40, 10), 22, falloff=1.8)
    for y in range(N):
        for x in range(N):
            nx = (x + 0.5 - 32.0) / 20.5
            ny = (y + 0.5 - 33.0) / 13.0
            d2 = nx * nx + ny * ny
            if d2 >= 1.0:
                continue
            d = math.sqrt(d2)
            # Quantize into pixel bands so the ellipse stays chunky.
            band = int(d * 6.0)
            palette = [
                (255, 250, 230),
                (255, 214, 120),
                (255, 170, 50),
                (230, 120, 30),
                (180, 70, 20),
                (120, 40, 16),
            ]
            col = palette[min(band, len(palette) - 1)]
            alpha = 235 * (1.0 - d) ** 0.65
            c.blend(x, y, col[0], col[1], col[2], alpha)
    # Bright core + a dust speckle or two.
    c.disc(32, 33, 3.0, (255, 255, 240), 255, falloff=0.55)
    c.disc(24, 30, 0.8, (255, 236, 190), 220, falloff=0.4)
    c.disc(40, 36, 0.7, (255, 220, 160), 180, falloff=0.4)
    return c


def edgeon_green() -> Canvas:
    c = Canvas()
    c.disc(32, 32, 22, (20, 80, 40), 18, falloff=1.7)
    for x in range(6, 58):
        nx = (x - 32) / 26.0
        bulge = math.exp(-((x - 32) ** 2) / 70.0)
        half_h = 1.15 + 7.5 * bulge
        for y in range(N):
            dy = abs(y + 0.5 - 32.0)
            if dy > half_h:
                continue
            t = dy / half_h
            # Dust lane: a darker cut through the midplane.
            lane = math.exp(-(dy ** 2) / 1.6) * (1.0 - abs(nx))
            g = 210 - 90 * t - 80 * lane
            r = 70 + 40 * bulge - 30 * lane
            b = 110 + 50 * (1.0 - t)
            alpha = 230 * (1.0 - t * 0.45) * (1.0 - abs(nx) ** 1.4)
            alpha *= 1.0 - 0.55 * lane
            c.blend(x, y, r, g, b, alpha)
    c.disc(32, 32, 4.2, (230, 255, 210), 250, falloff=0.7)
    c.disc(32, 32, 1.8, (255, 255, 245), 255, falloff=0.45)
    # Thin bright rim pixels along the disk.
    for x, a in ((10, 90), (18, 140), (46, 150), (54, 80)):
        c.disc(x, 32, 0.7, (180, 255, 190), a, falloff=0.4)
    return c


def irregular_candy() -> Canvas:
    c = Canvas()
    blobs = [
        (28, 30, 10.0, (255, 120, 180), 200),
        (40, 28, 8.0, (120, 230, 255), 190),
        (34, 40, 9.0, (255, 230, 90), 180),
        (22, 38, 6.5, (170, 255, 160), 170),
        (44, 38, 5.5, (255, 160, 220), 160),
        (30, 24, 4.5, (255, 255, 255), 150),
    ]
    for x, y, rad, col, a in blobs:
        c.disc(x, y, rad + 3.5, col, 30, falloff=1.8)
        c.disc(x, y, rad, col, a, falloff=1.05)
    # Knots of stars where the clumps overlap.
    for x, y in ((32, 32), (36, 30), (26, 34), (38, 36)):
        c.disc(x, y, 1.3, (255, 255, 255), 240, falloff=0.5)
    return c


def wormhole_purple() -> Canvas:
    c = Canvas()
    cx = cy = 32.0
    for y in range(N):
        for x in range(N):
            dx = x + 0.5 - cx
            dy = y + 0.5 - cy
            d = math.hypot(dx, dy)
            ang = math.atan2(dy, dx)
            # Accretion ring. The hole stays empty so background stars show.
            ring = math.exp(-((d - 15.5) ** 2) / 8.0)
            swirl = 0.65 + 0.35 * math.sin(ang * 3.0 + d * 0.35)
            halo = math.exp(-((d - 15.5) ** 2) / 40.0) * 0.35
            a = (ring * swirl + halo) * 230
            if a < 8 or d < 8.5:
                continue
            hot = ring * swirl
            r = 150 + 90 * hot
            g = 60 + 40 * math.sin(ang + 1.0)
            b = 210 + 45 * hot
            c.blend(x, y, r, g, b, a)
    # Bright inner lip + a couple of spark pixels on the ring.
    for i in range(28):
        ang = i / 28.0 * math.tau
        wob = 15.2 + 1.4 * math.sin(ang * 3.0)
        c.disc(cx + math.cos(ang) * wob, cy + math.sin(ang) * wob,
               1.15, (230, 190, 255), 180, falloff=0.6)
    c.disc(cx + 14, cy - 4, 1.0, (255, 255, 255), 230, falloff=0.4)
    c.disc(cx - 12, cy + 8, 0.8, (255, 220, 255), 200, falloff=0.4)
    return c


def _hash(ix: int, iy: int) -> float:
    n = (ix * 374761393 + iy * 668265263) & 0xFFFFFFFF
    n = (n ^ (n >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFFFF) / 0x1000000


def _noise(x: float, y: float) -> float:
    x0, y0 = math.floor(x), math.floor(y)
    fx, fy = x - x0, y - y0
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    v00 = _hash(int(x0), int(y0))
    v10 = _hash(int(x0) + 1, int(y0))
    v01 = _hash(int(x0), int(y0) + 1)
    v11 = _hash(int(x0) + 1, int(y0) + 1)
    return (v00 * (1 - fx) + v10 * fx) * (1 - fy) + (v01 * (1 - fx) + v11 * fx) * fy


def ice_nebula() -> Canvas:
    c = Canvas()
    for y in range(N):
        for x in range(N):
            nx = (x - 32) / 10.0
            ny = (y - 30) / 12.0
            n = _noise(nx + 2.2, ny + 0.4)
            n2 = _noise(nx * 2.1 + 8.0, ny * 2.1 + 3.0)
            # Irregular cloud: fall off toward the corners, broken by noise.
            fall = math.exp(-(nx * nx * 0.55 + ny * ny * 0.8))
            v = (n * 0.65 + n2 * 0.35) * fall
            if v < 0.22:
                continue
            t = (v - 0.22) / 0.78
            # Quantize so the wisp stays in chunky ice bands.
            band = int(t * 5)
            palette = [
                (140, 210, 230),
                (170, 235, 245),
                (210, 250, 255),
                (235, 255, 255),
                (255, 255, 255),
            ]
            col = palette[min(band, 4)]
            c.blend(x, y, col[0], col[1], col[2], 40 + 180 * t)
    return c


def pulsar_red() -> Canvas:
    c = Canvas()
    c.disc(32, 32, 16, (120, 20, 30), 36, falloff=1.7)
    # Two opposite beams, pixel-stepped.
    ang = 0.55
    direction = (math.cos(ang), math.sin(ang))
    for sign in (-1.0, 1.0):
        for i in range(1, 28):
            t = i / 28.0
            x = 32.0 + direction[0] * sign * i
            y = 32.0 + direction[1] * sign * i * 0.85
            width = 1.7 * (1.0 - t) + 0.45
            alpha = 220 * (1.0 - t) ** 0.8
            col = (
                clamp(255),
                clamp(80 + 140 * (1.0 - t)),
                clamp(70 + 40 * (1.0 - t)),
            )
            c.disc(x, y, width, col, alpha, falloff=0.7)
    c.disc(32, 32, 4.5, (255, 80, 70), 230, falloff=0.8)
    c.disc(32, 32, 2.0, (255, 245, 230), 255, falloff=0.45)
    # A faint counter-beam tick so it reads as a pulsar, not a comet.
    c.disc(32 - direction[1] * 6, 32 + direction[0] * 6, 0.8, (255, 180, 160), 140, falloff=0.4)
    c.disc(32 + direction[1] * 6, 32 - direction[0] * 6, 0.8, (255, 180, 160), 140, falloff=0.4)
    return c


def plasma_blob() -> Canvas:
    c = Canvas()
    balls = [
        (32, 32, 12.0, (255, 140, 40)),
        (24, 28, 8.0, (255, 70, 140)),
        (40, 30, 7.5, (255, 210, 60)),
        (34, 40, 8.5, (200, 60, 255)),
        (28, 38, 5.0, (255, 240, 180)),
    ]
    for x, y, rad, col in balls:
        c.disc(x, y, rad + 4.0, col, 40, falloff=1.6)
        c.disc(x, y, rad, col, 200, falloff=1.0)
    c.disc(31, 31, 3.2, (255, 255, 230), 255, falloff=0.55)
    # Bubble highlights.
    c.disc(26, 26, 1.4, (255, 255, 255), 180, falloff=0.5)
    c.disc(38, 36, 1.1, (255, 220, 255), 160, falloff=0.5)
    return c


def main() -> None:
    os.makedirs(OUT_DIR, exist_ok=True)
    makers = {
        "pixel_galaxy_spiral_pink.png": spiral_pink,
        "pixel_galaxy_elliptical_amber.png": elliptical_amber,
        "pixel_galaxy_edgeon_green.png": edgeon_green,
        "pixel_galaxy_irregular_candy.png": irregular_candy,
        "pixel_anomaly_wormhole_purple.png": wormhole_purple,
        "pixel_anomaly_ice_nebula.png": ice_nebula,
        "pixel_anomaly_pulsar_red.png": pulsar_red,
        "pixel_anomaly_plasma_blob.png": plasma_blob,
    }
    for name, fn in makers.items():
        path = os.path.join(OUT_DIR, name)
        fn().save(path)
        im = Image.open(path)
        corners = [im.getpixel((0, 0)), im.getpixel((im.width - 1, 0)),
                   im.getpixel((0, im.height - 1)), im.getpixel((im.width - 1, im.height - 1))]
        opaque = sum(1 for px in im.get_flattened_data() if px[3] > 16)
        print(f"{name}: {im.mode} {im.size} opaque≈{opaque} corners={corners}")


if __name__ == "__main__":
    main()

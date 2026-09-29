"""Shared sky + star helpers for animated base rooms (#515, #553; split out in #557).

    star_removed()  luminance with stars opened away (sky/haze level)
    solidify()      binary sky -> hole-filled, antialiased mask
    sky_fill()      starless sky colour, extrapolated past the mask edge
    PaintedStars    a painting's own star population (density, brightness, colour)
    star_tile()     tileable star field drawn from such a population

Per-base cut-outs live with each base (newcon/bake_sky.py, newcon/bake_hangar.py).
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

STAR_TILE = 512
RNG_SEED = 515


def star_removed(lum):
    return lum.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MaxFilter(5))


def solidify(sky, outside_seed):
    """Binary L sky (255) -> finished mask. Bright stars survive
    star_removed() and punch not-sky holes; a hole is any not-sky pixel that
    `outside_seed` (certainly not sky) cannot reach. Then blur + re-threshold
    rounds off square-kernel stair-steps, and a light feather gives a ~1 px
    antialiased edge."""
    sky = sky.copy()
    ImageDraw.floodfill(sky, outside_seed, 128)
    sky = Image.fromarray(np.where(np.asarray(sky) == 128, 0, 255).astype(np.uint8))
    rounded = np.asarray(sky.filter(ImageFilter.GaussianBlur(2.0))) >= 128
    return (Image.fromarray(rounded.astype(np.uint8) * 255)
            .filter(ImageFilter.GaussianBlur(0.8)))


def sky_fill(plate, mask):
    """Starless sky colour, extrapolated past the mask edge so bilinear
    stretching never pulls rib colour into the windows."""
    rgb = np.asarray(plate.convert("RGB"), dtype=np.float32)
    clean = np.dstack([np.asarray(star_removed(Image.fromarray(rgb[..., c].astype(np.uint8))),
                                  dtype=np.float32) for c in range(3)])
    w = (np.asarray(mask, dtype=np.float32) / 255.0)[..., None]
    small = (plate.width // 4, plate.height // 4)
    blur = ImageFilter.GaussianBlur(24)

    def down(a):
        return np.asarray(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
                          .resize(small, Image.BOX).filter(blur), dtype=np.float32)
    num = np.dstack([down(clean[..., c] * w[..., 0]) for c in range(3)])
    den = down(w[..., 0] * 255.0)[..., None] / 255.0
    fill = num / np.maximum(den, 1e-3)
    fallback = np.array([6.0, 8.0, 14.0])                  # deep space blue-black
    fill = np.where(den > 0.02, fill, fallback)
    return Image.fromarray(np.clip(fill, 0, 255).astype(np.uint8))


class PaintedStars:
    """The painting's own star population: density per sky pixel (one per
    local maximum), and each star's contrast over the sky and its colour.
    New stars are drawn from this, so they match the paint, not a guess."""

    def __init__(self, plate, mask):
        grey = plate.convert("L")
        lum = np.asarray(grey, dtype=np.float32)
        base = np.asarray(star_removed(grey), dtype=np.float32)
        local_max = lum >= np.asarray(grey.filter(ImageFilter.MaxFilter(3)), dtype=np.float32)
        sky = np.asarray(mask) > 200
        peaks = (lum - base > 18) & sky & local_max
        rgb = np.asarray(plate, dtype=np.float32)[peaks]
        self.density = peaks.sum() / max(sky.sum(), 1)
        self.contrast = (lum - base)[peaks] / 255.0
        self.tint = rgb / np.maximum(rgb.max(axis=1, keepdims=True), 1.0)


def star_tile(stars, fraction, sigma_range, rng, gain=1.3, radius=3):
    """Tileable star field: gaussian dots wrapped at the tile edges. `gain`
    compensates for sub-pixel centres spreading a star's peak; `radius` (px)
    bounds each dot, so keep it >= 2.5 sigma for wide glows."""
    n = int(STAR_TILE * STAR_TILE * stars.density * fraction)
    acc = np.zeros((STAR_TILE, STAR_TILE, 3), dtype=np.float32)
    yy, xx = np.mgrid[-radius:radius + 1, -radius:radius + 1]
    for _ in range(n):
        x, y = rng.uniform(0, STAR_TILE, 2)
        sigma = rng.uniform(*sigma_range)
        pick = rng.integers(0, stars.contrast.size)
        ix, iy = int(x), int(y)
        g = np.exp(-(((xx - (x - ix)) ** 2 + (yy - (y - iy)) ** 2) / (2 * sigma ** 2)))
        rows, cols = (iy + yy) % STAR_TILE, (ix + xx) % STAR_TILE
        acc[rows, cols] += (g * stars.contrast[pick] * gain)[..., None] * stars.tint[pick]
    a = np.clip(acc.max(axis=2), 0.0, 1.0)
    rgb = np.where(a[..., None] > 0, acc / np.maximum(a[..., None], 1e-6), 0.0)
    return Image.fromarray((np.dstack([np.clip(rgb, 0, 1), a]) * 255).astype(np.uint8), "RGBA")

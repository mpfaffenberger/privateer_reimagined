"""Shared sky + star helpers for animated base rooms (#515, #553; split out in #557).

    star_removed()  luminance with stars opened away (sky/haze level)
    solidify()      binary sky -> hole-filled, antialiased mask
    window_mask()   hand-drawn window polygons refined by darkness -> mask
    sky_fill()      starless sky colour, extrapolated past the mask edge
    hazy_sky_fill() the plate's own starless sky, full res, for glare and haze
    half_fill()     the painted sky itself at half size, extrapolated (#627)
    PaintedStars    a painting's own star population (density, brightness, colour)
    star_tile()     tileable star field drawn from such a population
    contact_sheet() the --debug grid of per-composite sky detections (#627)

Per-base cut-outs live with each base (newcon/bake_sky.py, newcon/bake_hangar.py).
"""
from pathlib import Path

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


def window_mask(plate, windows, outside_seed, holes=()):
    """Sky mask from coarse window polygons, each refined per pixel by its
    own darkness threshold: star-removed luminance at or below it is sky.
    `windows` is [(polygon, threshold), ...]; anything outside every window,
    or inside a `holes` polygon (a painted ship or rock in the sky), is never
    sky however dark. An opening drops thin dark seams inside a window.
    Holes are cut after solidify(), whose hole fill would take them back."""
    base = np.asarray(star_removed(plate.convert("L")), dtype=np.float32)
    limit = Image.new("L", plate.size, 0)            # per-pixel threshold, 0 = never sky
    draw = ImageDraw.Draw(limit)
    for poly, darkness in windows:
        draw.polygon(poly, fill=darkness)
    limit = np.asarray(limit, dtype=np.float32)
    dark = Image.fromarray(((base <= limit) & (limit > 0)).astype(np.uint8) * 255)
    mask = solidify(dark.filter(ImageFilter.MinFilter(7)).filter(ImageFilter.MaxFilter(7)),
                    outside_seed)
    if not holes:
        return mask
    cut = Image.new("L", plate.size, 0)
    for poly in holes:
        ImageDraw.Draw(cut).polygon(poly, fill=255)
    keep = 1.0 - np.asarray(cut.filter(ImageFilter.GaussianBlur(0.8)), dtype=np.float32) / 255.0
    return Image.fromarray((np.asarray(mask, dtype=np.float32) * keep + 0.5).astype(np.uint8))


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


def hazy_sky_fill(plate, blur=1.5):
    """Starless sky at full resolution: the plate itself with its stars
    opened away, lightly blurred. For skies with glare or haze, where
    sky_fill()'s broad average of the masked sky is darker than the haze
    beside it and outlines the mask. The painting's own values reach right
    to the mask's edge, so the edge is seamless wherever it falls."""
    rgb = plate.convert("RGB")
    return Image.merge("RGB", [star_removed(c).filter(ImageFilter.GaussianBlur(blur))
                               for c in rgb.split()])

def half_fill(plate, mask, band=5):
    """The painted sky itself at half size, for skies that are smooth
    gradients (dusk, daylight), so half size loses little. Unlike
    sky_fill() it keeps the painting's own detail: use it where there are no
    stars to remove (#583, #593, #595; shared in #627).

    With nothing passing, plate * (1 - m) + fill * m must look like the
    plate, so the feathered edge matters. The plate is box-averaged inside
    the mask grown by `band` px, so the feather composites back to the
    painting rather than to sky extrapolated into it, then grown outward in
    widening rings until every pixel is filled (the bilinear stretch never
    pulls in black). Scored against the paintings (composited error over
    the sky, 18 plates each) it beat or tied the three per-base versions it
    replaced on mean and p99 for agricultural's, pleasure's and oxford's
    masks; only pleasure's extreme tail got slightly worse (max 15 -> 18
    LSB, 0.02% -> 0.04% of sky pixels over 8 LSB)."""
    from scipy import ndimage   # here, not at the top: most sky users don't need scipy
    rgb = np.asarray(plate.convert("RGB"), dtype=np.float32)
    w = np.asarray(mask.filter(ImageFilter.MaxFilter(band)), dtype=np.float32) / 255.0
    h2, w2 = rgb.shape[0] // 2, rgb.shape[1] // 2

    def half(a):
        return a[:h2 * 2, :w2 * 2].reshape(h2, 2, w2, 2, *a.shape[2:]).mean(axis=(1, 3))
    num, den = half(rgb * w[..., None]), half(w)[..., None]
    fill = np.where(den > 0.5, num / np.maximum(den, 1e-6), 0.0)
    for sigma in (2.0, 8.0, 32.0, 128.0):                  # grow outward, near first
        blur_n = np.dstack([ndimage.gaussian_filter(num[..., c], sigma) for c in range(3)])
        blur_d = ndimage.gaussian_filter(den[..., 0], sigma)[..., None]
        fill = np.where(den > 0.5, fill,
                        np.where(blur_d > 1e-3, blur_n / np.maximum(blur_d, 1e-6), fill))
        den = np.maximum(den, np.where(blur_d > 1e-3, 1.0, 0.0))
    return Image.fromarray(np.clip(fill, 0, 255).astype(np.uint8))


def contact_sheet(thumbs, path, cols=6):
    """The --debug grid: equal-size thumbnails (one per composite), `cols`
    across, saved to `path`. Nothing to do without thumbs."""
    if not thumbs:
        return
    tw, th = thumbs[0].size
    sheet = Image.new("RGB", (cols * tw, -(-len(thumbs) // cols) * th))
    for i, t in enumerate(thumbs):
        sheet.paste(t, ((i % cols) * tw, (i // cols) * th))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)


class PaintedStars:
    """The painting's own star population: density per sky pixel (one per
    local maximum), and each star's contrast over the sky and its colour.
    New stars are drawn from this, so they match the paint, not a guess.
    A star stands `rise` (8-bit luminance) over the sky; lower it for a
    painting whose stars are mostly faint."""

    def __init__(self, plate, mask, rise=18):
        grey = plate.convert("L")
        lum = np.asarray(grey, dtype=np.float32)
        base = np.asarray(star_removed(grey), dtype=np.float32)
        local_max = lum >= np.asarray(grey.filter(ImageFilter.MaxFilter(3)), dtype=np.float32)
        sky = np.asarray(mask) > 200
        peaks = (lum - base > rise) & sky & local_max
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

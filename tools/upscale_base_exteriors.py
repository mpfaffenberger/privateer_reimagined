#!/usr/bin/env python3
"""Build smooth 2x runtime sprites from canonical generated base exteriors.

The generated source sprites have useful ~1K interior detail, but their alpha is
binary. Magnifying that binary silhouette in flight exposes staircase edges.
This tool performs an edge-safe Lanczos upscale while treating color as
premultiplied at the boundary, then sharpens only fully opaque interior pixels.
That avoids both jagged alpha and the dark fringe caused by resizing straight
RGBA whose transparent pixels contain black RGB.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageFilter

REPO = Path(__file__).resolve().parents[1]
SOURCE_DIR = REPO / "generated" / "base_exterior_generation" / "clean"
OUTPUT_DIR = REPO / "assets" / "sprites"
BASE_NAMES = (
    "agricultural",
    "mining",
    "new_constantinople",
    "new_detroit",
    "oxford",
    "perry",
    "pleasure",
    "refinery",
)


@dataclass(frozen=True)
class AlphaStats:
    transparent: int
    partial: int
    opaque: int


def alpha_stats(image: Image.Image) -> AlphaStats:
    histogram = image.getchannel("A").histogram()
    return AlphaStats(histogram[0], sum(histogram[1:255]), histogram[255])


def _binary_alpha(image: Image.Image) -> bool:
    return alpha_stats(image).partial == 0


def _unpremultiply_boundary(rgb: Image.Image, alpha: Image.Image) -> Image.Image:
    """Recover straight RGB only for Lanczos-created partial-alpha pixels.

    Canonical inputs are binary-alpha and transparent RGB is zero, so resizing
    RGB is equivalent to resizing premultiplied color. Fully transparent and
    opaque pixels need no division; touching only the narrow partial edge keeps
    this dependency-free implementation quick even for multi-megapixel output.
    """
    channels = [bytearray(channel.tobytes()) for channel in rgb.split()]
    alpha_bytes = alpha.tobytes()
    for index, value in enumerate(alpha_bytes):
        if value == 0:
            channels[0][index] = 0
            channels[1][index] = 0
            channels[2][index] = 0
        elif value < 255:
            channels[0][index] = min(255, channels[0][index] * 255 // value)
            channels[1][index] = min(255, channels[1][index] * 255 // value)
            channels[2][index] = min(255, channels[2][index] * 255 // value)
    return Image.merge("RGB", [Image.frombytes("L", rgb.size, data) for data in channels])


def upscale_image(source: Image.Image, scale: int = 2) -> Image.Image:
    """Return a high-resolution sprite with a smooth, halo-free silhouette."""
    if scale < 2:
        raise ValueError("scale must be at least 2")

    source = source.convert("RGBA")
    if not _binary_alpha(source):
        raise ValueError("canonical source must have binary alpha")

    size = (source.width * scale, source.height * scale)
    source_rgb = source.convert("RGB")
    source_alpha = source.getchannel("A")
    resized_rgb = source_rgb.resize(size, Image.Resampling.LANCZOS)
    resized_alpha = source_alpha.resize(size, Image.Resampling.LANCZOS)

    straight_rgb = _unpremultiply_boundary(resized_rgb, resized_alpha)
    sharpened = straight_rgb.filter(
        ImageFilter.UnsharpMask(radius=1.0 * scale, percent=65, threshold=4)
    )
    # Texture sharpening belongs inside the object, never on its silhouette.
    opaque_mask = resized_alpha.point(lambda value: 255 if value == 255 else 0)
    final_rgb = Image.composite(sharpened, straight_rgb, opaque_mask)
    final = final_rgb.convert("RGBA")
    final.putalpha(resized_alpha)
    return final


def paths_for(names: Iterable[str]) -> Iterable[tuple[str, Path, Path]]:
    for name in names:
        yield (
            name,
            SOURCE_DIR / f"base_{name}.png",
            OUTPUT_DIR / f"base_{name}.png",
        )


def validate_pair(source_path: Path, output_path: Path, scale: int) -> AlphaStats:
    source = Image.open(source_path).convert("RGBA")
    output = Image.open(output_path).convert("RGBA")
    expected = (source.width * scale, source.height * scale)
    if output.size != expected:
        raise ValueError(f"{output_path.name}: expected {expected}, got {output.size}")
    stats = alpha_stats(output)
    if not stats.transparent or not stats.opaque:
        raise ValueError(f"{output_path.name}: lost transparent or opaque regions")
    if not stats.partial:
        raise ValueError(f"{output_path.name}: silhouette still has binary alpha")
    return stats


def write_upscaled(source_path: Path, output_path: Path, scale: int = 2) -> AlphaStats:
    """Build and validate one runtime sprite from its canonical clean source."""
    if not source_path.is_file():
        raise FileNotFoundError(f"missing canonical source: {source_path}")
    source = Image.open(source_path).convert("RGBA")
    output = upscale_image(source, scale)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output.save(output_path, "PNG", optimize=True)
    return validate_pair(source_path, output_path, scale)


def build(names: Iterable[str], scale: int) -> None:
    for name, source_path, output_path in paths_for(names):
        with Image.open(source_path) as source:
            source_size = source.size
        stats = write_upscaled(source_path, output_path, scale)
        print(
            f"{name:20} {source_size[0]}x{source_size[1]} -> "
            f"{source_size[0] * scale}x{source_size[1] * scale}; "
            f"soft-edge pixels={stats.partial:,}"
        )


def check(names: Iterable[str], scale: int) -> None:
    for name, source_path, output_path in paths_for(names):
        stats = validate_pair(source_path, output_path, scale)
        with Image.open(output_path) as output:
            print(
                f"{name:20} {output.width}x{output.height}; "
                f"soft-edge pixels={stats.partial:,} [PASS]"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--identity", action="append", choices=BASE_NAMES)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    names = args.identity or BASE_NAMES
    if args.check:
        check(names, args.scale)
    else:
        build(names, args.scale)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

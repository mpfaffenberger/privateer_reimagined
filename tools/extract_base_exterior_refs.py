#!/usr/bin/env python3
"""Decode canonical Privateer base exteriors from the user's GOG data.

Outputs transparent PNG frames suitable for reference-conditioned generation.
Raw Origin artwork remains local under generated/ and is never committed.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_bolt_sprites import find_shap  # noqa: E402
from extract_gun_sprites import Reader, decode_shape, load_palette  # noqa: E402

BASE_APPEARANCES = {
    "agricultural": "AGRIC.IFF",
    "new_constantinople": "CONST.IFF",
    "new_detroit": "DETROIT.IFF",
    "oxford": "OXFORD.IFF",
    "perry": "PERRY.IFF",
    "pleasure": "PLEASURE.IFF",
    "refinery": "REFINE.IFF",
    "mining": "ROIDBASE.IFF",
}


def decode_appearance(path: Path, palette: list[tuple[int, int, int]]):
    data = path.read_bytes()
    start, _size = find_shap(data)
    if start is None:
        raise ValueError(f"{path.name}: no APPR/BMAP/SHAP chunk")
    reader = Reader(data)
    reader.pos = start
    return decode_shape(reader)


def compose_tiles(frames, palette: list[tuple[int, int, int]]) -> Image.Image:
    """Reassemble SHAP spatial tiles using their signed source offsets."""
    left = min(frame.x for frame in frames)
    top = min(frame.y for frame in frames)
    right = max(frame.x + frame.w for frame in frames)
    bottom = max(frame.y + frame.h for frame in frames)
    canvas = Image.new("RGBA", (right - left, bottom - top), (0, 0, 0, 0))
    for frame in frames:
        canvas.alpha_composite(frame.to_rgba(palette), (frame.x - left, frame.y - top))
    return canvas


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path,
                        default=Path("gog_extracted/extracted/priv"))
    parser.add_argument("--out", type=Path,
                        default=Path("generated/base_exterior_refs"))
    args = parser.parse_args()

    appearance_dir = args.root / "DATA" / "APPEARNC"
    palette = load_palette(str(args.root / "DATA" / "PALETTE" / "SPACE.PAL"))
    args.out.mkdir(parents=True, exist_ok=True)

    total = 0
    for identity, filename in BASE_APPEARANCES.items():
        frames = decode_appearance(appearance_dir / filename, palette)
        identity_dir = args.out / identity
        identity_dir.mkdir(parents=True, exist_ok=True)
        for index, frame in enumerate(frames):
            frame.to_rgba(palette).save(identity_dir / f"tile_{index:02d}.png")
        composite = compose_tiles(frames, palette)
        composite.save(identity_dir / "composite.png")
        sizes = sorted({(frame.w, frame.h) for frame in frames})
        print(f"{identity:20s} {len(frames):2d} tile(s)  composite={composite.size} "
              f"tile_sizes={sizes}")
        total += len(frames)
    print(f"decoded {total} canonical base tile(s) into {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

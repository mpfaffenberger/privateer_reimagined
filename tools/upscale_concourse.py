#!/usr/bin/env python3
"""upscale_concourse.py — plain NEAREST upscale of every extracted concourse
PNG (backgrounds + flipbook atlases) in place, in-repo replacement for the
old upscale_concourse.sh (which ran the frames through Real-ESRGAN).

Why: Real-ESRGAN's anime model smooths/anti-aliases the hard pixel edges of
WCU's 1990s-era art — great for "upres this photo", wrong for "keep the
retro pixelated look". This project's other legacy-pixel-art pipeline
(build_wcnews_atlas.py) already settled on plain NEAREST upscaling for
exactly this reason; this script brings the concourse set in line with that
convention instead of being the odd one out.

Run AFTER tools/extract_wcu_concourse.py (which writes the native-res PNGs
straight off the WCU source, no resampling of any kind). Nearest-neighbor
just replicates pixels into blocks, so — like the old script's 2x
Real-ESRGAN pass — the atlases' UV ratios in concourse.json need NO changes:
doubling both frame_w/atlas_w (etc.) by the same factor is a no-op on the
stored fractions.

Usage:
    tools/upscale_concourse.py                  # 2x every assets/concourse/*/*.png
    tools/upscale_concourse.py --scale 3
    tools/upscale_concourse.py --dir assets/concourse/mining
"""
import argparse
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parent.parent
DEFAULT_ROOT = REPO / "assets" / "concourse"


def upscale_nearest(path: Path, scale: int) -> tuple[int, int]:
    im = Image.open(path)
    new_size = (im.width * scale, im.height * scale)
    im.resize(new_size, Image.NEAREST).save(path)
    return new_size


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--scale", type=int, default=2, help="integer upscale factor (default 2)")
    ap.add_argument("--dir", default=str(DEFAULT_ROOT),
                     help="root to walk for *.png (default assets/concourse)")
    args = ap.parse_args()

    root = Path(args.dir)
    pngs = sorted(root.glob("*/*.png")) if root == DEFAULT_ROOT else sorted(root.glob("*.png"))
    if not pngs:
        print(f"[upscale] no PNGs found under {root}")
        return

    n = 0
    for f in pngs:
        w, h = upscale_nearest(f, args.scale)
        print(f"  {f.relative_to(REPO)} -> {w}x{h}")
        n += 1
    print(f"[upscale] nearest-upscaled {n} concourse image(s) {args.scale}x")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""sharpen_room_backgrounds.py — mild unsharp-mask pass for the "computer
screen" rooms (Commodity Exchange, Ship Dealer, both Guilds, Bar).

WCU's own source art for these rooms is DXT1-compressed and inherently soft/
painterly (confirmed by decoding bases/Commodity.image with two independent
decoders straight off disk — this is the original WCU asset, not something
our own pipeline blurred). Concourse/landing rooms read fine blown up with
plain nearest-neighbor (tools/upscale_concourse.py); these five read a
little mushy at 2x, so they get one extra pass here.

This is NOT an AI upscale — PIL's UnsharpMask, same category of filter as an
old Photoshop sharpen. Radius/percent/threshold are tuned conservatively
(checked visually) to add edge definition without haloing.

Run AFTER tools/upscale_concourse.py, since it operates on the final-size
PNGs. Idempotent-UNSAFE: running this twice over-sharpens — if you need to
redo it, re-run extract_wcu_concourse.py + upscale_concourse.py first to get
back to a clean base.

Usage:
    tools/sharpen_room_backgrounds.py                 # every archetype
    tools/sharpen_room_backgrounds.py --dir assets/concourse/mining
"""
import argparse
from pathlib import Path

from PIL import Image, ImageFilter

REPO = Path(__file__).resolve().parent.parent
DEFAULT_ROOT = REPO / "assets" / "concourse"

# Room-background filenames this applies to (see build_room() in
# extract_wcu_concourse.py: "<room_key>_bg.png"). NPC/character overlays
# (sd, myg, mtg, btr, patrons, ...) are untouched — they already read fine.
TARGET_BACKGROUNDS = [
    "commodity_bg.png",
    "shipdealer_bg.png",
    "mercguild_bg.png",
    "merchguild_bg.png",
    "bar_bg.png",
]

UNSHARP = dict(radius=2, percent=60, threshold=3)


def sharpen(path: Path) -> None:
    im = Image.open(path)
    im.filter(ImageFilter.UnsharpMask(**UNSHARP)).save(path)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", default=str(DEFAULT_ROOT),
                     help="root to walk for archetype folders (default assets/concourse)")
    args = ap.parse_args()

    root = Path(args.dir)
    archetype_dirs = [root] if (root / TARGET_BACKGROUNDS[0]).exists() else sorted(
        d for d in root.iterdir() if d.is_dir())

    n = 0
    for d in archetype_dirs:
        for name in TARGET_BACKGROUNDS:
            f = d / name
            if not f.exists():
                continue
            sharpen(f)
            print(f"  {f.relative_to(REPO)}")
            n += 1
    print(f"[sharpen] unsharp-masked {n} room background(s)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow"]
# ///
"""Draw the matched street grid over the Oxford plate (#592).

If camera_match.py is right, the u lines (red) run along the streets that go
down-right on screen and the v lines (green) along the ones going up-right,
hugging kerbs and garden walls; the white posts stand 4 m tall on the street
lamps' bases. Every route in routes.json is drawn too, in yellow, and with
--no-grid only the routes.

    uv run tools/room_anim/oxford/check_camera.py --out build/room_anim/oxford/check.png
"""
import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent)]

import camera_match as cm  # noqa: E402
from base import paths  # noqa: E402

LAMPS = [(533, 680)]            # painted street-lamp bases used for the scale
SPAN, STEP = 80, 5              # grid half-extent and spacing, m


def _line(draw, pts, rgb, width=1):
    px = [cm.project(*cm.to_world(u, v)) for u, v in pts]
    draw.line(px, fill=rgb, width=width)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=paths("oxford").build / "check.png")
    ap.add_argument("--routes", type=Path, default=HERE / "routes.json",
                    help="draw each route's (u, v) waypoints (skipped if missing)")
    ap.add_argument("--no-grid", action="store_true", help="routes only")
    args = ap.parse_args()
    img = Image.open(paths("oxford").plate).convert("RGB")
    draw = ImageDraw.Draw(img)
    for k in range(-SPAN, SPAN + 1, STEP) if not args.no_grid else ():
        major = k % 20 == 0
        _line(draw, [(k, -SPAN), (k, SPAN)], (60, 255, 60) if major else (30, 150, 30))
        _line(draw, [(-SPAN, k), (SPAN, k)], (255, 60, 60) if major else (150, 30, 30))
        if major:
            for u, v, label in ((k, 0, f"u{k}"), (0, k, f"v{k}")):
                draw.text(cm.project(*cm.to_world(u, v)), label, fill=(255, 255, 0))
    for bx, by in LAMPS:
        x, y = cm.ground_point(bx, by)
        draw.line([cm.project(x, y, 0.0), cm.project(x, y, 4.0)], fill=(255, 255, 255), width=2)
    if args.routes.exists():
        for name, route in json.loads(args.routes.read_text()).items():
            if name.startswith("_"):
                continue
            _line(draw, [tuple(p) for p in route["uv"]], (255, 230, 0), width=2)
            draw.text(cm.project(*cm.to_world(*route["uv"][0])), name, fill=(255, 230, 0))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    img.save(args.out)
    print(f"[check_camera] {args.out}")


if __name__ == "__main__":
    main()

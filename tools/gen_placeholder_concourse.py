#!/usr/bin/env python3
"""Generate placeholder concourse art for base screens (np-9cu.4).

The real Privateer-style pixel-art concourses are a later art pass (tracked
as a separate backlog issue). Until then each base needs *some* full-screen
background so base_screens.cpp has art to blit and the hotspot layout reads
in screenshots. This tool emits a simple, CC0-by-construction placeholder:

    * a dark vertical gradient (deep space-station blue),
    * the base display name + faction across the top,
    * a labelled translucent rectangle at every hotspot's normalized rect,

so the placeholder visibly agrees with the JSON hotspot layout the engine
draws over it. Reads each base's assets/bases/<id>/base.json (tolerating the
`// line comments` our hand-authored configs use) and writes
assets/bases/<id>/concourse.png next to it.

Usage:
    tools/gen_placeholder_concourse.py                 # all bases under assets/bases
    tools/gen_placeholder_concourse.py achilles hector # just these ids

Idempotent; rerun freely.
"""

import json
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 1280, 720
REPO = Path(__file__).resolve().parent.parent
BASES_DIR = REPO / "assets" / "bases"


def strip_comments(text: str) -> str:
    """Drop `// line comments` so Python's json can parse our configs."""
    return re.sub(r"//[^\n]*", "", text)


def load_font(size: int):
    """Best-effort TrueType; fall back to PIL's bitmap default."""
    for cand in (
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        if Path(cand).exists():
            try:
                return ImageFont.truetype(cand, size)
            except OSError:
                continue
    return ImageFont.load_default()


def gradient(top, bottom):
    img = Image.new("RGBA", (W, H))
    px = img.load()
    for y in range(H):
        t = y / (H - 1)
        r = int(top[0] * (1 - t) + bottom[0] * t)
        g = int(top[1] * (1 - t) + bottom[1] * t)
        b = int(top[2] * (1 - t) + bottom[2] * t)
        for x in range(W):
            px[x, y] = (r, g, b, 255)
    return img


def text_centered(draw, cx, cy, s, font, fill):
    l, t, r, b = draw.textbbox((0, 0), s, font=font)
    draw.text((cx - (r - l) / 2, cy - (b - t) / 2), s, font=font, fill=fill)


def render_base(base_id: str) -> bool:
    cfg_path = BASES_DIR / base_id / "base.json"
    if not cfg_path.exists():
        print(f"[gen_concourse] {base_id}: no base.json, skipped")
        return False
    cfg = json.loads(strip_comments(cfg_path.read_text()))

    name = cfg.get("display_name", base_id)
    # `or` (not just default) so an empty-string faction also falls back —
    # otherwise the subtitle bakes a bare "[]" (e.g. Oxford).
    faction = cfg.get("faction") or "independent"
    hotspots = cfg.get("hotspots", [])

    img = gradient((26, 34, 52), (6, 8, 14))
    draw = ImageDraw.Draw(img, "RGBA")

    title_font = load_font(48)
    sub_font = load_font(24)
    label_font = load_font(22)

    draw.text((40, 36), name, font=title_font, fill=(255, 217, 77, 255))
    draw.text((42, 96), f"[{faction}]", font=sub_font, fill=(160, 180, 210, 255))
    text_centered(draw, W / 2, H * 0.34, "- CONCOURSE (placeholder art) -",
                  sub_font, (120, 140, 170, 255))

    for hs in hotspots:
        rect = hs.get("rect", [0, 0, 0, 0])
        if len(rect) != 4:
            continue
        x0, y0 = rect[0] * W, rect[1] * H
        x1, y1 = x0 + rect[2] * W, y0 + rect[3] * H
        # Boxes only, no labels: base_screens.cpp draws the live hotspot
        # labels over these regions, so baking text here would double up.
        draw.rectangle([x0, y0, x1, y1], fill=(40, 80, 120, 90),
                       outline=(200, 170, 60, 160), width=2)

    out = BASES_DIR / base_id / "concourse.png"
    img.save(out)
    print(f"[gen_concourse] wrote {out.relative_to(REPO)} "
          f"({len(hotspots)} hotspots)")
    return True


def main():
    ids = sys.argv[1:]
    if not ids:
        ids = sorted(p.name for p in BASES_DIR.iterdir() if p.is_dir())
    ok = sum(render_base(b) for b in ids)
    print(f"[gen_concourse] done — {ok}/{len(ids)} base(s)")


if __name__ == "__main__":
    main()

"""Side-by-side canon-vs-generated sheet for judging portrait fidelity.

The failure this catches: a generated ``_ref.png`` can look great on its own
and still be the WRONG PERSON. The only way to see that is to put the original
1993 plate next to it. Each row is one character: canonical source on the left,
our generated ref on the right.

Usage::

    python -m tools.cinematics.canon_compare                # every char with both
    python -m tools.cinematics.canon_compare tayla lynch

Writes ``tools/cinematics/previews/_canon_compare.png``.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

_CELL_H = 300
_PAD = 12
_LABEL_W = 130
_BG = (18, 20, 26)
_FG = (238, 236, 230)
_DIM = (150, 146, 138)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _portraits_root() -> Path:
    return _repo_root() / "assets" / "cinematics" / "portraits"


def _bible() -> dict:
    path = _repo_root() / "assets" / "data" / "characters.json"
    try:
        return json.loads(path.read_text()).get("characters", {})
    except Exception:
        return {}


def _font(size: int):
    for candidate in (
        "/System/Library/Fonts/Supplemental/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        try:
            return ImageFont.truetype(candidate, size)
        except Exception:
            continue
    return ImageFont.load_default()


def _fit(img: Image.Image, height: int) -> Image.Image:
    ratio = height / img.height
    return img.resize(
        (max(1, int(img.width * ratio)), height), Image.Resampling.LANCZOS
    )


def build(characters: list[str] | None = None) -> Path:
    bible = _bible()
    root = _portraits_root()

    rows: list[tuple[str, Path, Path]] = []
    for cid, entry in sorted(bible.items()):
        if characters and cid not in characters:
            continue
        rel = entry.get("reference_image")
        gen = root / cid / "_ref.png"
        if not rel or not gen.is_file():
            continue
        canon = root / rel
        if not canon.is_file():
            continue
        rows.append((cid, canon, gen))

    if not rows:
        raise SystemExit("no characters with BOTH a canon reference_image and a _ref.png")

    name_font = _font(15)
    sub_font = _font(11)
    head_font = _font(14)

    # Measure first so the sheet is exactly wide enough.
    widths = []
    for _, canon, gen in rows:
        cw = _fit(Image.open(canon), _CELL_H).width
        gw = _fit(Image.open(gen), _CELL_H).width
        widths.append(cw + gw + _PAD)
    sheet_w = _LABEL_W + max(widths) + _PAD * 2
    sheet_h = len(rows) * (_CELL_H + _PAD) + _PAD + 26

    sheet = Image.new("RGB", (sheet_w, sheet_h), _BG)
    draw = ImageDraw.Draw(sheet)
    draw.text(
        (_PAD, 7),
        "CANON (1993 original)  ->  GENERATED _ref.png",
        fill=_DIM,
        font=head_font,
    )

    y = 26 + _PAD
    for cid, canon_path, gen_path in rows:
        entry = bible.get(cid, {})
        draw.text((_PAD, y + 6), entry.get("display_name", cid), fill=_FG, font=name_font)
        draw.text((_PAD, y + 24), cid, fill=_DIM, font=sub_font)

        x = _LABEL_W
        for path in (canon_path, gen_path):
            img = _fit(Image.open(path).convert("RGB"), _CELL_H)
            sheet.paste(img, (x, y))
            x += img.width + _PAD
        y += _CELL_H + _PAD

    out_dir = Path(__file__).resolve().parent / "previews"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / "_canon_compare.png"
    sheet.save(out)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("characters", nargs="*")
    args = ap.parse_args(argv)
    out = build(args.characters or None)
    print(f"[canon-compare] wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

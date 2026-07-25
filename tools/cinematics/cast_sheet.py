"""Build a labelled contact sheet of the cast's canonical ``_ref.png`` files.

Judging portraits one folder at a time hides the thing that actually matters:
whether the cast reads as DISTINCT PEOPLE side by side. Two fixers who each
look fine alone can still occupy the same visual register (same age, colouring,
and demeanour), which reads as sloppy the moment they appear in the same scene.

Usage::

    python -m tools.cinematics.cast_sheet                # every character with a _ref
    python -m tools.cinematics.cast_sheet tayla lynch    # just these

Writes ``tools/cinematics/previews/_cast_sheet.png``.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

_THUMB_W = 256
_THUMB_H = 320
_LABEL_H = 34
_PAD = 10
_COLS = 4
_BG = (18, 20, 26)
_FG = (238, 236, 230)
_DIM = (150, 146, 138)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _portraits_dir() -> Path:
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
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        try:
            return ImageFont.truetype(candidate, size)
        except Exception:
            continue
    return ImageFont.load_default()


def build(characters: list[str] | None = None) -> Path:
    bible = _bible()
    root = _portraits_dir()

    if characters:
        entries = [(c, root / c / "_ref.png") for c in characters]
        missing = [c for c, p in entries if not p.is_file()]
        if missing:
            raise SystemExit(f"no _ref.png for: {', '.join(missing)}")
    else:
        entries = sorted(
            ((p.parent.name, p) for p in root.glob("*/_ref.png")),
            key=lambda t: t[0],
        )
    if not entries:
        raise SystemExit("no _ref.png files found")

    cols = min(_COLS, len(entries))
    rows = (len(entries) + cols - 1) // cols
    cell_w = _THUMB_W + _PAD
    cell_h = _THUMB_H + _LABEL_H + _PAD
    sheet = Image.new(
        "RGB", (cols * cell_w + _PAD, rows * cell_h + _PAD + 26), _BG
    )
    draw = ImageDraw.Draw(sheet)
    title_font = _font(15)
    name_font = _font(14)
    sub_font = _font(11)

    draw.text(
        (_PAD, 8),
        f"CAST REFERENCE SHEET  -  {len(entries)} character(s)",
        fill=_DIM,
        font=title_font,
    )

    for i, (cid, path) in enumerate(entries):
        col, row = i % cols, i // cols
        x = _PAD + col * cell_w
        y = _PAD + 26 + row * cell_h
        try:
            thumb = Image.open(path).convert("RGB").resize(
                (_THUMB_W, _THUMB_H), Image.Resampling.LANCZOS
            )
            sheet.paste(thumb, (x, y))
        except Exception as exc:  # keep going; a bad file shouldn't kill the sheet
            draw.rectangle([x, y, x + _THUMB_W, y + _THUMB_H], outline=(120, 40, 40))
            draw.text((x + 8, y + 8), f"unreadable\n{exc}", fill=(200, 90, 90), font=sub_font)

        entry = bible.get(cid, {})
        display = entry.get("display_name", cid)
        draw.text((x, y + _THUMB_H + 4), display, fill=_FG, font=name_font)
        draw.text((x, y + _THUMB_H + 19), cid, fill=_DIM, font=sub_font)

    out_dir = Path(__file__).resolve().parent / "previews"
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / "_cast_sheet.png"
    sheet.save(out)
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("characters", nargs="*", help="character ids (default: all)")
    args = ap.parse_args(argv)
    out = build(args.characters or None)
    print(f"[cast-sheet] wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

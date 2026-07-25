#!/usr/bin/env python3
"""download_wcnews_character_refs.py — pull CANONICAL Privateer character art
from the Combat Information Center wcpedia, for use as portrait references.

Why this exists:
    The cinematic portrait pipeline (tools/cinematics/portraits.py) generates
    each character's ``_ref.png`` from a written appearance description in
    assets/data/characters.json. Those descriptions were authored from the
    dialogue, not from the game, so the generated faces are plausible but NOT
    canonical -- they don't look like the 1993 characters. Conditioning on the
    real artwork fixes that at the source: every per-line portrait already
    conditions on ``_ref.png``, so getting the ref right propagates everywhere.

    The wcpedia hosts the mission-manual ("MM - PRIV - <name>") character
    plates plus in-game conversation stills, which is the ground truth we
    should have started from.

Legal / hygiene:
    These are 1993 Origin Systems assets, not ours. Downloads land in
    assets/cinematics/portraits/_wcnews_refs/ which is GITIGNORED -- we do not
    commit them. They are conditioning references for locally generated art,
    the same posture as tools/download_wcnews_ship_sprites.py.

Usage:
    tools/download_wcnews_character_refs.py --list
    tools/download_wcnews_character_refs.py                 # whole cast
    tools/download_wcnews_character_refs.py tayla lynch
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT_ROOT = REPO / "assets" / "cinematics" / "portraits" / "_wcnews_refs"
API = "https://www.wcnews.com/wcpedia/api.php"
USER_AGENT = "biscuit-the-puppy/1.0 (+research; portrait reference conditioning)"

# character-bible id -> search terms used to find their wcpedia file pages.
# The mission-manual plates are named "MM - PRIV - <Name> <Letter>"; the
# conversation stills are usually "PRIV - <Name>" or appear on mission pages.
CAST: dict[str, list[str]] = {
    "sandoval":  ["MM - PRIV - Sandoval", "Sandoval"],
    "tayla":     ["MM - PRIV - Tayla", "Tayla"],
    "lynch":     ["MM - PRIV - Lynch", "Roman Lynch", "Lynch"],
    "masterson": ["MM - PRIV - Masterson", "Masterson"],
    "murphy":    ["MM - PRIV - Murphy", "Lynn Murphy", "Murphy"],
    "monkhouse": ["MM - PRIV - Monkhouse", "Monkhouse"],
    "cross":     ["MM - PRIV - Cross", "Taryn Cross", "Tayrn Cross"],
    "terrell":   ["MM - PRIV - Terrell", "Admiral Terrell", "Terrell"],
    "goodin":    ["MM - PRIV - Goodin", "Sandra Goodin", "Goodin"],
    "miggs":     ["MM - PRIV - Miggs", "Miggs"],
}


def _api(params: dict) -> dict:
    params = {**params, "format": "json"}
    url = f"{API}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=30) as fh:
        return json.loads(fh.read().decode("utf-8", "replace"))


def find_files(term: str, limit: int = 40) -> list[str]:
    """Search the File: namespace (ns=6) for image pages matching ``term``."""
    try:
        data = _api(
            {
                "action": "query",
                "list": "search",
                "srsearch": term,
                "srnamespace": 6,
                "srlimit": limit,
            }
        )
    except Exception as exc:
        print(f"    ! search failed for {term!r}: {exc}")
        return []
    return [r["title"] for r in data.get("query", {}).get("search", [])]


def file_url(title: str) -> tuple[str, int, int] | None:
    try:
        data = _api(
            {
                "action": "query",
                "titles": title,
                "prop": "imageinfo",
                "iiprop": "url|size",
            }
        )
    except Exception:
        return None
    for page in data.get("query", {}).get("pages", {}).values():
        for info in page.get("imageinfo", []):
            url = info.get("url")
            if url:
                return url, int(info.get("width") or 0), int(info.get("height") or 0)
    return None


def download(url: str, dest: Path) -> bool:
    dest.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=60) as fh:
            dest.write_bytes(fh.read())
        return True
    except Exception as exc:
        print(f"    ! download failed: {exc}")
        return False


def fetch_character(cid: str, terms: list[str], *, dry_run: bool = False) -> int:
    print(f"\n=== {cid} ===")
    seen: set[str] = set()
    hits: list[tuple[str, str, int, int]] = []
    for term in terms:
        for title in find_files(term):
            if title in seen:
                continue
            seen.add(title)
            got = file_url(title)
            if not got:
                continue
            url, w, h = got
            # Skip obvious non-portraits: maps, logos, tiny icons.
            if w < 80 or h < 80:
                continue
            hits.append((title, url, w, h))
        time.sleep(0.3)

    if not hits:
        print("    (no image pages found)")
        return 0

    for title, url, w, h in hits:
        name = title.replace("File:", "").replace(" ", "_")
        print(f"    {w:>4}x{h:<4}  {name}")
        if not dry_run:
            download(url, OUT_ROOT / cid / name)
            time.sleep(0.3)
    return len(hits)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("characters", nargs="*", help="bible ids (default: all)")
    ap.add_argument("--list", action="store_true", help="show hits, download nothing")
    args = ap.parse_args(argv)

    ids = args.characters or list(CAST)
    unknown = [c for c in ids if c not in CAST]
    if unknown:
        raise SystemExit(f"unknown character id(s): {', '.join(unknown)}")

    total = 0
    for cid in ids:
        total += fetch_character(cid, CAST[cid], dry_run=args.list)

    where = "(dry run)" if args.list else str(OUT_ROOT)
    print(f"\n{total} image(s) {'found' if args.list else 'saved'} -> {where}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

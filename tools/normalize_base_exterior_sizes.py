#!/usr/bin/env python3
"""Normalize generated base exterior sizes across authored system files."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from base_exterior_policy import BASE_SPRITES, display_length_meters

REPO = Path(__file__).resolve().parents[1]
SYSTEMS_DIR = REPO / "assets" / "systems"
PLACEMENT_RE = re.compile(
    r'(?P<prefix>"sprite"\s*:\s*"(?P<sprite>sprites/base_[^"]+)"'
    r'(?:(?!\n\s*\}).)*?"length_meters"\s*:\s*)'
    r'(?P<length>\d+(?:\.\d+)?)',
    re.DOTALL,
)


def normalize_text(text: str) -> tuple[str, int, list[str]]:
    """Return normalized text, recognized placement count, and errors."""
    count = 0
    errors: list[str] = []

    def replace(match: re.Match[str]) -> str:
        nonlocal count
        sprite = match.group("sprite")
        if sprite not in BASE_SPRITES:
            errors.append(f"unknown generated base sprite: {sprite}")
            return match.group(0)
        count += 1
        return f'{match.group("prefix")}{display_length_meters(sprite)}'

    return PLACEMENT_RE.sub(replace, text), count, errors


def process(write: bool) -> int:
    placements = 0
    references = 0
    changed_files = 0
    errors: list[str] = []
    for path in sorted(SYSTEMS_DIR.glob("*.json")):
        original = path.read_text(encoding="utf-8")
        references += sum(original.count(f'"{sprite}"') for sprite in BASE_SPRITES)
        normalized, count, file_errors = normalize_text(original)
        placements += count
        errors.extend(f"{path.name}: {error}" for error in file_errors)
        if normalized != original:
            if write:
                path.write_text(normalized, encoding="utf-8")
                changed_files += 1
            else:
                errors.append(f"{path.name}: non-canonical base exterior size")

    if placements == 0:
        errors.append("no generated base exterior placements found")
    if references != placements:
        errors.append(
            f"found {references} generated base references but only "
            f"{placements} placements with length_meters"
        )
    for error in errors:
        print(f"ERROR: {error}")
    action = "updated" if write else "validated"
    print(f"{action}: {placements} placements across {changed_files} changed files")
    return 1 if errors else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="rewrite non-canonical sizes")
    args = parser.parse_args()
    return process(args.write)


if __name__ == "__main__":
    raise SystemExit(main())

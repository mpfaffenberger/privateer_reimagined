#!/usr/bin/env python3
"""Assign canonical exterior sprites to dockable bases in system JSON files."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from build_system_from_wcpedia import exterior_sprite_for

REPO = Path(__file__).resolve().parents[1]


def update_system(path: Path) -> tuple[int, list[str]]:
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return 0, [f"{path.name}: skipped nonstandard JSON"]

    by_position = {
        tuple(nav.get("position", [])): nav
        for nav in root.get("nav_points", [])
        if nav.get("dockable") and nav.get("base_id")
    }
    changed = 0
    notes: list[str] = []
    for sprite in root.get("placed_sprites", []):
        nav = by_position.get(tuple(sprite.get("position", [])))
        if not nav:
            continue
        stem = exterior_sprite_for(str(nav["base_id"]))
        if sprite.get("sprite") == stem:
            continue
        notes.append(f"{path.stem}/{nav['base_id']}: {sprite.get('sprite')} -> {stem}")
        sprite["sprite"] = stem
        changed += 1
    if changed:
        path.write_text(json.dumps(root, indent=2) + "\n", encoding="utf-8")
    return changed, notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--systems", type=Path, default=REPO / "assets/systems")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    total = 0
    all_notes: list[str] = []
    for path in sorted(args.systems.glob("*.json")):
        before = path.read_bytes()
        changed, notes = update_system(path)
        if args.dry_run and changed:
            path.write_bytes(before)
        total += changed
        all_notes.extend(notes)
    print("\n".join(all_notes))
    print(f"{'would update' if args.dry_run else 'updated'} {total} base sprite assignment(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

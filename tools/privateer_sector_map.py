#!/usr/bin/env python3
"""Extract canonical 2D system positions from Privateer's QUADRANT.IFF.

Each FORM/SYST INFO payload is:
    uint8 system_id, int16_le x, int16_le y, char name[]

This is deliberately separate from the signed 24-bit 3D coordinates used by
other Privateer records. The chart stores only signed 16-bit X/Y positions.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = REPO / "generated/gog_extracted/priv/DATA/SECTORS/QUADRANT.IFF"
DEFAULT_GALAXY = REPO / "assets/galaxy.json"

# Canonical data and the WCPedia-derived catalog disagree on three labels.
ALIASES = {
    "testbed": "eden",
    "44pim": "44p1m",
    "hindsvariablen": "hindsvariablenorth",
}


def normalize_name(name: str) -> str:
    key = "".join(ch for ch in name.casefold() if ch.isalnum())
    return ALIASES.get(key, key)


def load_positions(path: Path) -> dict[str, tuple[int, int]]:
    """Return normalized system name -> canonical (x, y)."""
    data = path.read_bytes()
    positions: dict[str, tuple[int, int]] = {}
    cursor = 0
    marker = b"SYSTINFO"

    while (offset := data.find(marker, cursor)) >= 0:
        size_at = offset + len(marker)
        if size_at + 4 > len(data):
            raise ValueError(f"truncated INFO size at 0x{offset:x}")
        size = int.from_bytes(data[size_at:size_at + 4], "big")
        payload_at = size_at + 4
        payload = data[payload_at:payload_at + size]
        if len(payload) != size or size < 7:
            raise ValueError(f"invalid SYST/INFO payload at 0x{offset:x}")

        x = int.from_bytes(payload[1:3], "little", signed=True)
        y = int.from_bytes(payload[3:5], "little", signed=True)
        name = payload[5:].split(b"\0", 1)[0].decode("cp437")
        key = normalize_name(name.strip("#"))
        if key in positions:
            raise ValueError(f"duplicate system name in QUADRANT.IFF: {name}")
        positions[key] = (x, y)
        cursor = payload_at + size

    if not positions:
        raise ValueError(f"no SYST/INFO records found in {path}")
    return positions


def update_galaxy(source: Path, galaxy_path: Path) -> int:
    positions = load_positions(source)
    galaxy = json.loads(galaxy_path.read_text())
    missing: list[str] = []

    for system in galaxy["systems"]:
        position = positions.get(normalize_name(system["display_name"]))
        if position is None:
            missing.append(system["display_name"])
            continue
        system["galaxy_position"] = list(position)

    if missing:
        raise ValueError("systems missing canonical positions: " + ", ".join(missing))

    galaxy_path.write_text(json.dumps(galaxy, indent=2) + "\n")
    print(f"updated {len(galaxy['systems'])} systems from {source}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--galaxy", type=Path, default=DEFAULT_GALAXY)
    args = parser.parse_args()
    return update_galaxy(args.source, args.galaxy)


if __name__ == "__main__":
    raise SystemExit(main())

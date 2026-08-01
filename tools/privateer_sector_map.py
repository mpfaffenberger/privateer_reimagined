#!/usr/bin/env python3
"""Extract canonical chart records from Privateer's QUADRANT.IFF.

Each FORM/SYST INFO payload is:
    uint8 system_id, int16_le x, int16_le y, char name[]

SYST forms are nested in FORM/QUAD, whose INFO payload supplies the canonical
quadrant name. These signed 16-bit chart coordinates are unrelated to the
signed 24-bit 3D coordinates used by other Privateer records.
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


def _children(data: bytes, start: int, end: int):
    """Yield (chunk_id, form_type, payload_start, payload_end) for IFF chunks."""
    offset = start
    while offset + 8 <= end:
        chunk_id = data[offset:offset + 4]
        size = int.from_bytes(data[offset + 4:offset + 8], "big")
        payload_start = offset + 8
        payload_end = payload_start + size
        if payload_end > end:
            raise ValueError(f"truncated {chunk_id!r} chunk at 0x{offset:x}")
        if chunk_id == b"FORM":
            if size < 4:
                raise ValueError(f"short FORM chunk at 0x{offset:x}")
            yield chunk_id, data[payload_start:payload_start + 4], payload_start + 4, payload_end
        else:
            yield chunk_id, None, payload_start, payload_end
        offset = payload_end + (size & 1)  # IFF chunks are word-aligned.


def _info_payload(data: bytes, start: int, end: int) -> bytes:
    for chunk_id, _, payload_start, payload_end in _children(data, start, end):
        if chunk_id == b"INFO":
            return data[payload_start:payload_end]
    raise ValueError("FORM is missing its INFO chunk")


def load_records(path: Path) -> dict[str, tuple[int, int, str]]:
    """Return normalized system name -> canonical (x, y, quadrant_name)."""
    data = path.read_bytes()
    if data[:4] != b"FORM":
        raise ValueError(f"not an IFF FORM: {path}")
    root_size = int.from_bytes(data[4:8], "big")
    root_end = 8 + root_size
    if root_end > len(data) or data[8:12] != b"UNIV":
        raise ValueError(f"not a QUADRANT.IFF UNIV form: {path}")

    records: dict[str, tuple[int, int, str]] = {}
    for chunk_id, form_type, quad_start, quad_end in _children(data, 12, root_end):
        if chunk_id != b"FORM" or form_type != b"QUAD":
            continue
        quad_info = _info_payload(data, quad_start, quad_end)
        if len(quad_info) < 5:
            raise ValueError("short QUAD/INFO payload")
        quadrant = quad_info[4:].split(b"\0", 1)[0].decode("cp437")

        for child_id, child_form, syst_start, syst_end in _children(data, quad_start, quad_end):
            if child_id != b"FORM" or child_form != b"SYST":
                continue
            payload = _info_payload(data, syst_start, syst_end)
            if len(payload) < 7:
                raise ValueError("short SYST/INFO payload")
            x = int.from_bytes(payload[1:3], "little", signed=True)
            y = int.from_bytes(payload[3:5], "little", signed=True)
            name = payload[5:].split(b"\0", 1)[0].decode("cp437")
            key = normalize_name(name.strip("#"))
            if key in records:
                raise ValueError(f"duplicate system name in QUADRANT.IFF: {name}")
            records[key] = (x, y, quadrant)

    if not records:
        raise ValueError(f"no SYST/INFO records found in {path}")
    return records


def load_positions(path: Path) -> dict[str, tuple[int, int]]:
    """Return normalized system name -> canonical (x, y)."""
    return {key: (x, y) for key, (x, y, _) in load_records(path).items()}


def update_galaxy(source: Path, galaxy_path: Path) -> int:
    records = load_records(source)
    galaxy = json.loads(galaxy_path.read_text())
    missing: list[str] = []

    for system in galaxy["systems"]:
        record = records.get(normalize_name(system["display_name"]))
        if record is None:
            missing.append(system["display_name"])
            continue
        x, y, quadrant = record
        system["galaxy_position"] = [x, y]
        system["sector"] = f"{quadrant} Quadrant"

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

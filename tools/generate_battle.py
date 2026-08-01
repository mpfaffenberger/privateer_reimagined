#!/usr/bin/env python3
"""Regenerate capital_line_battle.json with tighter spacing."""
import json

paradigm_z = -2000
kamekh_z = 2000
spacing = 500

ships = []
for i in range(20):
    x = (i - 9.5) * spacing
    ships.append({
        "atlas": "ships/paradigm/atlas_manifest_3d",
        "position": [x, 0, paradigm_z],
        "length_meters": 250,
        "lights_enabled": True,
        "faction": "confed",
        "ai": {"initial_state": "engage"}
    })
for i in range(20):
    x = (i - 9.5) * spacing
    ships.append({
        "atlas": "ships/kamekh/atlas_manifest_3d",
        "position": [x, 0, kamekh_z],
        "length_meters": 180,
        "lights_enabled": True,
        "faction": "kilrathi",
        "ai": {"initial_state": "engage"}
    })

system = {
    "name": "Capital Line Battle",
    "description": "20 Paradigms vs 20 Kamekhs — two parallel battle lines, 4km apart.",
    "skybox_seed": "sector_4774",
    "star": {"preset": "blue"},
    "asteroid_fields": [],
    "placed_meshes": [],
    "placed_sprites": [],
    "placed_ship_sprites": ships,
    "nav_points": [{"name": "Center", "kind": "nav", "position": [0, 0, 0]}],
    "encounters": [],
    "player_start": {
        "position": [0, 10000, 0],
        "look_at": [0, 0, 0]
    }
}

header = """// ---------------------------------------------------------------------
// capital_line_battle.json — 20v20 CAPITAL FLEET ENGAGEMENT
//
//   20× Paradigm (Confed capital)  — Z = -2000
//   20× Kamekh   (Kilrathi capital) — Z = +2000
//
// Both lines start in "engage" state. 4000m gap — close enough for
// immediate lock-on without bumper-car collisions.
//
// Ships per line: 20, spaced 500m apart (~10km total span).
// Camera: 10km above center, looking straight down.
//
// Launch with:  ./build/new_privateer --system capital_line_battle --skip-title
// ---------------------------------------------------------------------
"""

with open("assets/systems/capital_line_battle.json", "w") as f:
    f.write(header)
    json.dump(system, f, indent=2)
    f.write("\n")

print(f"Done — {len(ships)} ships, 4km gap, 500m spacing")

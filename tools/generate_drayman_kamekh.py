#!/usr/bin/env python3
"""Generate drayman_vs_kamekh.json — 4 Draymen vs 2 Kamekhs."""
import json, textwrap

ships = []

# 4 Draymen — Confed merchant line at Z = -2000
for i in range(4):
    ships.append({
        "atlas":          "ships/drayman/atlas_manifest_3d",
        "position":       [(i - 1.5) * 500, 0, -2000],
        "length_meters":  60,
        "lights_enabled": True,
        "faction":        "confed",
        "ai":             {"initial_state": "engage"}
    })

# 2 Kamekhs — Kilrathi capital line at Z = +2000
for i in range(2):
    ships.append({
        "atlas":          "ships/kamekh/atlas_manifest_3d",
        "position":       [(i - 0.5) * 800, 0, 2000],
        "length_meters":  180,
        "lights_enabled": True,
        "faction":        "kilrathi",
        "ai":             {"initial_state": "engage"}
    })

system = {
    "name":        "4 Draymen vs 2 Kamekhs",
    "description": "4 Confed Draymen vs 2 Kilrathi Kamekhs — merchant swarm vs capital ships.",
    "skybox_seed": "sector_4774",
    "star":        {"preset": "blue"},
    "asteroid_fields": [],
    "placed_meshes": [],
    "placed_sprites": [],
    "placed_ship_sprites": ships,
    "nav_points": [
        {"name": "Center", "kind": "nav", "position": [0, 0, 0]}
    ],
    "encounters": [],
    "player_start": {
        "position": [0, 6000, 0],
        "look_at":  [0, 0, 0]
    }
}

header = textwrap.dedent("""\
    // ---------------------------------------------------------------------
    // drayman_vs_kamekh.json — 4 DRAYMEN vs 2 KAMEKHS
    //
    // 4 Confed Draymen (merchant ships, 2 meson blasters each, 35cm armor)
    // vs 2 Kilrathi Kamekhs (capital ships, 4 turreted lasers, 2 turreted
    // plasma cannons, DF/HS/IR missiles, 70cm armor).
    //
    // The merchants swarm in, the Kamekhs rain hell. Who wins?
    //
    // Camera: 6km above center, looking straight down.
    //
    // Launch with:  ./build/new_privateer --system drayman_vs_kamekh --skip-title
    // ---------------------------------------------------------------------
    """)

with open("assets/systems/drayman_vs_kamekh.json", "w") as f:
    f.write(header)
    json.dump(system, f, indent=2)
    f.write("\n")

print("Written assets/systems/drayman_vs_kamekh.json")
print("  4 Draymen (Confed merchants) vs 2 Kamekhs (Kilrathi capitals)")

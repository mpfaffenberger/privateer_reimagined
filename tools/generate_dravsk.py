#!/usr/bin/env python3
"""Generate drayman_vs_kamekh.json — 4 Draymen vs 2 Kamekhs."""
import json, textwrap

ships = []

# 4 Draymen — Confed merchant line at Z = -2000
for i in range(4):
    ships.append({
        "atlas":          "ships/drayman/atlas_manifest_3d",
        "position":       [(i - 1.5) * 400, 0, -2000],
        "length_meters":  60,
        "lights_enabled": True,
        "faction":        "confed",
        "ai":             {"initial_state": "engage"}
    })

# 2 Kamekhs — Kilrathi capital line at Z = +2000
for i in range(2):
    ships.append({
        "atlas":          "ships/kamekh/atlas_manifest_3d",
        "position":       [(i - 0.5) * 600, 0, 2000],
        "length_meters":  180,
        "lights_enabled": True,
        "faction":        "kilrathi",
        "ai":             {"initial_state": "engage"}
    })

system = {
    "name":        "4 Draymen vs 2 Kamekhs",
    "description": "4 Confederation Draymen merchants vs 2 Kilrathi Kamekhs — merchant swarm vs capital ships.",
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
        "position": [0, 5000, 0],
        "look_at":  [0, 0, 0]
    }
}

header = textwrap.dedent("""\
    // ---------------------------------------------------------------------
    // drayman_vs_kamekh.json — 4 DRA YMEN vs 2 KAMEKHS
    //
    // 4 Confed Draymen (merchants) vs 2 Kilrathi Kamekhs (capitals).
    // The Draymen are basically brick walls with engines, but they have
    // 2 meson blasters and 35cm frontal armor each.
    //
    // The Kamekhs are absolute warships: 4 turreted lasers, 2 turreted
    // plasma cannons, DF/HS/IR missiles, and 70cm frontal armor.
    //
    // Who wins? The merchants swarm or the capital ships obliterate them?
    //
    // Camera: 5km above center, looking straight down.
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

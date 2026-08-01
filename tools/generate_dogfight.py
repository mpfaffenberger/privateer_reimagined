#!/usr/bin/env python3
"""Generate a massive 100 Dralthi vs 80 Talons dogfight scene."""
import json, random, textwrap

random.seed(42)

ships = []

# 100 Kilrathi Dralthi - scattered in a large volume
for i in range(100):
    x = round(random.gauss(0, 2000))
    y = round(random.gauss(0, 800))
    z = round(random.gauss(-1500, 1000))
    ships.append({
        "atlas":          "ships/dralthi/atlas_manifest_3d",
        "position":       [x, y, z],
        "length_meters":  18,
        "lights_enabled": True,
        "faction":        "kilrathi",
        "ai":             {"initial_state": "engage"}
    })

# 80 Militia Talons - scattered in a large volume, offset
for i in range(80):
    x = round(random.gauss(0, 2000))
    y = round(random.gauss(0, 800))
    z = round(random.gauss(1500, 1000))
    ships.append({
        "atlas":          "ships/talon/atlas_manifest_3d",
        "position":       [x, y, z],
        "length_meters":  18,
        "lights_enabled": True,
        "faction":        "militia",
        "ai":             {"initial_state": "engage"}
    })

system = {
    "name":        "180-Ship Dogfight",
    "description": "100 Dralthi vs 80 Talons — massive fighter cloud battle.",
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
        "position": [0, 15000, 0],
        "look_at":  [0, 0, 0]
    }
}

header = textwrap.dedent("""\
    // ---------------------------------------------------------------------
    // dogfight.json — 100 DRALTHI vs 80 TALONS
    //
    // 180 fighters total, two overlapping volume clouds:
    //
    //   100× Dralthi (Kilrathi light fighter) — cloud centered at Z=-1500
    //    80× Talons  (Militia light fighter)  — cloud centered at Z=+1500
    //
    // Each cloud spans roughly 6km × 3km × 4km. Clouds overlap in the
    // center zone for immediate dogfighting chaos.
    //
    // Every ship in "engage" — instant mayhem.
    //
    // Camera: 15km above center, looking straight down at the carnage.
    //
    // Launch with:  ./build/new_privateer --system dogfight --skip-title
    // ---------------------------------------------------------------------
    """)

with open("assets/systems/dogfight.json", "w") as f:
    f.write(header)
    json.dump(system, f, indent=2)
    f.write("\n")

dralthi = [s for s in ships if "dralthi" in s["atlas"]]
talons = [s for s in ships if "talon" in s["atlas"]]
all_x = [s["position"][0] for s in ships]
all_y = [s["position"][1] for s in ships]
all_z = [s["position"][2] for s in ships]
print(f"Written assets/systems/dogfight.json")
print(f"  {len(dralthi)} Dralthi + {len(talons)} Talons = {len(ships)} ships")
print(f"  Bounds: X [{min(all_x)}, {max(all_x)}], Y [{min(all_y)}, {max(all_y)}], Z [{min(all_z)}, {max(all_z)}]")
print(f"  O(N²) perception: {len(ships)**2} checks — may be heavy!")

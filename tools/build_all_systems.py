#!/usr/bin/env python3
"""build_all_systems.py — generate every Gemini system JSON + the galaxy graph.

Walks assets/data/gemini_systems.json, runs the per-system converter
(build_system_from_wcpedia.build) for all 71 systems, then emits
assets/galaxy.json: the system catalog + the directed jump topology derived
from each system's jump-point nav links.

  * System id   = snake(clean_name(name))   ("Palan (star system)" -> "palan")
  * Skybox seed = the system id              (procedural, unique per system)
  * Jump edge   = for each jump nav in S pointing to D (where D is a real
    generated system): {from:S, from_nav:"<D> Jump", to:D, to_nav:"<S> Jump"}.
    Jump navs pointing OUTSIDE the generated set (Kilrathi/edge systems, source
    typos) stay in the system JSON as dangling frontier gates but get no
    galaxy edge.

Troy's hand-set player_start (off Achilles) is re-applied after generation so
the batch doesn't clobber it.

Usage: tools/build_all_systems.py
"""
from __future__ import annotations
import json
from pathlib import Path

import build_system_from_wcpedia as conv

REPO   = Path(__file__).resolve().parents[1]
DATA   = REPO / "assets" / "data" / "gemini_systems.json"
GALAXY = REPO / "assets" / "galaxy.json"
SCALE  = 10.0 / 3.0

# Coarse 2D galaxy-map layout: quadrant -> base cell, systems gridded within.
QUAD_BASE = {
    "Fariss Quadrant":   (-6,  6),   # NW
    "Clarke Quadrant":   ( 6,  6),   # NE
    "Humboldt Quadrant": (-6, -6),   # SW
    "Potter Quadrant":   ( 6, -6),   # SE
}


# Non-system wcpedia pages that slipped into the scrape (sector overview /
# test fixture) — not real star systems, so don't generate them.
BLOCKLIST = {"Test Bed"}


def main() -> int:
    data = json.loads(DATA.read_text())
    systems = [s for s in data["systems"]
               if conv.clean_name(s["name"]) not in BLOCKLIST
               and s.get("nav_points")]

    # First pass: assign ids; know which jump targets are real.
    id_of = {}
    for s in systems:
        id_of[s["name"]] = conv.snake(conv.clean_name(s["name"]))
    real_ids = set(id_of.values())

    catalog = []     # galaxy "systems" entries
    jumps = []       # galaxy "jumps" entries
    quad_counts: dict[str, int] = {}

    built   = {}   # sid -> engine dict (kept so we can synthesize navs)
    display_of = {}
    for s in systems:
        sid     = id_of[s["name"]]
        display = conv.clean_name(s["name"])
        quad    = s.get("info", {}).get("Quadrant", "Gemini")
        sector  = s.get("info", {}).get("Sector", "Gemini Sector")
        display_of[sid] = display
        built[sid] = conv.build(s, sid, skybox=sid, scale=SCALE)

        bx, by = QUAD_BASE.get(quad, (0, 0))
        k = quad_counts.get(quad, 0); quad_counts[quad] = k + 1
        catalog.append({
            "id": sid, "display_name": display,
            "sector": quad or sector,
            "galaxy_position": [bx + (k % 5) - 2, by + (k // 5) - 2],
            "json_path": f"assets/systems/{sid}.json",
        })

    # Reciprocity pass: every jump A->B implies a return gate on B. If B's
    # scrape didn't capture a '<A> Jump' nav (one-way / lossy source data),
    # synthesize one so the jump back resolves and the graph is bidirectional.
    SYS = REPO / "assets" / "systems"
    modified = set()
    for sid, out in list(built.items()):
        for nav in list(out["nav_points"]):
            if nav.get("kind") != "jump":
                continue
            dest = nav.get("links_to", "")
            if dest not in built or dest == sid:
                continue
            want = f"{display_of[sid]} Jump"   # return gate name on dest's side
            dnavs = built[dest]["nav_points"]
            if any(n["name"] == want for n in dnavs):
                continue
            # place the synthetic gate on a ring around dest's spread of navs
            ring = 150000
            import math
            ang = (len([n for n in dnavs if n.get("kind") == "jump"]) * 2.399963)
            pos = [round(math.cos(ang) * ring), 0, round(math.sin(ang) * ring)]
            dnavs.append({"name": want, "kind": "jump", "position": pos,
                          "links_to": sid, "links_to_nav": f"{display_of[dest]} Jump"})
            modified.add(dest)

    for sid in modified:
        (SYS / f"{sid}.json").write_text(json.dumps(built[sid], indent=2) + "\n")

    # Emit the directed jump graph from the (now reciprocal) nav lists.
    for sid, out in built.items():
        for nav in out["nav_points"]:
            if nav.get("kind") != "jump":
                continue
            dest = nav.get("links_to", "")
            if dest in built and dest != sid:
                jumps.append({
                    "from": sid, "from_nav": nav["name"],
                    "to": dest, "to_nav": f"{display_of[sid]} Jump",
                })

    GALAXY.write_text(json.dumps({"systems": catalog, "jumps": jumps},
                                 indent=2) + "\n")

    # Re-apply Troy's hand-set spawn (off Achilles) the batch just reset.
    troy = REPO / "assets" / "systems" / "troy.json"
    if troy.exists():
        d = json.loads(troy.read_text())
        d["player_start"] = {
            "comment": "Spawn ~6 km off Achilles Mining Base (Nav 2), looking at it.",
            "position": [200000, -133333, 6000], "look_at": [200000, -133333, 0],
        }
        troy.write_text(json.dumps(d, indent=2) + "\n")

    n_jump_navs = 0
    n_dangling = 0
    for s in systems:
        sid = id_of[s["name"]]
        p = REPO / "assets" / "systems" / f"{sid}.json"
        nd = json.loads(p.read_text())
        for nav in nd["nav_points"]:
            if nav.get("kind") == "jump":
                n_jump_navs += 1
                if nav.get("links_to") not in real_ids:
                    n_dangling += 1

    print(f"generated {len(catalog)} systems")
    print(f"galaxy: {len(catalog)} nodes, {len(jumps)} directed jump edges")
    print(f"jump navs: {n_jump_navs} total, {n_dangling} dangling (target outside sector)")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())

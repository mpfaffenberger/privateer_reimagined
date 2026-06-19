#!/usr/bin/env python3
"""build_system_from_wcpedia.py — convert one scraped wcpedia system into an
engine StarSystem JSON (assets/systems/<out>.json).

Reads assets/data/gemini_systems.json (produced by scrape_wcpedia_systems.py)
and emits nav points, jump links, dockable bases/planets, asteroid fields, and
an encounter rule, all in our engine's schema.

Coordinate convention (VERIFIED against the hand-authored troy.json — Achilles
and Helen map to the unit):
    engine = (wiki.x, wiki.z, wiki.y) * SCALE
The wiki stores (X, Y_depth, Z_up); our engine is (X, Y_up, Z_depth). So the
wiki Z (up) becomes our Y, and the wiki Y (depth) becomes our Z. SCALE = 10/3
is the established Troy fudge factor (raw too small, 10x too big).

Usage:
  tools/build_system_from_wcpedia.py Troy --out troy_canonical
  tools/build_system_from_wcpedia.py "Pender's Star" --out penders_star
"""
from __future__ import annotations
import argparse, json, re, sys
from pathlib import Path

REPO  = Path(__file__).resolve().parents[1]
DATA  = REPO / "assets" / "data" / "gemini_systems.json"
SYS   = REPO / "assets" / "systems"
SCALE = 10.0 / 3.0

# wcpedia faction label -> our faction key. Tolerates the source typos.
FACTION = {
    "pirate": "pirate", "militia": "militia", "retro": "retro",
    "merchant": "merchant", "kilrathi": "kilrathi",
    "confederation": "confed", "conederation": "confed",
    "bounty hunter": "hunter", "bounty hunters": "hunter",
}
# wcpedia ship -> our ship-class key. Ships without their own ship.json fall
# back to the nearest hull we DO have a class for (documented per line).
SHIP = {
    "talon": "talon", "tarsus": "tarsus", "galaxy": "galaxy",
    "centurion": "centurion", "orion": "orion", "broadsword": "broadsword",
    "paradigm": "paradigm", "stiletto": "stiletto",
    "drayman": "galaxy",      # heavy merchant transport -> our merchant hull
    "demon":   "centurion",   # bounty-hunter fighter -> centurion (also hunter)
    "dralthi": "talon",       # Kilrathi light fighter -> Talon stand-in
    "gothri":  "orion",       # Kilrathi heavy -> gunship stand-in
    "kamekh":  "paradigm",    # Kilrathi corvette -> capital stand-in
    "gladius": "centurion",   # Confed light fighter -> centurion stand-in
    "merchant": "tarsus",     # column-shift artifact -> harmless small hull
}


def snake(name: str) -> str:
    # Drop apostrophes outright ("Pender's Star" -> "penders_star", not
    # "pender_s_star") before collapsing the rest to underscores.
    s = name.strip().lower().replace("'", "").replace("’", "")
    s = re.sub(r"[^a-z0-9]+", "_", s).strip("_")
    return s


def emap(x: int, y: int, z: int) -> list[int]:
    """wiki (x, y_depth, z_up) -> engine (x, y_up, z_depth) * SCALE."""
    return [round(x * SCALE), round(z * SCALE), round(y * SCALE)]


def load_system(name: str) -> dict:
    d = json.loads(DATA.read_text())
    for s in d["systems"]:
        if s["name"] == name or snake(s["name"]) == snake(name):
            return s
    sys.exit(f"system '{name}' not found in {DATA.name}")


def nav_from(desc: str, pos: list[int], idx: int, sysname: str):
    """Return (navpoint dict, optional sprite dict, optional asteroid dict)."""
    d = desc.strip()
    low = d.lower()
    back = f"{sysname} Jump"   # convention: arrival nav on the far side
    if low.startswith("jump point"):
        dest = re.sub(r"(?i)^jump point\s*[-–]\s*", "", d).strip()
        return ({"name": f"{dest} Jump", "kind": "jump", "position": pos,
                 "links_to": snake(dest), "links_to_nav": back}, None, None)
    if "mining base" in low or low.endswith("base") or "station" in low:
        bid = snake(d.split(" Mining")[0].split(" Base")[0])
        nav = {"name": d, "kind": "station", "position": pos,
               "dockable": True, "base_id": bid}
        spr = {"sprite": "sprites/mining_base", "position": pos, "length_meters": 3000}
        return (nav, spr, None)
    if "planet" in low:
        bid = snake(d.split(" ")[0])
        nav = {"name": d, "kind": "planet", "position": pos,
               "dockable": True, "base_id": bid}
        spr = {"sprite": "sprites/helen_planet", "position": pos, "length_meters": 2000}
        return (nav, spr, None)
    if "asteroid" in low:
        nav = {"name": d or f"Asteroid Field {idx}", "kind": "nav", "position": pos}
        ast = {"center": pos, "half_extent": [40000, 12000, 40000],
               "count": 320, "base_radius": 120, "seed": 0xA57E000 + idx}
        return (nav, None, ast)
    # plain / empty nav (arrival beacon, deep-space waypoint)
    return ({"name": d or f"{sysname} Nav {idx}", "kind": "nav", "position": pos},
            None, None)


def build_encounters(src: dict) -> list[dict]:
    """Aggregate the per-nav spawn groups into ONE weighted 'anywhere' rule
    (faction mix + class mix), weighted by chance% x ship count. Per-nav
    region fidelity waits on the engine 'nav' region (future work)."""
    fac_w: dict[str, float] = {}
    cls_w: dict[str, float] = {}
    maxgrp = 2
    for groups in src["encounters"].values():
        for g in groups:
            ch = g["chance_pct"] if isinstance(g["chance_pct"], (int, float)) else 0
            grp_ships = 0
            for m in g["members"]:
                cnt = m["count"] if isinstance(m["count"], int) else 1
                grp_ships += cnt
                fk = FACTION.get(m["faction"].strip().lower())
                sk = SHIP.get(m["ship"].strip().lower())
                if fk:
                    fac_w[fk] = fac_w.get(fk, 0.0) + ch * cnt
                if sk:
                    cls_w[sk] = cls_w.get(sk, 0.0) + ch * cnt
            maxgrp = max(maxgrp, grp_ships)
    if not fac_w or not cls_w:
        return []
    norm = max(fac_w.values())
    factions = [{"name": k, "weight": round(v / norm, 3)}
                for k, v in sorted(fac_w.items(), key=lambda kv: -kv[1])]
    normc = max(cls_w.values())
    classes = [{"name": k, "weight": round(v / normc, 3)}
               for k, v in sorted(cls_w.items(), key=lambda kv: -kv[1])]
    return [{
        "name": "gemini_traffic",
        "region": "anywhere",
        "factions": factions,
        "classes": classes,
        "max_concurrent": min(6, maxgrp),
        "spawn_interval": 12.0,
        "initial_ai_state": "patrol",
    }]


def main() -> int:
    global SCALE
    ap = argparse.ArgumentParser()
    ap.add_argument("system")
    ap.add_argument("--out", default=None, help="output stem (default: snake(name))")
    ap.add_argument("--scale", type=float, default=10.0 / 3.0)
    args = ap.parse_args()
    SCALE = args.scale

    src = load_system(args.system)
    sysname = src["name"]
    out_stem = args.out or snake(sysname)

    navs, sprites, fields = [], [], []
    centroid = [0, 0, 0]
    spawn_nav = None
    for i, n in enumerate(src["nav_points"], 1):
        pos = emap(n["x"], n["y"], n["z"])
        nav, spr, ast = nav_from(n["description"], pos, i, sysname)
        navs.append(nav)
        if spr:
            sprites.append(spr)
        if ast:
            fields.append(ast)
        for k in range(3):
            centroid[k] += pos[k]
        if not n["description"].strip():
            spawn_nav = pos     # the canonical empty 'you arrive here' nav
    n = max(1, len(navs))
    centroid = [round(c / n) for c in centroid]
    if spawn_nav is None:
        spawn_nav = navs[-1]["position"]

    info = src.get("info", {})
    out = {
        "name": sysname,
        "description": (f"{info.get('Quadrant','Gemini')} / {info.get('Sector','Gemini Sector')}. "
                        f"Jump links: {info.get('Jump Links','')}. "
                        f"Random mission opponents: {info.get('Random Mission Opponents','')}. "
                        f"(Generated from wcpedia by tools/build_system_from_wcpedia.py.)"),
        "skybox_seed": out_stem,
        "star": {"preset": "yellow"},
        "asteroid_fields": fields,
        "placed_sprites": sprites,
        "nav_points": navs,
        "encounters": build_encounters(src),
        "player_start": {"position": spawn_nav, "look_at": centroid},
    }
    path = SYS / f"{out_stem}.json"
    path.write_text(json.dumps(out, indent=2) + "\n")
    print(f"wrote {path.relative_to(REPO)}: {len(navs)} nav, {len(sprites)} sprite, "
          f"{len(fields)} field, {len(out['encounters'])} encounter rule(s)")
    print(f"  jump links -> {[n['links_to'] for n in navs if n['kind']=='jump']}")
    print(f"  bases      -> {[n['base_id'] for n in navs if n.get('dockable')]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

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
    "drayman": "drayman", "demon": "demon", "dralthi": "dralthi",
    "gothri":  "gothri",  "kamekh": "kamekh", "gladius": "gladius",
    "scout":   "scout",   "strakha": "strakha",
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
    """Return (navpoint dict, optional sprite dict, optional asteroid dict).

    Some scraped descriptions concatenate an 'Asteroid Field' modifier with
    the real nav type ('Asteroid Field Jump Point - War', 'Asteroid Field
    Lisacc Mining Base'). We strip that leading modifier to expose the core
    type AND still spawn the asteroid field, so a jump point / base that sits
    inside a belt is recognised as such (this is what was silently dropping
    the jump navs for War, Rikel, Gamma, etc.)."""
    d = desc.strip()
    back = f"{sysname} Jump"   # convention: arrival nav on the far side
    has_ast = "asteroid" in d.lower()
    core = re.sub(r"(?i)^asteroid field\s*", "", d).strip()
    clow = core.lower()
    ast = ({"center": pos, "half_extent": [40000, 12000, 40000],
            "count": 320, "base_radius": 120, "seed": 0xA57E000 + idx}
           if has_ast else None)

    if clow.startswith("jump point"):
        dest = re.sub(r"(?i)^jump point\s*[-–]\s*", "", core).strip()
        return ({"name": f"{dest} Jump", "kind": "jump", "position": pos,
                 "links_to": snake(dest), "links_to_nav": back}, None, ast)
    if "mining base" in clow or clow.endswith("base") or "station" in clow:
        bid = snake(core.split(" Mining")[0].split(" Base")[0])
        nav = {"name": core, "kind": "station", "position": pos,
               "dockable": True, "base_id": bid}
        spr = {"sprite": "sprites/mining_base", "position": pos, "length_meters": 3000}
        return (nav, spr, ast)
    if "planet" in clow:
        bid = snake(core.split(" ")[0])
        nav = {"name": core, "kind": "planet", "position": pos,
               "dockable": True, "base_id": bid}
        spr = {"sprite": "sprites/helen_planet", "position": pos, "length_meters": 2000}
        return (nav, spr, ast)
    if has_ast:
        nav = {"name": core or f"Asteroid Field {idx}", "kind": "nav", "position": pos}
        return (nav, None, ast)
    # plain / empty nav (arrival beacon, deep-space waypoint)
    return ({"name": d or f"{sysname} Nav {idx}", "kind": "nav", "position": pos},
            None, None)


def canon_groups(groups: list) -> list[dict]:
    """Canonicalize one nav point's wcnews encounter table into engine form:
    [{chance, members:[{faction, ship, count}]}], mapping faction/ship names
    to our keys (ships without a ship.json fall back via SHIP). Drops members
    whose faction/ship don't resolve and groups left empty."""
    out = []
    for g in groups:
        ch = g["chance_pct"] if isinstance(g["chance_pct"], (int, float)) else 0
        members = []
        for m in g["members"]:
            fk = FACTION.get(m["faction"].strip().lower())
            sk = SHIP.get(m["ship"].strip().lower())
            cnt = m["count"] if isinstance(m["count"], int) else 1
            if fk and sk and cnt > 0:
                members.append({"faction": fk, "ship": sk, "count": cnt})
        if ch > 0 and members:
            out.append({"chance": ch, "members": members})
    return out


def clean_name(name: str) -> str:
    """Strip wcpedia disambiguation suffixes so the id/display are clean:
    'Palan (star system)' -> 'Palan', 'Midgard (Gemini Sector)' -> 'Midgard'."""
    return re.sub(r"\s*\(.*?\)\s*", "", name).strip()


def build(src: dict, out_stem: str, skybox: str, scale: float) -> dict:
    """Build one engine StarSystem JSON from a scraped system, write it to
    assets/systems/<out_stem>.json, and return the dict (the caller uses its
    nav_points to build the galaxy jump graph)."""
    global SCALE
    SCALE = scale
    display = clean_name(src["name"])

    navs, sprites, fields = [], [], []
    centroid = [0, 0, 0]
    spawn_nav = None
    for i, n in enumerate(src["nav_points"], 1):
        pos = emap(n["x"], n["y"], n["z"])
        nav, spr, ast = nav_from(n["description"], pos, i, display)
        groups = canon_groups(src["encounters"].get(f"Nav {i}", []))
        if groups:
            nav["encounters"] = groups
        navs.append(nav)
        if spr:
            sprites.append(spr)
        if ast:
            fields.append(ast)
        for k in range(3):
            centroid[k] += pos[k]
        if not n["description"].strip():
            spawn_nav = pos
    nn = max(1, len(navs))
    centroid = [round(c / nn) for c in centroid]
    if spawn_nav is None:
        spawn_nav = navs[-1]["position"] if navs else [0, 0, 0]

    info = src.get("info", {})
    out = {
        "name": display,
        "description": (f"{info.get('Quadrant','Gemini')} / {info.get('Sector','Gemini Sector')}. "
                        f"Jump links: {info.get('Jump Links','')}. "
                        f"Random mission opponents: {info.get('Random Mission Opponents','')}. "
                        f"(Generated from wcpedia by tools/build_system_from_wcpedia.py.)"),
        "skybox_seed": skybox,
        "star": {"preset": "yellow"},
        "asteroid_fields": fields,
        "placed_sprites": sprites,
        "nav_points": navs,
        "encounters": [],   # no continuous director; per-nav tables drive spawns
        "player_start": {"position": spawn_nav, "look_at": centroid},
    }
    (SYS / f"{out_stem}.json").write_text(json.dumps(out, indent=2) + "\n")
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("system")
    ap.add_argument("--out", default=None, help="output stem (default: snake(name))")
    ap.add_argument("--scale", type=float, default=10.0 / 3.0)
    ap.add_argument("--skybox", default=None,
                    help="skybox_seed (default: the system's own id, generated on the fly)")
    args = ap.parse_args()

    src = load_system(args.system)
    out_stem = args.out or snake(clean_name(src["name"]))
    out = build(src, out_stem, args.skybox or out_stem, args.scale)
    navs = out["nav_points"]
    enc = sum(len(n.get("encounters", [])) for n in navs)
    print(f"wrote assets/systems/{out_stem}.json: {len(navs)} nav, "
          f"{len(out['placed_sprites'])} sprite, {len(out['asteroid_fields'])} field, "
          f"{enc} per-nav encounter group(s)")
    print(f"  jump links -> {[n['links_to'] for n in navs if n['kind']=='jump']}")
    print(f"  bases      -> {[n['base_id'] for n in navs if n.get('dockable')]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

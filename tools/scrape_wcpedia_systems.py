#!/usr/bin/env python3
"""scrape_wcpedia_systems.py — pull the canonical Gemini Sector data from the
Wing Commander Encyclopedia (wcnews.com/wcpedia) into one normalized JSON.

Each system page (e.g. /wcpedia/Pender%27s_Star) carries, in clean tables:
  * an infobox      — Sector, Quadrant, Jump Links, Random Mission Opponents
  * Nav Points      — name, X, Y, Z, description (jump points, asteroid fields…)
  * Stellar Objects — bases / planets / etc. at nav points
  * Random Encounters — PER NAV POINT spawn groups: chance %, count, faction,
                        ship, "intelligence" (timid/confident/fanatical + novice/pro)

We start from the "Star System Atlas" nav box at the bottom of ANY system page
(it lists every system in the sector), then fetch + parse each one. Output:
assets/data/gemini_systems.json — the source of truth a later converter maps
into our engine's StarSystem / NavPointDef / EncounterRuleDef format.

Polite: caches raw HTML under /tmp/wcpedia_cache, 0.4s between live fetches.

Usage:
  tools/scrape_wcpedia_systems.py                 # all systems
  tools/scrape_wcpedia_systems.py --only Troy Pender's_Star
  tools/scrape_wcpedia_systems.py --refresh        # ignore HTML cache
"""
from __future__ import annotations
import argparse, html, json, re, sys, time, urllib.parse, urllib.request
from pathlib import Path

BASE   = "https://www.wcnews.com"
SEED   = "/wcpedia/Pender%27s_Star"          # any system page lists all the rest
REPO   = Path(__file__).resolve().parents[1]
OUT    = REPO / "assets" / "data" / "gemini_systems.json"
CACHE  = Path("/tmp/wcpedia_cache")
UA     = "Mozilla/5.0 (new_privateer wcpedia ingest)"

# Pages in the atlas nav box that are NOT systems.
NOT_SYSTEMS = {"Gemini Sector", "Fariss Quadrant", "Clarke Quadrant",
               "Humboldt Quadrant", "Potter Quadrant"}


def fetch(path: str, refresh: bool = False) -> str:
    CACHE.mkdir(parents=True, exist_ok=True)
    key = urllib.parse.unquote(path).split("/wcpedia/")[-1]
    key = re.sub(r"[^A-Za-z0-9_.-]", "_", key)
    cf = CACHE / f"{key}.html"
    if cf.exists() and not refresh:
        return cf.read_text(encoding="utf-8", errors="replace")
    url = BASE + path
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        body = r.read().decode("utf-8", errors="replace")
    cf.write_text(body, encoding="utf-8")
    time.sleep(0.4)
    return body


def cell(c: str) -> str:
    t = re.sub(r"<[^>]+>", " ", c)
    return re.sub(r"\s+", " ", html.unescape(t)).strip()


def tables(srchtml: str) -> list[list[list[str]]]:
    out = []
    for tm in re.finditer(r"<table.*?</table>", srchtml, re.S):
        rows = []
        for r in re.findall(r"<tr[^>]*>(.*?)</tr>", tm.group(0), re.S):
            cells = [cell(c) for c in re.findall(r"<t[dh][^>]*>(.*?)</t[dh]>", r, re.S)]
            rows.append(cells)
        out.append(rows)
    return out


def system_links(srchtml: str) -> list[tuple[str, str]]:
    navbox = tables(srchtml)  # we want the LAST table's raw HTML for links
    last = re.findall(r"<table.*?</table>", srchtml, re.S)[-1]
    seen, out = set(), []
    for href, title, text in re.findall(
            r'<a href="(/wcpedia/[^"]+)"[^>]*?(?:title="([^"]*)")?>([^<]*)</a>', last):
        name = html.unescape(title or text).strip()
        if not name or name in seen or name in NOT_SYSTEMS:
            continue
        seen.add(name)
        out.append((name, html.unescape(href)))
    return out


def parse_infobox(tbls) -> dict:
    info = {}
    for rows in tbls:
        for r in rows:
            if len(r) == 2:
                k, v = r[0].rstrip(":").strip(), r[1].strip()
                if k in ("Sector", "Quadrant", "Jump Links",
                         "Random Mission Opponents"):
                    info[k] = v
    return info


def parse_navpoints(tbls) -> list[dict]:
    for rows in tbls:
        if not rows:
            continue
        hdr = [c.lower() for c in rows[0]]
        if hdr[:4] == ["point", "x", "y", "z"]:
            nav = []
            for r in rows[1:]:
                if len(r) < 5:
                    continue
                try:
                    x, y, z = int(r[1]), int(r[2]), int(r[3])
                except ValueError:
                    continue
                nav.append({"point": r[0], "x": x, "y": y, "z": z,
                            "description": r[4]})
            return nav
    return []


def parse_encounters(tbls) -> dict:
    """Return {nav_point_name: [ {chance, members:[{count,faction,ship,intel}]} ]}.
    A blank-% row continues the previous group (extra ships in the same spawn)."""
    for rows in tbls:
        if not rows:
            continue
        hdr = [c.lower() for c in rows[0]]
        if hdr[:5] == ["%", "#", "faction", "ships", "intelligence"]:
            result: dict[str, list] = {}
            cur_nav = None
            cur_group = None
            for r in rows[1:]:
                # nav header row: a single non-empty cell like "Nav 1"
                nonempty = [c for c in r if c]
                if len(nonempty) == 1 and re.match(r"(?i)nav\s*\d", nonempty[0]):
                    cur_nav = nonempty[0]
                    result.setdefault(cur_nav, [])
                    cur_group = None
                    continue
                if len(r) < 5 or cur_nav is None:
                    continue
                pct, cnt, fac, ship, intel = r[0], r[1], r[2], r[3], r[4]
                member = {"count": int(cnt) if cnt.isdigit() else cnt,
                          "faction": fac, "ship": ship, "intelligence": intel}
                if pct.strip():  # new group
                    chance = int(pct.replace("%", "").strip()) \
                        if pct.replace("%", "").strip().isdigit() else pct
                    cur_group = {"chance_pct": chance, "members": [member]}
                    result[cur_nav].append(cur_group)
                elif cur_group is not None:  # continuation
                    cur_group["members"].append(member)
            return result
    return {}


def parse_stellar(tbls) -> list[dict]:
    for rows in tbls:
        if not rows:
            continue
        hdr = [c.lower() for c in rows[0]]
        # Stellar Objects tables vary; capture name + nav + type-ish columns.
        if "object" in " ".join(hdr) or ("nav" in hdr and "type" in " ".join(hdr)):
            objs = []
            for r in rows[1:]:
                if any(r):
                    objs.append(r)
            return objs
    return []


def scrape_system(name: str, href: str, refresh: bool) -> dict:
    srchtml = fetch(href, refresh)
    tbls = tables(srchtml)
    return {
        "name": name,
        "url": BASE + href,
        "info": parse_infobox(tbls),
        "nav_points": parse_navpoints(tbls),
        "stellar_objects": parse_stellar(tbls),
        "encounters": parse_encounters(tbls),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None,
                    help="system display names to scrape (default: all)")
    ap.add_argument("--refresh", action="store_true", help="ignore HTML cache")
    args = ap.parse_args()

    seed_html = fetch(SEED, args.refresh)
    links = system_links(seed_html)
    # MediaWiki doesn't self-link the current page, so the seed system is
    # absent from its own atlas box — add it back explicitly.
    if not any(h == SEED for _, h in links):
        links.insert(0, ("Pender's Star", SEED))
    if args.only:
        want = set(args.only)
        links = [(n, h) for (n, h) in links
                 if n in want or n.replace(" ", "_") in want]
    print(f"systems to scrape: {len(links)}")

    out = []
    for i, (name, href) in enumerate(links, 1):
        try:
            data = scrape_system(name, href, args.refresh)
        except Exception as exc:                       # noqa: BLE001
            print(f"  [{i}/{len(links)}] {name}: FAILED {exc}", file=sys.stderr)
            continue
        nnav = len(data["nav_points"])
        nenc = sum(len(v) for v in data["encounters"].values())
        print(f"  [{i}/{len(links)}] {name:<28} nav={nnav} enc_groups={nenc} "
              f"jumps='{data['info'].get('Jump Links','')}'")
        out.append(data)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps({"source": BASE + SEED, "systems": out},
                              indent=2, ensure_ascii=False) + "\n")
    print(f"\nwrote {len(out)} systems -> {OUT.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

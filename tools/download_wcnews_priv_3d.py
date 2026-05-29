#!/usr/bin/env python3
"""download_wcnews_priv_3d.py — mirror original Privateer .3ds reference
models from the wcnews download archive.

Why this exists:
    The wcnews "priv1/3D" archive is a complete dump of the 1992-era
    3D Studio Max reference models used to author every ship, station,
    cockpit, and prop in Wing Commander: Privateer. Each zip contains
    a single `.3ds` mesh plus its `.bmp` texture set.

    Our current ship-art pipeline feeds AI-rendered references into
    ChatGPT-image-2 and the results have been mid at best. These OG
    models are deterministic ground truth — far better as a sprite
    source than wrestling with a generative model.

    A side bonus: our existing `orion.materials.json` (and friends)
    has a pile of 92-byte placeholder PNGs whose names are clearly
    3DSMax material slots (DJALMA, MERCHGREY, GRIME, DARKENGINE...).
    The real BMP data for those slots almost certainly lives in
    MRCHSHIP / MRCHNT / etc. here, so this download directly unblocks
    fixing the "washed-out" rendering issue too.

Legal / hygiene:
    1993 Origin Systems assets. `assets/meshes/` is gitignored, so
    nothing committed. Local research use only, not for redistribution.

Usage:
    tools/download_wcnews_priv_3d.py                 # download curated ship set
    tools/download_wcnews_priv_3d.py --list          # dry-run; print plan
    tools/download_wcnews_priv_3d.py --all           # every zip in the archive
    tools/download_wcnews_priv_3d.py --only DEMON DRALTHI
    tools/download_wcnews_priv_3d.py --no-extract    # just cache the zips

Layout produced:
    assets/meshes/ships_raw/
        _zips/                       <- raw downloaded archives (cache)
        demon/                       <- extracted contents per zip
            DEMON.3ds
            BNTARM1.bmp ...
        dralthi/
        ...
        manifest.json                <- inventory: zip -> [files], sizes
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEST_DIR = REPO / "assets" / "meshes" / "ships_raw"
ZIP_CACHE = DEST_DIR / "_zips"
MANIFEST  = DEST_DIR / "manifest.json"

ARCHIVE_BASE = "https://download.wcnews.com/files/priv1/3D"
USER_AGENT   = "biscuit-the-puppy/1.0 (+research; new_privateer)"

# Curated ship/vehicle/spacecraft set. Anything NOT in here (bars,
# hallways, missiles, weapons, characters, planet props, etc.) is
# skipped by default. Use --all to bypass. Use --only X Y Z to override.
#
# Categories below are documentation only — the script treats this as a
# flat allowlist. Names match the wcnews archive exactly (case-sensitive
# on case-sensitive file systems, but the index is also case-sensitive).
SHIP_ZIPS: set[str] = {
    # ── Player / merchant ships (codename → ship)
    # CLUNKER = Tarsus, FIGHTER = Centurion, MRCHSHIP/MRCHNT = Galaxy,
    # 37TUG = Galaxy/tug, OXSHIP = Oxford ship.
    "CLUNKER.zip", "CLUNKEXP.zip", "CLUNKPAD.zip", "CLUNKPIT.zip",
    "CLNKLAND.zip", "CLNKVIEW.zip", "CLNKGEAR.zip", "CLNKGEA2.zip",
    "LANDCLNK.zip", "LNCHCLNK.zip",
    "FIGHTER.zip", "FIGHTEXP.zip", "FIGHTGER.zip", "FIGHTLND.zip",
    "FIGHTPAD.zip", "FIGHTPIT.zip", "FIGHTVW.zip", "FIGHT-L.zip",
    "LANDFGHT.zip", "LNCHFGHT.zip",
    "MRCHSHIP.zip", "MRCHNT.zip", "MRCHGEAR.zip", "MRCHLAND.zip",
    "MRCHVIEW.zip", "MERCH-L.zip", "MERCH-TO.zip",
    "MERCHPAD.zip", "MERCHPIT.zip",
    "LANDMRCH.zip", "LNCHMRCH.zip",
    "37TUG.zip", "TUG-TO.zip", "TUGGEAR.zip", "TUGLAND.zip",
    "TUGPAD.zip", "TUGPIT.zip", "TUGPIT2.zip", "TUGSID.zip",
    "LANDTUG.zip", "LNCHTUG.zip", "VIEW37.zip",
    "OXSHIP.zip",
    "LANDSTAR.zip", "LNCHSTAR.zip",   # starport pad placeholders

    # ── Confed / Militia / pirate / Kilrathi / Retro / Steltek fighters
    "BRDSWRDX.zip", "BROADVW.zip", "BRDDEB.zip",          # Broadsword
    "DEMON.zip",   "DEMONVW.zip",                          # Demon
    "DRALTHI.zip", "DRALVW.zip",   "DRLTHVW.zip",          # Dralthi
    "GLADIUS.zip", "GLADVIEW.zip",                         # Gladius
    "GOTHRIX.zip", "GOTHVIEW.zip", "GOTHDEB.zip",          # Gothri
    "KAMEKH.zip",  "KAMEKVW.zip",                          # Kamekh corvette
    "PARADIGM.zip", "Paradigm.max.zip",                    # Paradigm destroyer
    "STILETVW.zip",                                        # Stiletto view
    "STRAKHA.zip", "STRAKVW.zip", "STRAKDEB.zip",          # Strakha stealth
    "TALON5.zip",  "TALMIL.zip",   "TALMIVW.zip",
    "TALP37.zip",  "TALPRVW.zip",  "TALRELVW.zip",
    "TALZEAL.zip", "TALDEB.zip",                           # Talon variants
    "Dray01.zip",                                          # Drayman
    "ENFORCER.zip",                                        # Confed Enforcer
    "STELFGHT.zip", "STELFT1.zip", "STELFT2.zip",
    "STELFT3.zip",  "STELFTX.zip", "STELFTY.zip",
    "STELTEK.zip",                                         # Steltek family
    "SCOUT.zip", "SCOUTVIE.zip",                           # Scout
    "SHUTTLE2.zip", "TUNSHUTL.zip",                        # Shuttles
    "FRIGBOX.zip", "FRIGVIEW.zip",                         # Frigate
    "PERSHIP1.zip", "PERSHIP2.zip", "PERSHIP4.zip",        # Perry ships
    "PERYCNPC.zip",
    "TRANSPRT.zip", "TRANSVW.zip",                         # Transport
    "DERLCT.zip",                                          # Derelict
    "JAKE.zip", "JAKE01.zip",                              # Jake (NPC ship)
    "DRONE.zip", "DRONE09.zip", "DRONVIEW.zip",            # Drones

    # ── Surface vehicles / movers (still ship-shaped enough to be useful)
    "AIRCAR.zip", "NDAIRCAR.zip", "ND_AIRCA.zip",
    "ADMIRAL.zip", "MOTOSYCL.zip", "CART.zip", "TRUCK.zip",
    "TRAILER.zip", "TRAILVW.zip", "LOADER.zip", "TRACTOR.zip",
}


# ─────────────────────────── HTTP / archive helpers ──────────────────────


def _http_get(url: str, timeout: float = 30.0) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def discover_remote_zips() -> list[str]:
    """Scrape the nginx-style index page and return every .zip listed."""
    html = _http_get(f"{ARCHIVE_BASE}/").decode("utf-8", "replace")
    names = re.findall(r'href="([^"]+\.zip)"', html)
    # Stable order — original archive listing is already alpha-ish but
    # nginx isn't strict about it across regenerations.
    return sorted(set(names))


def fetch_zip(name: str, delay_s: float) -> tuple[bool, str]:
    dest = ZIP_CACHE / name
    if dest.exists() and dest.stat().st_size > 0:
        return True, "cached"
    dest.parent.mkdir(parents=True, exist_ok=True)
    url = f"{ARCHIVE_BASE}/{name}"
    try:
        data = _http_get(url)
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as exc:
        return False, f"{type(exc).__name__}: {exc}"
    # Quick sanity: every real zip starts with 'PK\x03\x04'.
    if not data.startswith(b"PK\x03\x04"):
        return False, f"not a zip ({len(data)} bytes; starts {data[:4]!r})"
    dest.write_bytes(data)
    if delay_s > 0:
        time.sleep(delay_s)
    return True, f"{len(data) // 1024} KiB"


def extract_zip(name: str) -> tuple[bool, str, list[str]]:
    """Extract `name` from ZIP_CACHE into DEST_DIR/<stem_lower>/.

    Returns (ok, msg, member_names). Skips extraction if the target dir
    already has files (idempotent re-runs are cheap).
    """
    src = ZIP_CACHE / name
    stem = Path(name).stem.lower().replace(".", "_")
    out  = DEST_DIR / stem
    try:
        with zipfile.ZipFile(src) as zf:
            members = [m for m in zf.namelist() if not m.endswith("/")]
            if out.exists() and any(out.iterdir()):
                return True, "already extracted", members
            out.mkdir(parents=True, exist_ok=True)
            # Flatten any nested paths inside the archive so a single ship
            # always lives in a single flat directory — these archives
            # rarely contain subfolders but Be Defensive.
            for member in members:
                flat = Path(member).name
                if not flat:
                    continue
                (out / flat).write_bytes(zf.read(member))
            return True, f"{len(members)} files", members
    except (zipfile.BadZipFile, OSError) as exc:
        return False, f"{type(exc).__name__}: {exc}", []


# ─────────────────────────── orchestration ───────────────────────────────


def select_targets(remote: list[str], args: argparse.Namespace) -> list[str]:
    remote_set = set(remote)
    if args.only:
        # Allow either with or without .zip suffix; normalize.
        requested = {n if n.endswith(".zip") else f"{n}.zip" for n in args.only}
        missing = requested - remote_set
        if missing:
            print(f"warning: {len(missing)} requested zip(s) not on server: "
                  f"{sorted(missing)}", file=sys.stderr)
        return sorted(requested & remote_set)
    if args.all:
        return remote
    # Default: curated ship set, intersected with what actually exists.
    missing = SHIP_ZIPS - remote_set
    if missing:
        print(f"note: {len(missing)} curated ship zip(s) absent from server: "
              f"{sorted(missing)}", file=sys.stderr)
    return sorted(SHIP_ZIPS & remote_set)


def write_manifest(records: list[dict]) -> None:
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps({
        "source": f"{ARCHIVE_BASE}/",
        "count":  len(records),
        "entries": records,
    }, indent=2) + "\n")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--all",        action="store_true",
                   help="download every zip in the archive (~321)")
    p.add_argument("--only",       nargs="*", default=None,
                   help="explicit zip allowlist (with or without .zip)")
    p.add_argument("--list",       dest="list_only", action="store_true",
                   help="print planned downloads and exit")
    p.add_argument("--no-extract", action="store_true",
                   help="cache the .zip files but skip unpacking")
    p.add_argument("--delay",      type=float, default=0.1,
                   help="seconds between non-cached HTTP fetches (be polite)")
    args = p.parse_args()

    print(f"discovering remote archive at {ARCHIVE_BASE}/ ...")
    try:
        remote = discover_remote_zips()
    except (urllib.error.URLError, TimeoutError) as exc:
        print(f"could not reach archive: {exc}", file=sys.stderr)
        return 2
    print(f"server lists {len(remote)} zips total")

    targets = select_targets(remote, args)
    print(f"planning to fetch {len(targets)} zip(s)")
    if args.list_only:
        for name in targets:
            print(f"  PLAN  {ARCHIVE_BASE}/{name}")
        return 0
    if not targets:
        print("nothing to do.")
        return 0

    failures: list[tuple[str, str]] = []
    records:  list[dict] = []
    for i, name in enumerate(targets, 1):
        ok, msg = fetch_zip(name, args.delay)
        mark = "ok  " if ok else "FAIL"
        print(f"  [{i:3d}/{len(targets)}] {mark} dl  {name:<22} ({msg})")
        if not ok:
            failures.append((name, msg))
            continue
        if args.no_extract:
            continue
        ok, msg, members = extract_zip(name)
        mark = "ok  " if ok else "FAIL"
        print(f"           {mark} ext {name:<22} ({msg})")
        if not ok:
            failures.append((name, msg))
            continue
        records.append({
            "zip":     name,
            "out_dir": str((DEST_DIR / Path(name).stem.lower().replace(".", "_"))
                            .relative_to(REPO)),
            "members": sorted(members),
        })

    if not args.no_extract:
        write_manifest(records)
        print(f"\nwrote manifest: {MANIFEST.relative_to(REPO)}  "
              f"({len(records)} entries)")

    if failures:
        print(f"\n=== {len(failures)} failure(s) ===", file=sys.stderr)
        for name, msg in failures:
            print(f"  {name}: {msg}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

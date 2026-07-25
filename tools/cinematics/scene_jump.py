#!/usr/bin/env python3
"""Jump the running game straight to any bar-fixer scene.

Reviewing 37 conversations by playing the campaign is not review, it's a
playthrough. This drives dev_remote to put the player exactly where a given
scene fires: it sets the plot flags that scene REQUIRES, clears the ones that
BLOCK it, warps to the right system, docks at the right base, and opens the
Bar. Then you click "Talk to ..." and watch.

Scene order matches docs/fixer_script.md (narrative order, not JSON order).

Usage::

    tools/cinematics/scene_jump.py --list
    tools/cinematics/scene_jump.py 1              # by number
    tools/cinematics/scene_jump.py sandoval       # by id substring
    tools/cinematics/scene_jump.py --next         # advance one scene
    tools/cinematics/scene_jump.py --reset        # clear all plot flags

Requires the game to be running (dev_remote on 127.0.0.1:47001).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

DEV = "http://127.0.0.1:47001"
REPO = Path(__file__).resolve().parents[2]
STATE = Path("/tmp/np_scene_cursor")

# archetype -> candidate bases, for entries placed by archetype rather than by
# a specific base id.
ARCHETYPE_BASES = {
    "mining": ["hector", "romulus", "rygannon"],
}

# base id -> system to /goto before docking.
BASE_SYSTEM = {
    "hector":             "troy",
    "romulus":            "castor",
    "new_detroit":        "new_detroit",
    "oakham":             "pentonville",
    "new_constantinople": "new_constantinople",
    "oxford":             "oxford",
    "basra_refinery":     "palan",
    "palan":              "palan",
    "rygannon":           "rygannon",
    "perry_naval":        "perry",
}

# Narrative order (mirrors tools/cinematics/export_script.py).
ORDER = [
    "sandoval_offer", "tayla_artifact_handoff",
    "tayla_m02_offer",
    "tayla_m03_offer", "tayla_m03_debrief",
    "tayla_m04_offer", "tayla_m04_debrief",
    "tayla_m05_offer", "tayla_m05_debrief",
    "lynch_m06_offer", "lynch_m06_debrief",
    "lynch_m07_offer", "lynch_m08_offer", "lynch_m09_offer",
    "masterson_m10_offer", "masterson_m11_offer",
    "masterson_m12_offer", "masterson_m13_offer",
    "oxford_library_scene",
    "murphy_m14_offer", "murphy_m14_debrief",
    "murphy_m15_offer", "murphy_m15_debrief",
    "murphy_m16_offer",
    "monkhouse_m17_offer", "monkhouse_m17_debrief",
    "cross_m18_offer", "cross_m18_debrief",
    "cross_m19_offer", "cross_m19_debrief",
    "cross_m20_offer", "cross_m20_debrief",
    "cross_m21_offer",
    "goodin_offer",
    "terrell_offer", "terrell_debrief", "terrell_epilogue",
]

# Flags the campaign sets that no fixer sets itself (mission completions).
# scene_jump has to fake these to reach a debrief without flying the mission.


def post(path: str, body: dict | None = None, timeout: float = 6.0):
    data = json.dumps(body).encode() if body is not None else b""
    req = urllib.request.Request(DEV + path, data=data, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as fh:
            return json.loads(fh.read() or b"{}")
    except Exception as exc:
        return {"error": str(exc)}


def get(path: str, timeout: float = 6.0):
    try:
        with urllib.request.urlopen(DEV + path, timeout=timeout) as fh:
            return json.loads(fh.read() or b"{}")
    except Exception as exc:
        return {"error": str(exc)}


def load_fixers() -> dict:
    doc = json.loads((REPO / "assets" / "data" / "fixers.json").read_text())
    return {e["id"]: e for e in doc["fixers"]}


def all_flags(fixers: dict) -> set[str]:
    """Every flag mentioned anywhere -- the set we clear on reset."""
    flags: set[str] = set()
    for e in fixers.values():
        flags |= set(e.get("requires_flags", []))
        flags |= set(e.get("forbids_flags", []))
        for key in ("accept_actions", "refuse_actions", "done_actions"):
            for a in e.get(key, []):
                if a.startswith(("set_flag:", "clear_flag:")):
                    flags.add(a.split(":", 1)[1])
    return flags


def reset(fixers: dict) -> None:
    for flag in sorted(all_flags(fixers)):
        post("/plot", {"action": "clear_flag", "id": flag})
    print(f"[scene] cleared {len(all_flags(fixers))} plot flags")


def jump(entry: dict, fixers: dict) -> None:
    fid = entry["id"]
    base = entry.get("base")
    if not base:
        # Archetype-placed (Goodin appears at ANY mining base). Pick one that
        # matches the archetype and is not in exclude_bases -- rygannon is the
        # obvious mining base but she explicitly excludes it, so a naive
        # fallback lands somewhere she will never appear.
        arch = entry.get("archetypes", [])
        excluded = set(entry.get("exclude_bases", []))
        for cand in ARCHETYPE_BASES.get(arch[0] if arch else "", []):
            if cand not in excluded:
                base = cand
                break
        if not base:
            print(f"[scene] no unexcluded base for archetype {arch}")
            return
        print(f"[scene] {fid} is archetype-placed ({', '.join(arch)}) -> {base}")

    system = BASE_SYSTEM.get(base)
    if not system:
        print(f"[scene] don't know which system hosts {base!r}")
        return

    # 1. Clear everything, then set exactly this scene's preconditions. Doing a
    #    full reset first means jumping BACKWARDS works -- otherwise a flag set
    #    by a later scene keeps blocking an earlier one.
    reset(fixers)
    for flag in entry.get("requires_flags", []):
        post("/plot", {"action": "set_flag", "id": flag})
    for flag in entry.get("forbids_flags", []):
        post("/plot", {"action": "clear_flag", "id": flag})

    # 2. Some scenes also want a plot ITEM to make sense in the fiction
    #    (Grayson should be holding the artifact from scene 2 onward).
    if ORDER.index(fid) >= 1:
        post("/plot", {"action": "give_item", "id": "steltek_artifact"})

    # 2b. Debriefs gate on flags the CAMPAIGN sets when a mission is actually
    #     flown (m04_delivered, killed:garrovick, ...). Nothing in fixers.json
    #     sets those, so a review jump has to fake them -- otherwise every
    #     debrief scene is unreachable without playing the mission.
    for flag in entry.get("requires_flags", []):
        post("/plot", {"action": "set_flag", "id": flag})

    # 3. Fly there. /goto is refused unless we're in Flight, so if a previous
    #    jump left us docked we have to launch first -- otherwise every jump
    #    after the first silently stays put at the old base.
    if get("/base").get("base_id"):
        post("/base/screen", {"name": "Launch"})
        time.sleep(4)

    print(f"[scene] {fid}: -> {system} / {base}")
    post("/goto", {"system": system})
    time.sleep(7)
    post("/dock", {"base": base})
    time.sleep(5)
    post("/base/screen", {"name": "Bar"})
    time.sleep(1.5)

    st = get("/base")
    present = st.get("fixers", [])
    where = st.get("base_id", "?")
    if fid in present:
        print(f"[scene] OK: at {where}, '{fid}' is present -- click Talk to ...")
    else:
        print(f"[scene] at {where}, present={present}")
        print(f"[scene] WARNING: {fid} did not pass its gate. Requires="
              f"{entry.get('requires_flags')} forbids={entry.get('forbids_flags')}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("scene", nargs="?", help="scene number or id substring")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--next", action="store_true")
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args(argv)

    fixers = load_fixers()

    if args.list:
        for i, fid in enumerate(ORDER, 1):
            e = fixers.get(fid, {})
            n = len(e.get("dialogue", []))
            print(f"  {i:>2}  {fid:34} {e.get('base','?'):20} {n:>3} lines")
        return 0

    if get("/base").get("error"):
        print("[scene] game not running (dev_remote 127.0.0.1:47001 unreachable)")
        return 1

    if args.reset:
        reset(fixers)
        STATE.write_text("0")
        return 0

    if args.next:
        cur = int(STATE.read_text()) if STATE.exists() else 0
        idx = min(cur, len(ORDER) - 1)
        STATE.write_text(str(idx + 1))
    elif args.scene and args.scene.isdigit():
        idx = int(args.scene) - 1
        STATE.write_text(str(idx + 1))
    elif args.scene:
        matches = [i for i, f in enumerate(ORDER) if args.scene in f]
        if not matches:
            print(f"[scene] no scene matching {args.scene!r}")
            return 1
        idx = matches[0]
        STATE.write_text(str(idx + 1))
    else:
        print("[scene] give a number/id, or --next / --list / --reset")
        return 1

    if not 0 <= idx < len(ORDER):
        print(f"[scene] out of range (1..{len(ORDER)})")
        return 1

    fid = ORDER[idx]
    print(f"\n=== SCENE {idx + 1}/{len(ORDER)}: {fid} ===")
    jump(fixers[fid], fixers)
    return 0


if __name__ == "__main__":
    sys.exit(main())

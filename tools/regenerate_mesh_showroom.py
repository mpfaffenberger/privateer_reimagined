#!/usr/bin/env python3
"""regenerate_mesh_showroom.py — emit a fresh assets/systems/mesh_showroom.json
that loads every .obj under assets/meshes/ships_wcnews/.

Why generate instead of hand-edit:
    We have ~108 meshes converted from the wcnews fan archive and the
    set will keep growing as we download more zips. Hand-maintaining a
    JSON with 108 placed_meshes + 108 nav_points is a recipe for drift
    (mesh added → forgot to add nav → fly there → nothing). One script,
    one source of truth, idempotent re-run. DRY.

Layout:
    Alpha-sorted ships → row-major grid (cols × rows). Default 12 × 9 =
    108 slots — fits today's set with zero waste. If the set grows past
    that the grid auto-expands (rows increase, cols held constant) so
    the showroom stays rectangular.

Geometry choices:
    All ships use the same `length_meters` so the grid stays uniform
    and visual comparison is fair — relative real-world sizes can be
    re-introduced later by a per-codename override table. All ships
    use `euler_deg=[0,0,180]` (roll 180°) matching every existing
    wcnews scene; the few meshes with their own native frame can be
    fixed up in the per-ship override block below.

Output:
    assets/systems/mesh_showroom.json (overwrite). Re-run after every
    `tools/import_3ds_meshes.py` pass and the showroom catches up.

Usage:
    tools/regenerate_mesh_showroom.py                # default grid
    tools/regenerate_mesh_showroom.py --cols 10      # custom width
    tools/regenerate_mesh_showroom.py --length 200   # bigger ships
    tools/regenerate_mesh_showroom.py --spacing 500  # wider gaps
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

REPO    = Path(__file__).resolve().parents[1]
SRC_DIR = REPO / "assets" / "meshes" / "ships_wcnews"
OUT_PATH = REPO / "assets" / "systems" / "mesh_showroom.json"
ASSET_PREFIX = "meshes/ships_wcnews/"   # what goes in the JSON, relative to assets/

# Per-codename overrides for meshes whose native frame / colour balance
# doesn't match the global default. Each value is a dict of any subset
# of {euler_deg, length_meters, ambient_floor, spec, tint, obj}. Anything
# missing falls back to the global defaults applied in build_mesh_entry.
#
# Keep this short — only add a row when eyeballing the showroom proves a
# ship needs it.
PER_SHIP_OVERRIDES: dict[str, dict] = {
    # The Orion's stock material palette is a row of mid-dark greys
    # (MERCHGREY, DKER_3, DARKENGINE, BURNT_IRON, BLACK) that look
    # crushed under the engine's half-Lambert wrap. A 1.4x body tint
    # restores it to roughly the brightness of the canonical Origin
    # reference render without touching the source tint PNGs (which
    # we'd want to keep accurate for the eventual sprite pipeline).
    #
    # Orientation overrides for the few wcnews meshes whose native frame
    # doesn't match the bulk default. See build_mesh_entry for the full
    # derivation of the global default ([90, 0, 180]).
    #
    # Centurion and Orion ship nose-DOWN (-Y) in their source files
    # rather than the usual nose-UP (+Y) — so they only need the pitch
    # half of the fix, no roll-flip. [90, 0, 0] alone maps -Y to -Z
    # forward, and the -Z native top still lands at world +Y up.
    "fighter":  {"euler_deg": [90, 0, 0]},
    # Orion + Gladius: their hull paint material reads as a muddy medium-
    # grey under the default spec=0.5 because spec is gated by the gray
    # 1×1 fallback texture (no spec map exists in the source). Bumping
    # spec to 0.85 cranks up the Blinn-Phong highlight so the hull reads
    # as polished metal under the studio sun, not flat painted plastic.
    "dd_tug":   {"euler_deg": [90, 0, 0], "tint": [1.4, 1.4, 1.4], "spec": 0.85},
    "gladius":  {"spec": 0.85},
    # Paradigm: source mesh was authored at a peculiar angle in 3DS Max —
    # nose ~45° off +Y, long axis pitched a few degrees below horizontal,
    # AND a small in-plane yaw. Trying to derive this from first principles
    # cost an hour of iteration; final values came from Mike dialling the
    # F5 in-engine slider until the silhouette matched the canonical
    # Origin reference. Iteration log (for the curious):
    #   [90, 0, -135] → wrong yaw direction
    #   [90, 0,  135] → nose slightly down
    #   [95, 0,  135] → still slightly down
    #   [100, 0, 135] → almost there
    #   [103, 26.5, 175.8] → eyeballed perfect via F5 mesh_orient_editor
    "paradigm": {"euler_deg": [103.0, 26.5, 175.8]},
}

# Final-cut whitelist with display-name renames. Map from on-disk .obj
# stem (under ships_wcnews/) → friendly label shown on the in-game nav
# overlay. When this dict is non-empty it OVERRIDES the variant filter
# below — only the listed ships get placed in the showroom.
#
# Set to {} to fall back to the broader variant-stripped catalogue.
KEEP_AND_RENAME: dict[str, str] = {
    # Mike's curated keep-list (the recognisable Privateer ships):
    "gladius":   "gladius",
    "mrchship":  "galaxy",
    "demon":     "demon",
    "clunker":   "tarsus",
    "brdswrdx":  "broadsword",
    "fighter":   "centurion",
    "transprt":  "drayman",
    "scout":     "scout",
    "paradigm":  "paradigm",
    "kamekh":    "kamekh",
    "gothrix":   "gothri",
    "dralthi":   "dralthi",
    "derlct":    "derelict",
    "talmil":    "talon",
    "strakha":   "strakha",
    "drone":     "drone",
    "stiletto":  "stiletto",   # Confed light fighter, flame geometry stripped (np: Troy Confed wing)
    # The Orion is the wcnews DD_TUG.zip mesh — a heavy mercenary
    # gunship with 4 cylindrical engines on outriggers. Confirmed by
    # eyeball against assets/ships/orion/canonical_reference.png.
    # (Earlier candidates mrchnt and 37tug were a debug box and a
    # cone-with-afterburners respectively — both rejected.)
    "dd_tug":    "orion",
    # FRIGVIEW.zip was a contender for "better paradigm" because it has
    # 94k tris vs paradigm's 29k — but it turns out to be a box-shaped
    # docking-bay prop, not a ship. The actual high-fidelity Paradigm
    # model lives in Paradigm.max.zip but that's a native 3DS Max scene
    # our pure-stdlib importer can't parse.
}

# Variant-filter rules. Most ships in the wcnews archive ship with a half-
# dozen related .3ds files: a base hull plus stand-ins for the cockpit
# interior (`*pit`), landing pad (`*pad`), in-flight view (`*vw` / `*view`),
# landing-gear-deployed (`*gear`), debris (`*deb`), explosion (`*exp`), the
# landed pose (`land*`), and the engines-firing-takeoff pose (`lnch*`,
# which is what Mike is referring to when he says "afterburner trails").
# For an at-a-glance ship lineup we want exactly ONE entry per real hull,
# so we strip the variants by name pattern.
#
# These suffix matches are deliberately conservative: every entry here was
# eyeballed against the actual wcnews fan-archive name list. If a future
# import surfaces a new variant kind, add it to the appropriate set.
VARIANT_SUFFIXES = (
    "vw", "view", "vie",        # alternate render / orbit-cam views
    "pit", "pit2",              # cockpit interior shots
    "pad",                       # parked on a landing pad
    "land", "lnd",              # landed pose (full + short forms)
    "gear", "gea2", "ger",      # landing-gear-deployed (full + short)
    "deb",                       # debris / wreckage
    "exp",                       # explosion frames
    "sid",                       # side-view diorama
    "-l", "-to", "-tu",         # short-form variant tags in the archive
)
VARIANT_PREFIXES = (
    "land",                      # landed-on-pad variants  (landclnk, landfght, ...)
    "lnch",                      # launching / afterburner (lnchclnk, lnchfght, ...)
)
# Specific stems to drop wholesale — duplicates / numbered variants /
# faction-recolours of an already-kept base hull.
VARIANT_STEMS = {
    # Talon: keep `talmil` (militia, the canonical Pirate Talon shape).
    "talon5", "talp37", "talzeal",
    # Numbered NPC / loadout duplicates
    "jake01", "drone09", "pership2", "pership4", "perycnpc",
    # Steltek scout variants — keep `stelfght` (base fighter) and
    # `steltek` (the canonical alien hull).
    "stelft1", "stelft2", "stelft3", "stelftx", "stelfty",
    # New Detroit aircar variants — keep base `aircar`.
    "nd_airca", "ndaircar",
    # Misc one-offs that are clearly view-like even though they don't
    # match the suffix rules.
    "view37", "tunshutl",
}


def is_variant(stem: str) -> bool:
    """True if `stem` looks like a derivative of some base ship and should
    be filtered out of the at-a-glance showroom."""
    if stem in VARIANT_STEMS:
        return True
    for pre in VARIANT_PREFIXES:
        if stem.startswith(pre):
            return True
    for suf in VARIANT_SUFFIXES:
        if stem.endswith(suf):
            return True
    return False


def discover_meshes(include_variants: bool) -> list[str]:
    """Return alpha-sorted stems of every .obj in ships_wcnews/.

    Priority:
      1. If KEEP_AND_RENAME is populated, return ONLY those stems (and
         warn about any that are missing on disk). The whitelist wins
         over every other filter.
      2. Else if `include_variants` is True, return everything.
      3. Else apply the variant filter (drop *vw, *pit, land*, lnch*, ...).
    """
    if not SRC_DIR.exists():
        return []
    stems = sorted(p.stem for p in SRC_DIR.iterdir()
                   if p.suffix.lower() == ".obj")
    if KEEP_AND_RENAME:
        present = [s for s in stems if s in KEEP_AND_RENAME]
        missing = sorted(set(KEEP_AND_RENAME) - set(stems))
        if missing:
            print(f"warning: KEEP_AND_RENAME refers to {len(missing)} stem(s) "
                  f"not in ships_wcnews/: {missing}", file=sys.stderr)
        # Sort by DISPLAY name so the showroom grid orders alphabetically
        # by the label the user actually sees on screen, not the cryptic
        # disk codename.
        return sorted(present, key=lambda s: KEEP_AND_RENAME[s])
    if include_variants:
        return stems
    return [s for s in stems if not is_variant(s)]


def display_name(stem: str) -> str:
    """Friendly label for a stem. Falls back to the stem itself when
    KEEP_AND_RENAME is empty / doesn't mention this ship."""
    return KEEP_AND_RENAME.get(stem, stem)


def build_layout(stems: list[str], cols: int, spacing: float
                 ) -> list[tuple[str, float, float]]:
    """Row-major grid centered on the origin. Returns [(stem, x, z), ...].

    Row index increases as Z decreases (so the first row sits at the
    largest +Z = nearest to the default spawn camera).
    """
    rows = math.ceil(len(stems) / cols)
    # Centre the grid: the *centre* of each cell sits at integer-spaced
    # coordinates. For an N-cell row that means x ∈ {(i - (N-1)/2) * spacing}.
    placed = []
    for idx, stem in enumerate(stems):
        r = idx // cols
        c = idx %  cols
        x =  (c - (cols - 1) / 2.0) * spacing
        z =  ((rows - 1) / 2.0 - r) * spacing
        placed.append((stem, x, z))
    return placed


def build_mesh_entry(stem: str, x: float, z: float,
                     default_length: float) -> dict:
    overrides = PER_SHIP_OVERRIDES.get(stem, {})
    # Default orientation = [pitch=90, yaw=0, roll=180].
    #
    # Why this specific combo: the wcnews .3ds files are 3DS Max scenes
    # authored with +Y forward (nose) and -Z up (top of the hull). That
    # "top at -Z" choice is non-standard for Max (usually +Z is up) but
    # is what the eyeball test against the showroom proves. Loading at
    # [0,0,0] makes ships stand on their tail (nose at world +Y); a
    # plain Rx(-90) puts the nose forward but lands the deck at world -Y
    # (upside down). The full fix is Rx(90) * Rz(180):
    #
    #   * Rz(180) flips X and Y inside the model frame, so the nose at
    #     model +Y gets pre-rotated to model -Y.
    #   * Rx(90) then maps model -Y to world -Z (forward) AND maps the
    #     model's true top (native -Z) to world +Y (up).
    #
    # A few meshes (centurion, orion) ship nose-DOWN (-Y) instead of
    # nose-UP, so their nose was already at -Y and they only need the
    # pitch half ([90, 0, 0]). Paradigm has an extra 45° model-frame
    # spin baked in — see its override. All four ship-specific cases
    # live in PER_SHIP_OVERRIDES above.
    return {
        "obj":           overrides.get("obj", f"{ASSET_PREFIX}{stem}.obj"),
        "position":      [round(x, 2), 0, round(z, 2)],
        "euler_deg":     overrides.get("euler_deg", [90, 0, 180]),
        "length_meters": overrides.get("length_meters", default_length),
        "ambient_floor": overrides.get("ambient_floor", 0.4),
        "spec":          overrides.get("spec", 0.5),
        "tint":          overrides.get("tint", [1.0, 1.0, 1.0]),
        "double_sided":  True,
    }


def build_nav_entry(stem: str, x: float, z: float) -> dict:
    return {"name": display_name(stem), "kind": "nav",
            "position": [round(x, 2), 0, round(z, 2)]}


def build_showroom(stems: list[str], cols: int, spacing: float,
                   length: float) -> dict:
    placed = build_layout(stems, cols, spacing)

    # Camera framing — sit far enough back + high enough that the whole
    # grid fits in frame on spawn. Derived from grid extents so it auto-
    # scales when the set grows.
    rows = math.ceil(len(stems) / cols)
    grid_w = (cols - 1) * spacing
    grid_d = (rows - 1) * spacing
    spawn_y = grid_d * 0.4 + 200      # bird's-eye-ish
    spawn_z = grid_d * 0.6 + 800      # parked behind the front row
    look_z  = -grid_d * 0.15          # tilt slightly into the grid

    return {
        "name":        "Mesh Showroom (wcnews)",
        "description": (
            f"Auto-generated by tools/regenerate_mesh_showroom.py. "
            f"{len(stems)} meshes laid out {cols}x{rows} at {spacing}m spacing, "
            f"length_meters={length}, euler_deg=[0,0,180]. Edit "
            f"PER_SHIP_OVERRIDES in the generator to tweak individual ships."),
        "skybox_seed":    "troy",
        "star":           {"preset": "yellow"},
        "studio_lighting": True,
        "asteroid_fields":     [],
        "placed_sprites":      [],
        "placed_ship_sprites": [],
        "placed_meshes": [build_mesh_entry(stem, x, z, length)
                          for stem, x, z in placed],
        "nav_points": [
            {"name": "Showroom Center", "kind": "nav",
             "position": [0, round(spawn_y * 0.7, 2), 0]},
            *[build_nav_entry(stem, x, z) for stem, x, z in placed],
        ],
        "player_start": {
            "position": [0, round(spawn_y, 2), round(spawn_z, 2)],
            "look_at":  [0, 0, round(look_z, 2)],
        },
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--cols",    type=int,   default=12,
                   help="ships per row in the grid (default: 12)")
    p.add_argument("--spacing", type=float, default=400.0,
                   help="distance between adjacent ships in metres (default: 400)")
    p.add_argument("--length",  type=float, default=150.0,
                   help="length_meters applied to every ship (default: 150)")
    p.add_argument("--include-variants", action="store_true",
                   help="include cockpit/landing/launching/view/debris/etc. variants")
    args = p.parse_args()

    stems = discover_meshes(include_variants=args.include_variants)
    if not stems:
        print(f"no meshes found under {SRC_DIR.relative_to(REPO)}; did you run "
              f"tools/import_3ds_meshes.py?", file=sys.stderr)
        return 1

    scene = build_showroom(stems, args.cols, args.spacing, args.length)
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    # ensure_ascii=False keeps any non-ASCII characters in the description
    # as raw UTF-8 bytes; the engine's JSON parser (src/json.cpp) accepts
    # UTF-8 in strings but does NOT decode \uXXXX escapes, so the default
    # ensure_ascii=True would emit unparseable `\u00d7` for `×` etc.
    OUT_PATH.write_text(json.dumps(scene, indent=2, ensure_ascii=False) + "\n")

    rows = math.ceil(len(stems) / args.cols)
    print(f"wrote {OUT_PATH.relative_to(REPO)}")
    print(f"  {len(stems)} meshes  {args.cols}×{rows}  spacing={args.spacing}m  "
          f"length={args.length}m")
    print(f"  grid extent: {(args.cols-1)*args.spacing:.0f}m × "
          f"{(rows-1)*args.spacing:.0f}m")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""extract_ship_lights.py — derive 3D nav-light positions for a ship from
its mesh material zones, using the original artists' material names.

The wcnews .3ds files label the special hull parts by material name:
    * RADARDISH                       → scanner dish  (blinking green)
    * *ENGINE* (CLUNKENGINE, ...)     → engine nacelle (blue exhaust at the
                                        outboard/rear end, red intake at the
                                        inboard/front end)
    * *GLOW* engine glows             → exhaust flare  (blue)

This tool reads the converted <ship>.obj + <ship>.materials.json, groups
vertices by material name, classifies each material into a light ROLE,
clusters each role's verts into spatially separate fixtures (4 engine
nacelles sharing one material → 4 separate lights), and writes the
fixtures as 3D points in <ship>.lights3d.json. A later pass (in
render_3d_sprite_atlases.py) projects these into every sprite cell.

Output schema (assets/meshes/ships_wcnews/<ship>.lights3d.json):
    {
      "ship": "clunker",
      "lights": [
        {"pos": [x,y,z], "normal": [nx,ny,nz], "color": [r,g,b],
         "kind": "steady"|"strobe", "hz": 0.0, "role": "exhaust"},
        ...
      ]
    }
Positions are in the SAME local space as the .obj (post-recenter), so
the atlas renderer can hand them straight to the engine's projection.

Usage:
    tools/extract_ship_lights.py clunker
    tools/extract_ship_lights.py --all
    tools/extract_ship_lights.py clunker --threshold 0.05
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

REPO    = Path(__file__).resolve().parents[1]
WCNEWS  = REPO / "assets" / "meshes" / "ships_wcnews"

# Tuning: engine materials smear across whole nacelles, so cap how many
# engine clusters we keep and ignore tiny panel specks. These defaults
# give ~2-4 engine fixtures on a fighter; bump for capital ships.
MAX_ENGINE_CLUSTERS = 4
MIN_CLUSTER_VERTS   = 12
ROLE_CAPS = {"radar": 4, "engine": MAX_ENGINE_CLUSTERS, "exhaust": 6}

# Light presets per role. Colours are 0..255 to match the .lights.json the
# editor writes. Exhaust = cool blue, intake = warm red, radar = green strobe.
ROLE_PRESETS = {
    "exhaust": {"color": [51, 140, 255], "kind": "steady", "hz": 0.0},
    "intake":  {"color": [255, 40, 40],  "kind": "steady", "hz": 0.0},
    "radar":   {"color": [26, 217, 64],  "kind": "strobe", "hz": 3.0},
}


def classify_material(name: str) -> str | None:
    """Map a material name to a feature role, or None if it's just hull."""
    n = name.upper()
    if any(k in n for k in ("RADAR", "DISH", "ANTEN", "SCANNER")):
        return "radar"
    # Glowing engine bits = exhaust flare. Check before the generic ENGINE
    # rule so a "*ENGINEGLOW*" lands as exhaust, not as a split nacelle.
    if "GLOW" in n and any(k in n for k in ("ENGINE", "BACK", "BURNER", "EXHAUST")):
        return "exhaust"
    if "ENGINE" in n:
        return "engine"          # nacelle body — split into exhaust + intake
    return None


# ─────────────────────────────── OBJ parse ──────────────────────────────


def parse_obj_material_verts(obj_path: Path
                             ) -> tuple[list[tuple[float, float, float]],
                                        dict[str, set[int]]]:
    """Return (all_vertices, {material_name: set_of_vertex_indices}).

    Accumulates the vertex indices touched by faces under each `usemtl`
    block. A material that appears in several blocks merges into one set.
    Vertex indices are 0-based into the returned vertex list.
    """
    verts: list[tuple[float, float, float]] = []
    mat_verts: dict[str, set[int]] = {}
    cur = None
    for line in obj_path.read_text().splitlines():
        if line.startswith("v "):
            _, x, y, z = line.split()[:4]
            verts.append((float(x), float(y), float(z)))
        elif line.startswith("usemtl "):
            cur = line[7:].strip()
            mat_verts.setdefault(cur, set())
        elif line.startswith("f ") and cur is not None:
            for tok in line.split()[1:]:
                # f v, v/vt, v/vt/vn, v//vn — first field is the position idx
                vi = tok.split("/")[0]
                if vi:
                    mat_verts[cur].add(int(vi) - 1)   # OBJ is 1-based
    return verts, mat_verts


# ─────────────────────────────── clustering ─────────────────────────────


def cluster_points(points: list[tuple[float, float, float]],
                   radius: float) -> list[list[int]]:
    """Greedy single-link clustering: indices within `radius` of an existing
    cluster member join that cluster. O(n²) but n is tiny per material
    (hundreds at most). Returns lists of indices into `points`."""
    n = len(points)
    unassigned = set(range(n))
    clusters: list[list[int]] = []
    r2 = radius * radius
    while unassigned:
        seed = unassigned.pop()
        stack = [seed]
        group = [seed]
        while stack:
            i = stack.pop()
            xi, yi, zi = points[i]
            close = [j for j in unassigned
                     if (points[j][0]-xi)**2 + (points[j][1]-yi)**2
                        + (points[j][2]-zi)**2 <= r2]
            for j in close:
                unassigned.discard(j)
                stack.append(j)
                group.append(j)
        clusters.append(group)
    return clusters


def _centroid(pts: list[tuple[float, float, float]]) -> tuple[float, float, float]:
    n = len(pts)
    sx = sum(p[0] for p in pts); sy = sum(p[1] for p in pts); sz = sum(p[2] for p in pts)
    return (sx/n, sy/n, sz/n)


def _normalize(v: tuple[float, float, float]) -> tuple[float, float, float]:
    m = math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])
    if m < 1e-9:
        return (0.0, 0.0, 1.0)
    return (v[0]/m, v[1]/m, v[2]/m)


def _axis_extremes(pts: list[tuple[float, float, float]]
                   ) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    """Return the two points at the extremes of the cluster's longest
    spatial axis (approximated by the bbox's dominant dimension). Cheap
    stand-in for full PCA — good enough to find the two ends of a
    roughly-cylindrical engine nacelle."""
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]; zs = [p[2] for p in pts]
    spans = (max(xs)-min(xs), max(ys)-min(ys), max(zs)-min(zs))
    axis = spans.index(max(spans))
    lo = min(pts, key=lambda p: p[axis])
    hi = max(pts, key=lambda p: p[axis])
    return lo, hi


# ─────────────────────────────── extraction ─────────────────────────────


def extract_lights(stem: str, threshold_frac: float) -> dict:
    obj  = WCNEWS / f"{stem}.obj"
    mats = WCNEWS / f"{stem}.materials.json"
    if not obj.exists() or not mats.exists():
        raise FileNotFoundError(f"missing {obj.name} or {mats.name}")

    verts, mat_verts = parse_obj_material_verts(obj)
    if not verts:
        raise ValueError("no vertices in obj")

    # Mesh bbox → clustering radius + ship centroid (for exhaust/intake end
    # disambiguation: the exhaust end of a nacelle is the one FARTHER from
    # the ship's centre, i.e. sticking out the back).
    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]
    diag = math.sqrt((max(xs)-min(xs))**2 + (max(ys)-min(ys))**2 + (max(zs)-min(zs))**2)
    radius = diag * threshold_frac
    ship_c = (0.0, 0.0, 0.0)   # mesh is recentred on its bbox centre at import

    lights: list[dict] = []

    def add_light(pos, role):
        preset = ROLE_PRESETS[role]
        lights.append({
            "pos":    [round(pos[0], 4), round(pos[1], 4), round(pos[2], 4)],
            "normal": list(_normalize((pos[0]-ship_c[0], pos[1]-ship_c[1], pos[2]-ship_c[2]))),
            "color":  preset["color"],
            "kind":   preset["kind"],
            "hz":     preset["hz"],
            "role":   role,
        })

    # Gather candidate clusters per role (with vertex counts) so we can
    # rank by size and keep only the biggest few before emitting lights.
    role_clusters: dict = {}
    for mat_name, vidx in mat_verts.items():
        role = classify_material(mat_name)
        if role is None or not vidx:
            continue
        pts = [verts[i] for i in vidx]
        for group in cluster_points(pts, radius):
            gpts = [pts[i] for i in group]
            if len(gpts) < MIN_CLUSTER_VERTS:   # ignore panel specks
                continue
            role_clusters.setdefault(role, []).append(gpts)

    for role, clusters in role_clusters.items():
        clusters.sort(key=len, reverse=True)     # biggest fixtures first
        for gpts in clusters[:ROLE_CAPS.get(role, 4)]:
            if role == "radar":
                add_light(_centroid(gpts), "radar")
            elif role == "exhaust":
                add_light(_centroid(gpts), "exhaust")
            elif role == "engine":
                # Split the nacelle: blue exhaust at the end farther from
                # ship centre, red intake at the nearer end.
                lo, hi = _axis_extremes(gpts)
                d_lo = sum((lo[k]-ship_c[k])**2 for k in range(3))
                d_hi = sum((hi[k]-ship_c[k])**2 for k in range(3))
                exhaust_pt, intake_pt = (hi, lo) if d_hi > d_lo else (lo, hi)
                add_light(exhaust_pt, "exhaust")
                add_light(intake_pt,  "intake")

    return {
        "ship": stem,
        "source": "material_zones",
        "cluster_radius": round(radius, 3),
        # Local-space axis-aligned bounding box. The atlas renderer projects
        # these 8 corners alongside the lights so it can map projected UVs
        # into the cropped sprite cell WITHOUT hardcoding title-bar pixels
        # or HiDPI scale factors (self-calibrating; see render_3d_sprite_
        # atlases.py).
        "aabb": [[round(min(xs), 4), round(min(ys), 4), round(min(zs), 4)],
                 [round(max(xs), 4), round(max(ys), 4), round(max(zs), 4)]],
        "lights": lights,
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("ships", nargs="*", help="ship .obj stems (e.g. clunker)")
    p.add_argument("--all", action="store_true", help="every .obj in ships_wcnews/")
    p.add_argument("--threshold", type=float, default=0.045,
                   help="cluster radius as fraction of mesh diagonal (default 0.045)")
    p.add_argument("--radar-only", action="store_true",
                   help="only emit radar-dish lights (RADARDISH material). The "
                        "engine materials in this archive smear over whole "
                        "nacelles, so their auto-placed exhaust/intake lights "
                        "are imprecise — radar dishes are the one clean signal.")
    args = p.parse_args()

    # Radar-only: zero the engine/exhaust caps so only RADARDISH survives.
    if args.radar_only:
        ROLE_CAPS["engine"] = 0
        ROLE_CAPS["exhaust"] = 0

    if args.all:
        stems = sorted(pp.stem for pp in WCNEWS.glob("*.obj")
                       if not pp.stem.endswith("_probe"))
    else:
        stems = args.ships
    if not stems:
        print("specify ship stems or --all", file=sys.stderr)
        return 1

    for stem in stems:
        try:
            data = extract_lights(stem, args.threshold)
        except (FileNotFoundError, ValueError) as exc:
            print(f"  {stem}: SKIP ({exc})", file=sys.stderr)
            continue
        out = WCNEWS / f"{stem}.lights3d.json"
        out.write_text(json.dumps(data, indent=2) + "\n")
        by_role: dict[str, int] = {}
        for l in data["lights"]:
            by_role[l["role"]] = by_role.get(l["role"], 0) + 1
        summary = ", ".join(f"{v} {k}" for k, v in sorted(by_role.items())) or "none"
        print(f"  {stem}: {len(data['lights'])} lights ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow"]
# ///
"""render_blender_sprite_atlas.py — the azimuth/elevation sprite capture for
ships whose mesh lives in a .blend (Meshy hulls, #699) rather than in the
engine's wcnews OBJ roster.

Same contract as render_3d_sprite_atlases.py, whose grid, camera math, cell
framing, light-spot schema and manifest writer it reuses. Only the "pose the
camera and grab a frame" step differs: Blender renders every cell headless
(blender_atlas_capture.py) instead of booting the engine and screencapturing
it, which only works on macOS. Every cell is rendered, none mirrored, so the
world-fixed studio sun stays honest on both flanks.

Feature lights come from a `<mesh>.lights3d.json` (the extract_ship_lights.py
schema: pos, normal, color, kind, hz, plus optional size/phase) in the
.blend's world frame converted to engine axes (+Y up, +Z nose). Each cell
gets a `.lights.json` sidecar with the lights that are visible from it.

Usage:
  uv run tools/render_blender_sprite_atlas.py blacksun490 \\
      --blend assets/meshes/ships_meshy/blacksun490.blend
  uv run tools/render_blender_sprite_atlas.py blacksun490 --blend ... --skip-render
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from render_3d_sprite_atlases import (  # noqa: E402  (path hack)
    ALL_VIEWS, CAPTURE_LENGTH_M, ORBIT_RADIUS_M, SHIPS_DIR,
    camera_for_orbit, cell_name, cell_uv, fit_cell, light_spot,
    write_cell_lights, write_manifest,
)

REPO = Path(__file__).resolve().parents[1]
CAPTURE_SCRIPT = Path(__file__).with_name("blender_atlas_capture.py")

# The engine's capture-clean studio light: apply_studio_sun in src/main.cpp
# puts the sun at (-200000, 150000, 200000), and capture scenes set
# ambient_floor 0.4. Energies are tuned so cells read like the engine's.
STUDIO_SUN_DIR = [-200000.0, 150000.0, 200000.0]
SUN_ENERGY = 3.0
AMBIENT = 0.35
FOV_DEG = 40.0          # wide enough for any pose at ORBIT_RADIUS_M; cells are cropped anyway
RENDER_PX = 1024        # raw frame size; cells are downsampled from it


def _blender() -> str:
    found = os.environ.get("BLENDER") or shutil.which("blender")
    if found:
        return found
    root = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Blender Foundation"
    installs = sorted(root.glob("Blender */blender.exe"))
    if not installs:
        sys.exit("no Blender: put it on PATH or set BLENDER")
    return str(installs[-1])


def render(name: str, blend: Path, lights: Path | None, raw_dir: Path) -> None:
    raw_dir.mkdir(parents=True, exist_ok=True)
    job = {
        "raw_dir": str(raw_dir), "resolution": RENDER_PX, "fov_deg": FOV_DEG,
        "length_m": CAPTURE_LENGTH_M, "radius_m": ORBIT_RADIUS_M,
        "sun_dir": STUDIO_SUN_DIR, "sun_energy": SUN_ENERGY, "ambient": AMBIENT,
        "lights": str(lights) if lights else None,
        "views": [{"file": cell_name(name, az, el),
                   "cam": camera_for_orbit(az, el, ORBIT_RADIUS_M)}
                  for az, el in ALL_VIEWS],
    }
    # The job holds machine-local absolute paths: keep it out of the repo.
    job_path = REPO / "build" / "blender_atlas" / f"{name}_job.json"
    job_path.parent.mkdir(parents=True, exist_ok=True)
    job_path.write_text(json.dumps(job, indent=1))
    subprocess.run([_blender(), "-b", str(blend), "--python", str(CAPTURE_SCRIPT),
                    "--", str(job_path)], check=True)


def post(name: str, codename: str, lights: Path | None, raw_dir: Path,
         cell_size: int) -> tuple[int, int]:
    """Frame every raw render into its cell, map the projected lights into
    cell UV and write the sidecars and manifest. Returns (cells, lit cells)."""
    cell_dir = SHIPS_DIR / name / "sprites_3d"
    specs = json.loads(lights.read_text())["lights"] if lights else []
    proj = json.loads((raw_dir / "projections.json").read_text()) if specs else {}
    n_ok = n_lit = 0
    for az, el in ALL_VIEWS:
        cell = cell_name(name, az, el)
        with Image.open(raw_dir / cell) as im:
            xform = fit_cell(im.convert("RGBA"), cell_dir / cell, cell_size)
        if xform is None:
            continue
        n_ok += 1
        if not specs:
            continue
        spots = [light_spot(spec, *cell_uv(p["px"], p["py"], xform))
                 for spec, p in zip(specs, proj[cell]) if p["visible"]]
        spots = [s for s in spots if s is not None]
        write_cell_lights(cell_dir / cell, spots)
        n_lit += bool(spots)
    write_manifest(name, codename, cell_size)
    return n_ok, n_lit


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("ship", help="atlas name, e.g. blacksun490")
    p.add_argument("--blend", type=Path, required=True,
                   help="ship .blend; its stem is the manifest's mesh_codename")
    p.add_argument("--lights", type=Path,
                   help="feature lights (default: <blend stem>.lights3d.json beside it)")
    p.add_argument("--cell-size", type=int, default=512)
    p.add_argument("--skip-render", action="store_true",
                   help="reuse the raw frames, only redo framing, lights and manifest")
    args = p.parse_args()

    lights = args.lights or args.blend.with_name(f"{args.blend.stem}.lights3d.json")
    lights = lights if lights.exists() else None
    raw_dir = SHIPS_DIR / args.ship / "sprites_3d" / "raw"
    if not args.skip_render:
        render(args.ship, args.blend.resolve(), lights and lights.resolve(), raw_dir)
    n_ok, n_lit = post(args.ship, args.blend.stem, lights, raw_dir, args.cell_size)
    print(f"{args.ship}: {n_ok}/{len(ALL_VIEWS)} cells, {n_lit} with lights")
    return 0 if n_ok == len(ALL_VIEWS) else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""render_3d_sprite_atlases.py — batch-generate sprite atlases directly from
the wcnews 3D meshes, skipping the AI image generator entirely.

Pipeline per ship:
  1. Write an ephemeral capture scene (single mesh at world origin, with
     the per-ship orientation overrides we worked out for the showroom).
  2. Boot the engine with `--capture-clean` (skybox / dust / sun / bloom
     all auto-skipped — gives us a pure black background, no atmospherics).
  3. Orbit the camera through 80 (azimuth, elevation) positions = the same
     16×5 grid the existing engine sprite system expects.
  4. For each viewpoint: POST /screenshot, wait for /tmp/np_shot.png to
     appear, post-process it (chroma-key black -> transparent alpha, crop
     to the ship's bounding box, centre and resize to a fixed cell size).
  5. Write everything to `assets/ships/<ship>/sprites_3d/` and emit an
     `atlas_manifest_3d.json` in the schema the engine already loads.

Why a fresh script instead of extending render_ship_atlas.py:
  render_ship_atlas.py is the existing GROUND-TRUTH renderer feeding the
  AI sprite pipeline. Its outputs are pre-shading reference frames, NOT
  finished sprites — they still get fed through ChatGPT-image-2 +
  pixelart post. This new script PRODUCES the finished sprites directly,
  so it owns the alpha-keying, cropping, and atlas-manifest steps the old
  pipeline farmed out to a half-dozen downstream tools.

Usage:
  tools/render_3d_sprite_atlases.py                  # every ship in SHIPS
  tools/render_3d_sprite_atlases.py --only orion talon
  tools/render_3d_sprite_atlases.py --cell-size 256  # smaller cells
  tools/render_3d_sprite_atlases.py --skip-render    # re-do post only
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from PIL import Image

REPO       = Path(__file__).resolve().parents[1]
SYSTEMS    = REPO / "assets" / "systems"
SHIPS_DIR  = REPO / "assets" / "ships"
GAME_BIN   = REPO / "build" / "new_privateer"
API        = "http://127.0.0.1:47001"
SHOT_PATH  = Path("/tmp/np_shot.png")

# Full sphere coverage: 16 azimuths × 5 mid-elevations + 2 polar caps = 82 cells.
# Azimuth resolution matches the existing AI atlases so the engine's cell-
# selection math just works. Polar caps are single cells (looking straight
# down / straight up the ship's pitch axis); azimuth at the pole is
# degenerate so we always store them under az=0.
AZIMUTHS         = [i * 22.5 for i in range(16)]
ELEVATIONS       = [-60.0, -30.0, 0.0, 30.0, 60.0]
POLAR_ELEVATIONS = [-90.0, 90.0]                # bottom-up + top-down

# Mirror-symmetry optimization. Most ships are bilateral mirrors across
# their nose-tail (X=0) plane, so views from az>180 are pixel-perfect
# horizontal flips of their az<180 mirror partners. We only RENDER the
# unique half + symmetry-plane cells, then derive the rest with a cheap
# PIL flip pass — cuts engine-boot+orbit time from 80 cells/ship to 47.
# The on-disk atlas still ships all 80+2 PNGs so the existing sprite
# picker doesn't need a mirror-aware branch (yet).
UNIQUE_AZIMUTHS = [az for az in AZIMUTHS if 0.0 <= az <= 180.0]   # 9 of 16


def _mirror_source_az(az: float) -> float | None:
    """For an azimuth in the mirrored half (az > 180), return the unique
    azimuth whose render gets h-flipped to produce it. Returns None when
    `az` is itself a unique azimuth (no mirroring needed)."""
    if 0.0 <= az <= 180.0:
        return None
    return 360.0 - az

# Single source of truth for the ship roster + orientations: lifted
# straight from the showroom generator. Any tweak made via the in-engine
# F5 mesh_orient_editor → baked into PER_SHIP_OVERRIDES → automatically
# picked up here on the next render run. No drift, one canonical list.
sys.path.insert(0, str(Path(__file__).parent))
from regenerate_mesh_showroom import (        # noqa: E402  (path hack)
    KEEP_AND_RENAME, PER_SHIP_OVERRIDES,
)

# Defaults applied when a ship has no PER_SHIP_OVERRIDES entry. MUST
# match the showroom generator's defaults (build_mesh_entry there) or
# sprite captures will diverge from what Mike sees flying around.
DEFAULT_EULER = [90.0, 0.0, 180.0]
DEFAULT_TINT  = [1.0, 1.0, 1.0]
DEFAULT_SPEC  = 0.5


def _build_ship_list() -> list[dict]:
    """Materialise the ship roster from the showroom generator's data.

    Each output entry: {codename, name, euler_deg, tint} — the exact
    shape downstream render functions expect. Ships missing from disk
    (i.e. KEEP_AND_RENAME refers to an obj that isn't in ships_wcnews/)
    are silently dropped; the import_3ds_meshes step would have already
    warned about them.
    """
    wcnews_dir = REPO / "assets" / "meshes" / "ships_wcnews"
    on_disk = {p.stem for p in wcnews_dir.glob("*.obj")} if wcnews_dir.exists() else set()
    ships: list[dict] = []
    for codename, display_name in KEEP_AND_RENAME.items():
        if codename not in on_disk:
            continue
        overrides = PER_SHIP_OVERRIDES.get(codename, {})
        ships.append({
            "codename":  codename,
            "name":      display_name,
            "euler_deg": overrides.get("euler_deg", DEFAULT_EULER),
            "tint":      overrides.get("tint",      DEFAULT_TINT),
            "spec":      overrides.get("spec",      DEFAULT_SPEC),
        })
    # Sort by display name so the per-batch progress reads alphabetically.
    ships.sort(key=lambda s: s["name"])
    return ships


SHIPS = _build_ship_list()

CAPTURE_LENGTH_M = 30.0          # nominal ship length in the capture scene
# Camera distance ≈ 2× ship length. At 1.2× (radius 35) the diagonal
# extent of asymmetric ships (paradigm wings, orion engine outriggers)
# clipped at the frame edge for several azimuths. 60 m gives enough
# breathing room for any axis-aligned orbit pose without making the
# silhouette so small that post-process upscale loses crispness.
ORBIT_RADIUS_M   = 60.0


# ───────────────────────────── HTTP / camera ─────────────────────────────


def _api_post(endpoint: str, data: dict | None = None,
              timeout: float = 5.0) -> None:
    body = json.dumps(data).encode() if data else b""
    req  = urllib.request.Request(f"{API}{endpoint}", data=body, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        resp.read()


def _wait_for_api(deadline_s: float = 15.0) -> bool:
    """Poll /state until the dev_remote thread is answering."""
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            with urllib.request.urlopen(f"{API}/state", timeout=1.0) as r:
                if r.status == 200:
                    return True
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            time.sleep(0.25)
    return False


def _camera_for_orbit(az_deg: float, el_deg: float, radius: float
                      ) -> tuple[float, float, float, float, float]:
    """Compute (x, y, z, yaw, pitch) so the camera sits on the viewing
    sphere at (az, el, r) and looks at the origin.

    Elevation sign convention: the engine's sprite picker (and the
    canonical Origin atlas convention documented in render_ship_atlas.py)
    treats el > 0 as the camera BELOW the ship looking UP at the ventral
    hull, and el < 0 as ABOVE looking DOWN at the dorsal hull. A naive
    `y = r*sin(el)` puts el>0 ABOVE, which inverts every cell's labelled
    elevation vs what the engine queries — so a cell tagged el=+60
    (engine: "belly view") would actually contain the dorsal view, and
    the in-game sprite picks the wrong frame as the camera changes
    altitude. We negate the elevation here so the captured frame matches
    its label under the engine's convention.
    """
    az = math.radians(az_deg)
    el = math.radians(-el_deg)        # see docstring: engine el>0 = below
    x  = radius * math.cos(el) * math.sin(az)
    y  = radius * math.sin(el)
    z  = radius * math.cos(el) * math.cos(az)
    # Yaw/pitch to look back at origin.
    r        = math.sqrt(x*x + y*y + z*z)
    yaw_deg  = math.degrees(math.atan2(x, z))
    pitch    = math.degrees(math.asin(-y / r)) if r > 1e-6 else 0.0
    return x, y, z, yaw_deg, pitch


def _set_camera(az: float, el: float, radius: float) -> None:
    x, y, z, yaw, pitch = _camera_for_orbit(az, el, radius)
    _api_post("/camera/set", {"x": x, "y": y, "z": z,
                              "yaw": yaw, "pitch": pitch})


def _take_screenshot(deadline_s: float = 3.0) -> Path:
    if SHOT_PATH.exists():
        SHOT_PATH.unlink()
    _api_post("/screenshot")
    end = time.time() + deadline_s
    while time.time() < end:
        if SHOT_PATH.exists() and SHOT_PATH.stat().st_size > 0:
            return SHOT_PATH
        time.sleep(0.05)
    raise TimeoutError("screenshot never appeared")


# ───────────────────────────── scene writer ──────────────────────────────


def _capture_scene_path(name: str) -> Path:
    """Ephemeral capture scenes get a `_3d_capture_` prefix so they sort
    next to each other in `assets/systems/` and are obvious to delete."""
    return SYSTEMS / f"_3d_capture_{name}.json"


def _write_capture_scene(ship: dict) -> Path:
    """Emit a minimal scene that puts ONE mesh at world origin with the
    ship-specific orientation override. capture-clean mode handles the
    black background + no-HUD bits for us."""
    scene = {
        "name":              f"3D Capture: {ship['name']}",
        "description":       (f"Auto-generated by tools/render_3d_sprite_atlases.py "
                              f"for {ship['name']} (mesh codename {ship['codename']}). "
                              f"Single placed_mesh at origin; run engine with "
                              f"--capture-clean for a black background."),
        "skybox_seed":       "troy",   # ignored when --capture-clean is set
        "star":              {"preset": "yellow"},
        "studio_lighting":   True,
        "asteroid_fields":     [],
        "placed_sprites":      [],
        "placed_ship_sprites": [],
        "placed_meshes": [{
            "obj":           f"meshes/ships_wcnews/{ship['codename']}.obj",
            "position":      [0, 0, 0],
            "euler_deg":     ship["euler_deg"],
            "length_meters": CAPTURE_LENGTH_M,
            "ambient_floor": 0.4,
            "spec":          ship.get("spec", DEFAULT_SPEC),
            "tint":          ship.get("tint", DEFAULT_TINT),
            "double_sided":  True,
        }],
        "nav_points":   [],
        "player_start": {"position": [0, 0, ORBIT_RADIUS_M]},
    }
    path = _capture_scene_path(ship["name"])
    path.write_text(json.dumps(scene, indent=2, ensure_ascii=False) + "\n")
    return path


# ───────────────────────────── post-process ──────────────────────────────


# `screencapture -l<wid>` returns the whole NSWindow including the macOS
# title bar (~28px logical). Its grey 'new_privateer — ...' caption text
# is well above our black threshold and pollutes the bounding box.
# Zero this many rows from the top of every raw frame before any alpha
# math runs. Slightly conservative — 28 actual + 4 slop for OS variance.
TITLE_BAR_ROWS = 32


def _black_to_alpha_crop(src: Path, dst: Path, cell_size: int,
                         margin_pct: float = 0.06,
                         black_thresh: int = 10) -> bool:
    """Convert a black-background screenshot to a centred sprite cell.

    Steps:
      * Open as RGBA. Zero out the title-bar strip first so its text
        doesn't survive thresholding.
      * Any pixel with all three RGB channels < `black_thresh` becomes
        transparent (alpha=0). Threshold tolerates the small amount of
        dithering / antialias bleed around dark hull edges.
      * Compute alpha-channel bounding box; bail if empty.
      * Crop to that box, leave `margin_pct` border (so the silhouette
        doesn't touch the cell edge — important for the engine's
        billboard scaler).
      * Letterbox-fit into a `cell_size × cell_size` canvas, centred.
    """
    im = Image.open(src).convert("RGBA")
    px = im.load()
    w, h = im.size
    # Nuke the title bar first.
    for y in range(min(TITLE_BAR_ROWS, h)):
        for x in range(w):
            px[x, y] = (0, 0, 0, 0)
    # Threshold the rest. Manual scan: 1280x800 (logical default) →
    # ~1M pixels, ~0.3s per frame, acceptable for 80-cell runs.
    for y in range(TITLE_BAR_ROWS, h):
        for x in range(w):
            r, g, b, _ = px[x, y]
            if r < black_thresh and g < black_thresh and b < black_thresh:
                px[x, y] = (0, 0, 0, 0)
    bbox = im.getbbox()
    if bbox is None:
        return False
    cropped = im.crop(bbox)
    cw, ch  = cropped.size
    target  = int(cell_size * (1.0 - 2 * margin_pct))
    scale   = min(target / cw, target / ch)
    new_w   = max(1, int(round(cw * scale)))
    new_h   = max(1, int(round(ch * scale)))
    resized = cropped.resize((new_w, new_h), Image.LANCZOS)
    canvas  = Image.new("RGBA", (cell_size, cell_size), (0, 0, 0, 0))
    canvas.paste(resized, ((cell_size - new_w) // 2,
                            (cell_size - new_h) // 2),
                 resized)
    dst.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(dst, "PNG")
    return True


def _az_tag(az: float) -> str:
    if abs(az - round(az)) < 0.01:
        return f"{int(round(az)):03d}"
    return f"{az:05.1f}".replace(".", "p")


def _el_tag(el: float) -> str:
    if abs(el - round(el)) < 0.01:
        return f"{int(round(el)):+04d}"
    sign = "+" if el >= 0 else "-"
    return sign + f"{abs(el):04.1f}".replace(".", "p")


def _cell_name(ship_name: str, az: float, el: float) -> str:
    return f"{ship_name}_az{_az_tag(az)}_el{_el_tag(el)}_3d.png"


# ───────────────────────────── per-ship run ──────────────────────────────


def _boot_game(scene_stem: str, log_path: Path) -> subprocess.Popen:
    """Launch the engine in capture-clean mode against `scene_stem`."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_f = open(log_path, "w")
    proc  = subprocess.Popen(
        [str(GAME_BIN), "--system", scene_stem, "--capture-clean"],
        stdout=log_f, stderr=subprocess.STDOUT,
        # New process group so a Ctrl-C on us also kills the engine.
        preexec_fn=os.setsid)
    return proc


def _kill_game(proc: subprocess.Popen) -> None:
    if proc.poll() is None:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)


def _render_one_ship(ship: dict, cell_size: int, skip_render: bool
                     ) -> tuple[int, int]:
    """Run the full capture+post pipeline for one ship.

    Returns (cells_written, cells_blank). `blank` is the count of
    viewpoints where the post-process bailed because the screenshot
    contained only black pixels (would indicate a render bug)."""
    name      = ship["name"]
    raw_dir   = SHIPS_DIR / name / "sprites_3d" / "raw"
    cell_dir  = SHIPS_DIR / name / "sprites_3d"
    raw_dir.mkdir(parents=True, exist_ok=True)

    if not skip_render:
        scene_path = _write_capture_scene(ship)
        scene_stem = scene_path.stem    # e.g. "_3d_capture_orion"
        log_path   = REPO / "build" / f"{scene_stem}.log"

        print(f"  booting engine for {name} ...")
        proc = _boot_game(scene_stem, log_path)
        try:
            if not _wait_for_api():
                raise RuntimeError(f"dev_remote never came up — see {log_path}")
            time.sleep(0.5)  # extra settle after API answers; mesh load is async-ish

            # Phase A: render only the symmetry-unique cells.
            for el in ELEVATIONS:
                for az in UNIQUE_AZIMUTHS:
                    _set_camera(az, el, ORBIT_RADIUS_M)
                    time.sleep(0.08)              # let camera apply
                    shot = _take_screenshot()
                    out  = raw_dir / _cell_name(name, az, el)
                    shutil.copyfile(shot, out)

            # Phase B: polar caps. Azimuth is degenerate at ±90 elevation,
            # so we always capture under az=0. The engine's camera-set API
            # only takes yaw+pitch (no roll), which means the "up" vector
            # at the pole defaults to world-up — fine for an in-engine
            # sprite picker that doesn't care about rotational alignment
            # of polar cells.
            for polar_el in POLAR_ELEVATIONS:
                _set_camera(0.0, polar_el, ORBIT_RADIUS_M)
                time.sleep(0.08)
                shot = _take_screenshot()
                out  = raw_dir / _cell_name(name, 0.0, polar_el)
                shutil.copyfile(shot, out)
        finally:
            _kill_game(proc)

        # Phase C: mirror-fill the symmetric half from the unique renders.
        # Done out-of-process (no engine needed) so we tear down the game
        # ASAP and free the GPU before the mirror pass.
        for el in ELEVATIONS:
            for az in AZIMUTHS:
                src_az = _mirror_source_az(az)
                if src_az is None:
                    continue
                src = raw_dir / _cell_name(name, src_az, el)
                dst = raw_dir / _cell_name(name, az,     el)
                if not src.exists():
                    continue
                with Image.open(src) as im:
                    im.transpose(Image.FLIP_LEFT_RIGHT).save(dst)

    # Post-process every raw frame on disk (idempotent). Iterates the
    # FULL 82-cell set including the polar caps and the mirror-derived
    # azimuths — the cells either exist (rendered or mirrored above) or
    # we count them as blank.
    all_views = ([(az, el) for el in ELEVATIONS for az in AZIMUTHS] +
                 [(0.0, el) for el in POLAR_ELEVATIONS])
    n_ok = n_blank = 0
    for az, el in all_views:
        raw  = raw_dir  / _cell_name(name, az, el)
        cell = cell_dir / _cell_name(name, az, el)
        if not raw.exists():
            n_blank += 1
            continue
        if _black_to_alpha_crop(raw, cell, cell_size):
            n_ok += 1
        else:
            n_blank += 1

    # Atlas manifest — same shape every other ship in the engine uses,
    # plus the two polar-cap samples appended at the end.
    samples = []
    for el in ELEVATIONS:
        for az in AZIMUTHS:
            rel = f"ships/{name}/sprites_3d/{_cell_name(name, az, el)}"
            samples.append({"az": az, "el": el, "sprite": rel})
    for polar_el in POLAR_ELEVATIONS:
        rel = f"ships/{name}/sprites_3d/{_cell_name(name, 0.0, polar_el)}"
        samples.append({"az": 0.0, "el": polar_el, "sprite": rel})
    manifest = {
        "ship":              name,
        "kind":              "view_sphere_sprite_atlas",
        "source":            "3d_mesh_capture",
        "mesh_codename":     ship["codename"],
        "forward_axis":      "+Z",
        "up_axis":           "+Y",
        "azimuth_degrees":   AZIMUTHS,
        "elevation_degrees": ELEVATIONS + POLAR_ELEVATIONS,
        "pixel_grid":        cell_size,
        "samples":           samples,
    }
    (SHIPS_DIR / name / "atlas_manifest_3d.json").write_text(
        json.dumps(manifest, indent=2) + "\n")
    return n_ok, n_blank


# ─────────────────────────────── main ────────────────────────────────────


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--only", nargs="*", default=None,
                   help="ship display names to render (default: all 17)")
    p.add_argument("--cell-size", type=int, default=512,
                   help="output cell width/height in pixels (default: 512)")
    p.add_argument("--skip-render", action="store_true",
                   help="don't re-screenshot, just re-run the post-process step")
    p.add_argument("--keep-scenes", action="store_true",
                   help="leave the _3d_capture_*.json scene files in place")
    args = p.parse_args()

    ships = SHIPS
    if args.only:
        wanted = set(args.only)
        ships  = [s for s in ships if s["name"] in wanted]
        missing = wanted - {s["name"] for s in ships}
        if missing:
            print(f"warning: unknown ship name(s): {sorted(missing)}",
                  file=sys.stderr)
    if not ships:
        print("nothing to do", file=sys.stderr)
        return 1

    print(f"plan: {len(ships)} ship(s), {len(AZIMUTHS)}x{len(ELEVATIONS)} = "
          f"{len(AZIMUTHS) * len(ELEVATIONS)} cells each, "
          f"cell_size={args.cell_size}px")
    if not GAME_BIN.exists() and not args.skip_render:
        print(f"missing {GAME_BIN.relative_to(REPO)} — build first", file=sys.stderr)
        return 2

    total_ok = total_blank = 0
    for i, ship in enumerate(ships, 1):
        print(f"[{i}/{len(ships)}] {ship['name']:<12s} "
              f"(mesh={ship['codename']})")
        try:
            n_ok, n_blank = _render_one_ship(ship, args.cell_size,
                                             args.skip_render)
        except Exception as exc:
            print(f"  FAILED: {exc}", file=sys.stderr)
            continue
        print(f"  → {n_ok} cells written, {n_blank} blank")
        total_ok += n_ok; total_blank += n_blank

        if not args.keep_scenes and not args.skip_render:
            sp = _capture_scene_path(ship["name"])
            if sp.exists(): sp.unlink()

    print(f"\ndone — {total_ok} cells total, {total_blank} blank")
    return 0 if total_blank == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

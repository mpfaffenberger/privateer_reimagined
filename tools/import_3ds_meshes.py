#!/usr/bin/env python3
"""import_3ds_meshes.py — convert wcnews fan-archive `.3ds + .bmp` ship models
into the engine's `.obj + .png + .materials.json` shape.

Source : assets/meshes/ships_raw/<shipdir>/<SHIP>.3ds  (+ .bmp textures)
Target : assets/meshes/ships_wcnews/<shipdir>.obj
         assets/meshes/ships_wcnews/<shipdir>.materials.json
         assets/meshes/ships_wcnews/<shipdir>_<texname>.png

Why pure-stdlib:
    The 3DS chunk format has been frozen since '92 and is dead simple
    (uint16 chunk_id + uint32 size + nested children). A specialised
    parser tuned to our exact output is ~200 lines and beats a heavy
    dep (trimesh / pyassimp / Blender) on every axis that matters here:
    no install, no version drift, no surprise transformations. Image
    conversion is delegated to macOS `sips` — also zero-install.

Output convention:
    * One OBJ per source .3ds. Each NAMED_OBJECT inside the .3ds becomes
      a `g <name>` group; each MSH_MAT_GROUP becomes a `usemtl <matname>`
      span inside that group. The OBJ loader keys per-submesh materials
      off `usemtl`, so the material name in materials.json MUST match.
    * Textures are prefixed with the ship dirname so two ships sharing
      a generic texture name (REFMAP.bmp / body.bmp) don't collide.
    * Lowercase output stems for the textures (lookup is case-sensitive
      on Linux), preserved-case for the `name` field in materials.json
      so it round-trips through the `usemtl` write side.

Usage:
    tools/import_3ds_meshes.py                       # convert every ship
    tools/import_3ds_meshes.py --only demon dralthi  # just these
    tools/import_3ds_meshes.py --list                # show plan, no I/O
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import zlib
import binascii
from dataclasses import dataclass, field
from pathlib import Path

REPO     = Path(__file__).resolve().parents[1]
SRC_DIR  = REPO / "assets" / "meshes" / "ships_raw"
DST_DIR  = REPO / "assets" / "meshes" / "ships_wcnews"

# Per-(ship, material) RGB overrides applied AFTER the .3ds parser pulls
# the raw diffuse colour. Some authored materials in the source archive
# are literal bugs — e.g. drone's GREENGLOW slot is stored as pure black,
# even though it's named like an emissive. Other materials are just
# unflattering at the engine's lighting + tone-map (drone's ALIEN patches
# came in at (30,30,30) muddy grey instead of the canonical bright green
# Privateer alien tech). Listing the fixes here keeps the converter
# pipeline as the single source of truth — re-running the import
# regenerates the corrected tint PNGs automatically.
#
# Keys: (ship_dir_name, material_name) — both case-sensitive, matching
#       the on-disk subdir under ships_raw/ and the material slot name
#       embedded in the .3ds file.
# Values: (r, g, b) in 0..1 range; gets baked into the 1×1 tint PNG.
MATERIAL_OVERRIDES: dict[tuple[str, str], tuple[float, float, float]] = {
    # ── Drone + Scout share an authoring pattern in the wcnews archive:
    #   * "SCOUT" / "DRONE"     → body hull, stored as muddy (30,30,30)
    #   * "ALIEN" / "ALIENCORRUG" → alien-tech accent panels, also muddy
    #   * "GREENGLOW"           → emissive, stored as PURE BLACK (bug)
    # Both ships are canonically "Steltek alien tech with bright green
    # glowing inserts". The corrected palette below restores that look:
    # body stays neutral grey (a touch darker than the source-mud), and
    # every alien-tech slot snaps to the same bright Privateer green.
    ("drone", "DRONE"):       (0.25, 0.25, 0.25),
    ("drone", "ALIEN"):       (0.10, 0.85, 0.25),
    ("drone", "GREENGLOW"):   (0.10, 0.85, 0.25),
    # Scout: mostly grey hull, but the small ALIEN accent spots get the
    # canonical Steltek green back. Iteration log:
    #   v1 (all green) → arms too froggy
    #   v2 (arms grey, rest green) → cuffs/rings too loud
    #   v3 (all grey) → boring, missed the alien-tech vibe
    #   v4 → keep ALIEN green (it's the small accent spots), grey the rest
    ("scout", "SCOUT"):       (0.25, 0.25, 0.25),
    ("scout", "ALIENCORRUG"): (0.25, 0.25, 0.25),
    ("scout", "ALIEN"):       (0.10, 0.85, 0.25),
    ("scout", "GREENGLOW"):   (0.25, 0.25, 0.25),
}

# ──────────────────────────── 3DS chunk constants ───────────────────────────
# Only the chunk IDs we actually consume. Anything not listed gets skipped
# wholesale via its declared size — that's how we stay forward-compatible
# with the long tail of variant chunks Discreet kept adding over the years.

CHUNK_MAIN3DS         = 0x4D4D
CHUNK_MDATA           = 0x3D3D
CHUNK_NAMED_OBJECT    = 0x4000
CHUNK_N_TRI_OBJECT    = 0x4100
CHUNK_POINT_ARRAY     = 0x4110
CHUNK_FACE_ARRAY      = 0x4120
CHUNK_MSH_MAT_GROUP   = 0x4130
CHUNK_TEX_VERTS       = 0x4140
CHUNK_MAT_ENTRY       = 0xAFFF
CHUNK_MAT_NAME        = 0xA000
CHUNK_MAT_DIFFUSE     = 0xA020
CHUNK_MAT_TEXMAP      = 0xA200      # diffuse
CHUNK_MAT_SPECMAP     = 0xA204      # specular intensity
CHUNK_MAT_OPACMAP     = 0xA210      # opacity
CHUNK_MAT_BUMPMAP     = 0xA230      # bump / normal
CHUNK_MAT_REFLMAP     = 0xA220      # reflection
CHUNK_MAT_SHIN_MAP    = 0xA33C      # shininess
CHUNK_MAT_SELF_ILMAP  = 0xA33D      # self-illumination → "glow"
CHUNK_MAT_MAPNAME     = 0xA300      # cstring child of every *MAP chunk

# ───────────────────────────── parsed-data types ────────────────────────────


@dataclass
class Material:
    name:     str
    diffuse:  str = ""     # texture filename (raw, as referenced in the .3ds)
    spec:     str = ""
    glow:     str = ""     # SELF_ILMAP
    bump:     str = ""
    # RGB diffuse color (0..1) — used to seed a tint-only material when
    # the .3ds entry has no diffuse map (a few of the wcnews ships paint
    # parts of the hull with flat colors).
    color_rgb: tuple[float, float, float] | None = None


@dataclass
class Submesh:
    """Faces in `face_indices` (into the parent NamedObject's face list)
    all share material `mat_name`. Empty mat_name → no material binding
    (the OBJ loader will fall back to the engine's neutral 1×1 white)."""
    mat_name:     str
    face_indices: list[int] = field(default_factory=list)


@dataclass
class NamedObject:
    name:     str
    verts:    list[tuple[float, float, float]] = field(default_factory=list)
    uvs:      list[tuple[float, float]]        = field(default_factory=list)
    faces:    list[tuple[int, int, int]]       = field(default_factory=list)
    submeshes: list[Submesh]                   = field(default_factory=list)


@dataclass
class ParsedScene:
    materials: list[Material]    = field(default_factory=list)
    objects:   list[NamedObject] = field(default_factory=list)


# ─────────────────────────────── 3DS parser ─────────────────────────────────


def _read_cstring(buf: memoryview, off: int) -> tuple[str, int]:
    """Read NUL-terminated cstring starting at `off`. Returns (str, next_off)."""
    end = off
    while end < len(buf) and buf[end] != 0:
        end += 1
    return bytes(buf[off:end]).decode("latin-1", "replace"), end + 1


def _iter_chunks(buf: memoryview, start: int, end: int):
    """Yield (chunk_id, body_view, body_start) for every chunk in [start, end).

    Each chunk header is 6 bytes: uint16 id + uint32 total_size (header included).
    Anything malformed (size < 6, size > remaining) terminates iteration cleanly
    — old 3DS files in the wild contain occasional junk past the last real chunk.
    """
    off = start
    while off + 6 <= end:
        cid, size = struct.unpack_from("<HI", buf, off)
        if size < 6 or off + size > end:
            return
        yield cid, buf[off + 6: off + size], off + 6
        off += size


def _parse_mapname(buf: memoryview, start: int, end: int) -> str:
    for cid, body, _ in _iter_chunks(buf, start, end):
        if cid == CHUNK_MAT_MAPNAME:
            name, _ = _read_cstring(body, 0)
            return name
    return ""


def _parse_color(buf: memoryview, start: int, end: int
                 ) -> tuple[float, float, float] | None:
    """A material color chunk wraps EITHER a COLOR_F (0x0010, 3×float32) or
    a COLOR_24 (0x0011, 3×uint8). Many 3DS files emit both 'main' and 'lin_'
    variants; we just take the first one we recognise."""
    for cid, body, _ in _iter_chunks(buf, start, end):
        if cid == 0x0010 and len(body) >= 12:
            return struct.unpack_from("<fff", body, 0)
        if cid == 0x0011 and len(body) >= 3:
            r, g, b = body[0] / 255.0, body[1] / 255.0, body[2] / 255.0
            return r, g, b
    return None


def _parse_material(buf: memoryview, start: int, end: int) -> Material:
    m = Material(name="")
    for cid, body, body_off in _iter_chunks(buf, start, end):
        body_end = body_off + len(body)
        if cid == CHUNK_MAT_NAME:
            m.name, _ = _read_cstring(body, 0)
        elif cid == CHUNK_MAT_DIFFUSE:
            m.color_rgb = _parse_color(buf, body_off, body_end)
        elif cid == CHUNK_MAT_TEXMAP:
            m.diffuse = _parse_mapname(buf, body_off, body_end)
        elif cid == CHUNK_MAT_SPECMAP:
            m.spec    = _parse_mapname(buf, body_off, body_end)
        elif cid == CHUNK_MAT_BUMPMAP:
            m.bump    = _parse_mapname(buf, body_off, body_end)
        elif cid == CHUNK_MAT_SELF_ILMAP:
            m.glow    = _parse_mapname(buf, body_off, body_end)
    return m


def _parse_trimesh(buf: memoryview, start: int, end: int, obj: NamedObject) -> None:
    for cid, body, _ in _iter_chunks(buf, start, end):
        if cid == CHUNK_POINT_ARRAY:
            (n,) = struct.unpack_from("<H", body, 0)
            stride = 12   # 3 × float32
            verts = struct.unpack_from(f"<{3*n}f", body, 2)
            obj.verts = [tuple(verts[i*3:i*3+3]) for i in range(n)]
        elif cid == CHUNK_FACE_ARRAY:
            (n,) = struct.unpack_from("<H", body, 0)
            tris = struct.unpack_from(f"<{4*n}H", body, 2)
            obj.faces = [(tris[i*4], tris[i*4+1], tris[i*4+2]) for i in range(n)]
            # Material groups are child chunks of FACE_ARRAY, sitting AFTER
            # the fixed-size face list. Their byte offset is 2 + 8*n bytes
            # into `body`.
            children_off = 2 + 8 * n
            for ccid, cbody, _ in _iter_chunks(body, children_off, len(body)):
                if ccid == CHUNK_MSH_MAT_GROUP:
                    mat_name, off = _read_cstring(cbody, 0)
                    (nf,) = struct.unpack_from("<H", cbody, off)
                    face_idx = list(struct.unpack_from(f"<{nf}H", cbody, off + 2))
                    obj.submeshes.append(Submesh(mat_name=mat_name,
                                                 face_indices=face_idx))
        elif cid == CHUNK_TEX_VERTS:
            (n,) = struct.unpack_from("<H", body, 0)
            uvs = struct.unpack_from(f"<{2*n}f", body, 2)
            obj.uvs = [(uvs[i*2], uvs[i*2+1]) for i in range(n)]


def parse_3ds(path: Path) -> ParsedScene:
    data = path.read_bytes()
    buf  = memoryview(data)
    if len(buf) < 6 or struct.unpack_from("<H", buf, 0)[0] != CHUNK_MAIN3DS:
        raise ValueError(f"{path.name}: not a 3DS file (bad MAIN3DS magic)")

    scene = ParsedScene()
    # Walk MAIN3DS → MDATA → {MAT_ENTRY, NAMED_OBJECT}
    main_end = struct.unpack_from("<I", buf, 2)[0]
    for cid, body, body_off in _iter_chunks(buf, 6, main_end):
        if cid != CHUNK_MDATA:
            continue
        body_end = body_off + len(body)
        for mid, mbody, mbody_off in _iter_chunks(buf, body_off, body_end):
            mbody_end = mbody_off + len(mbody)
            if mid == CHUNK_MAT_ENTRY:
                scene.materials.append(_parse_material(buf, mbody_off, mbody_end))
            elif mid == CHUNK_NAMED_OBJECT:
                name, after = _read_cstring(mbody, 0)
                obj = NamedObject(name=name)
                for sid, sbody, sbody_off in _iter_chunks(
                        buf, mbody_off + after, mbody_end):
                    if sid == CHUNK_N_TRI_OBJECT:
                        _parse_trimesh(buf, sbody_off, sbody_off + len(sbody), obj)
                if obj.verts and obj.faces:
                    scene.objects.append(obj)
    return scene


# ──────────────────────────── output writers ────────────────────────────────


def _sanitize_texname(raw: str) -> str:
    """Strip directory parts, lowercase, drop the `.bmp` suffix. Anything
    weird (spaces, colons) → underscore."""
    base = Path(raw).name.lower()
    if base.endswith(".bmp"):
        base = base[:-4]
    return "".join(c if c.isalnum() or c in "._-" else "_" for c in base)


def _write_tint_png(rgb: tuple[float, float, float], dst: Path) -> None:
    """Hand-rolled 1×1 RGB PNG writer (stdlib zlib + struct, no Pillow).

    PNG layout we emit:
        * 8-byte signature
        * IHDR (13B data: width=1, height=1, bit_depth=8, color_type=2/RGB,
                          compression=0, filter=0, interlace=0)
        * IDAT (zlib-compressed: one scanline = filter byte 0x00 + RGB triple)
        * IEND (empty)
    Each chunk: u32 length + 4-byte type + data + u32 crc32(type||data).
    Total file is ~70 bytes. Cheap and triggers zero new dependencies.
    """
    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = binascii.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    r = max(0, min(255, int(round(rgb[0] * 255))))
    g = max(0, min(255, int(round(rgb[1] * 255))))
    b = max(0, min(255, int(round(rgb[2] * 255))))
    sig  = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    pixel_row = bytes([0, r, g, b])      # filter byte 0 + one RGB triple
    idat = zlib.compress(pixel_row, 9)
    png  = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(png)


def _convert_bmp(src_dir: Path, raw_name: str, dst_path: Path) -> bool:
    """BMP → PNG via macOS `sips`. Returns True if PNG now exists at dst_path.

    3DS material maps name files case-sensitively but the on-disk wcnews
    archives are wildly inconsistent (mixed-case, lowercase, all-caps). We
    do a case-insensitive lookup against the source dir so the converter
    isn't broken by a single capital letter."""
    if dst_path.exists():
        return True
    if not raw_name:
        return False
    target = raw_name.lower()
    match = next((p for p in src_dir.iterdir()
                  if p.is_file() and p.name.lower() == target), None)
    if match is None:
        return False
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    res = subprocess.run(
        ["sips", "-s", "format", "png", str(match), "--out", str(dst_path)],
        capture_output=True)
    return res.returncode == 0 and dst_path.exists()


def write_obj(scene: ParsedScene, dst: Path) -> None:
    """Single-file OBJ: one `g <name>` per NamedObject, `usemtl` switches
    inside. Vertex / UV indices are 1-based and global across the file —
    standard OBJ convention.

    Note: 3DS stores UVs with v=0 at the BOTTOM; the engine's mesh shader
    flips v when sampling (mesh.glsl: `uv = vec2(v_uv.x, 1.0 - v_uv.y)`),
    so we write the raw UVs through unchanged. Any per-mesh `vflip` quirks
    can be fixed in the materials.json without touching the geometry.
    """
    lines: list[str] = [
        f"# Generated by tools/import_3ds_meshes.py from {dst.stem}",
        f"# objects={len(scene.objects)} materials={len(scene.materials)}",
        "",
    ]
    v_base = 1     # OBJ indices are 1-based; we keep a running offset
    vt_base = 1
    for obj in scene.objects:
        lines.append(f"g {obj.name}")
        for x, y, z in obj.verts:
            lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
        for u, v in obj.uvs:
            lines.append(f"vt {u:.6f} {v:.6f}")
        # Build a face→submesh map so we emit faces in submesh order with
        # one `usemtl` switch per group (instead of N tiny switches).
        face_to_mat: dict[int, str] = {}
        for sm in obj.submeshes:
            for fi in sm.face_indices:
                face_to_mat[fi] = sm.mat_name

        emitted = [False] * len(obj.faces)
        has_uvs = len(obj.uvs) == len(obj.verts)
        # Walk submeshes first (so faces with materials group together)…
        for sm in obj.submeshes:
            if not sm.face_indices:
                continue
            lines.append(f"usemtl {sm.mat_name}")
            for fi in sm.face_indices:
                if fi >= len(obj.faces) or emitted[fi]:
                    continue
                a, b, c = obj.faces[fi]
                if has_uvs:
                    lines.append(
                        f"f {a+v_base}/{a+vt_base} "
                        f"{b+v_base}/{b+vt_base} "
                        f"{c+v_base}/{c+vt_base}")
                else:
                    lines.append(f"f {a+v_base} {b+v_base} {c+v_base}")
                emitted[fi] = True
        # …then any orphan faces (no material assignment). Rare but legal.
        orphans = [i for i, e in enumerate(emitted) if not e]
        if orphans:
            lines.append("# orphan faces (no material)")
            for fi in orphans:
                a, b, c = obj.faces[fi]
                if has_uvs:
                    lines.append(
                        f"f {a+v_base}/{a+vt_base} "
                        f"{b+v_base}/{b+vt_base} "
                        f"{c+v_base}/{c+vt_base}")
                else:
                    lines.append(f"f {a+v_base} {b+v_base} {c+v_base}")
        v_base  += len(obj.verts)
        vt_base += len(obj.uvs)
        lines.append("")
    dst.write_text("\n".join(lines))


def write_materials_json(scene: ParsedScene, dst: Path, tex_prefix: str,
                         resolved: dict[str, str]) -> None:
    """Emit a materials.json sidecar matching the engine's loader format.

    `resolved` maps a raw 3DS texture filename → final PNG basename relative
    to `dst.parent`. Slots whose source BMP couldn't be found get omitted
    (the engine substitutes a 1×1 fallback at draw time)."""
    entries = []
    for m in scene.materials:
        e: dict[str, object] = {"name": m.name}
        if m.diffuse and m.diffuse in resolved:
            e["diffuse"] = resolved[m.diffuse]
        if m.spec    and m.spec    in resolved:
            e["spec"]    = resolved[m.spec]
        if m.glow    and m.glow    in resolved:
            e["glow"]    = resolved[m.glow]
        if m.bump    and m.bump    in resolved:
            e["normal"]  = resolved[m.bump]
        if m.color_rgb and "diffuse" not in e:
            # No diffuse map; preserve the flat color as a hint for any
            # later pipeline step. The engine doesn't read this yet but
            # it's cheap to capture now and useful for ship-tint editing.
            e["color"] = [round(c, 4) for c in m.color_rgb]
        entries.append(e)
    dst.write_text(json.dumps({"materials": entries}, indent=2) + "\n")


# ───────────────────────────── orchestration ────────────────────────────────


def discover_ships() -> list[Path]:
    """Every direct subdir of `assets/meshes/ships_raw/` that contains a
    `.3ds` file. The `_zips/` cache dir is skipped."""
    if not SRC_DIR.exists():
        return []
    out = []
    for d in sorted(SRC_DIR.iterdir()):
        if not d.is_dir() or d.name.startswith("_"):
            continue
        if any(p.suffix.lower() == ".3ds" for p in d.iterdir()):
            out.append(d)
    return out


def _recenter_scene(scene: ParsedScene) -> tuple[float, float, float]:
    """Translate every vertex so the scene's BOUNDING-BOX CENTRE sits at
    the local origin (0, 0, 0). Returns the offset that was subtracted,
    for diagnostics.

    Why bbox (vs vertex-mean or area-weighted):
        We've tried all three:
            * bbox centre        → midpoint of extents per axis
            * vertex-mean        → biased toward dense vertex clusters
            * area-weighted cent → true geometric SURFACE centre

        The latter two give the math-correct "centre of mass" of the
        surface, but for the kind of hard-edged spaceship meshes the
        wcnews archive ships — long thin noses, fat detailed engine
        pods, big flat wing panels — those centres land at the
        VERTEX/AREA-dense rear of the ship and the silhouette ends up
        floating upward in every render (orbit pivot at the tail).
        Centurion exhibited this most dramatically — engines are heavy
        with detail, nose is smooth, area-centroid sat at the engines.

        Bbox centre is the midpoint of the longest spatial extent on
        each axis. For a tapered fighter it lands roughly where the
        eye would put the visual centre regardless of how detailed each
        end is, because what eyes track is silhouette extent — not
        polygon density. Stuck with that.

        Outliers (e.g. a ship with a tiny nav-cone sticking 10 m out
        the front) can be handled with a per-ship offset override in
        the future if needed; for the current 17-ship roster bbox is
        empirically correct everywhere.
    """
    if not scene.objects:
        return (0.0, 0.0, 0.0)
    xs = [v[0] for obj in scene.objects for v in obj.verts]
    ys = [v[1] for obj in scene.objects for v in obj.verts]
    zs = [v[2] for obj in scene.objects for v in obj.verts]
    if not xs:
        return (0.0, 0.0, 0.0)
    cx = (min(xs) + max(xs)) * 0.5
    cy = (min(ys) + max(ys)) * 0.5
    cz = (min(zs) + max(zs)) * 0.5
    for obj in scene.objects:
        obj.verts = [(x - cx, y - cy, z - cz) for (x, y, z) in obj.verts]
    return (cx, cy, cz)


def convert_one(ship_dir: Path, dst_dir: Path) -> tuple[bool, str]:
    """Returns (ok, summary). Output files land directly in `dst_dir`."""
    threed = next((p for p in ship_dir.iterdir()
                   if p.suffix.lower() == ".3ds"), None)
    if threed is None:
        return False, "no .3ds in dir"

    try:
        scene = parse_3ds(threed)
    except (ValueError, struct.error) as exc:
        return False, f"parse failed: {exc}"

    if not scene.objects:
        return False, "no usable geometry"

    # Recentre BEFORE we write the .obj — bake the translation into the
    # vertex data so every downstream consumer gets a centred mesh
    # without having to know anything about per-ship offsets.
    offset = _recenter_scene(scene)

    stem = ship_dir.name
    dst_dir.mkdir(parents=True, exist_ok=True)

    # Apply curated MATERIAL_OVERRIDES, if any. Replaces the raw .3ds
    # diffuse RGB so the tint-PNG bake (further down) picks up the
    # corrected colour automatically.
    n_overridden = 0
    for m in scene.materials:
        key = (stem, m.name)
        if key in MATERIAL_OVERRIDES:
            m.color_rgb = MATERIAL_OVERRIDES[key]
            n_overridden += 1

    # Pass 1: convert every referenced BMP into a PNG with a ship-prefixed name.
    resolved: dict[str, str] = {}
    tex_misses = 0
    for m in scene.materials:
        for raw in (m.diffuse, m.spec, m.glow, m.bump):
            if not raw or raw in resolved:
                continue
            safe = _sanitize_texname(raw)
            out_name = f"{stem}_{safe}.png"
            if _convert_bmp(ship_dir, raw, dst_dir / out_name):
                resolved[raw] = out_name
            else:
                tex_misses += 1

    # Pass 2: bake flat-color materials into 1×1 tint PNGs. The engine's
    # materials loader only understands a `diffuse: <path>` slot — there's
    # no per-material color field in obj_loader.cpp — so without this every
    # untextured material would fall back to the renderer's white 1×1 image
    # and look identical / blown-out. By emitting a tint PNG per coloured
    # material we get the artist-intended hull tones for free, with zero
    # engine changes. The PNGs are tiny (~70 B each) and live next to the OBJ.
    tints_baked = 0
    for m in scene.materials:
        if m.color_rgb is None or m.diffuse:
            continue
        safe_name = "".join(c if c.isalnum() else "_" for c in m.name).lower()
        tint_path = dst_dir / f"{stem}_tint_{safe_name}.png"
        _write_tint_png(m.color_rgb, tint_path)
        # Synthesise a key into `resolved` so write_materials_json picks it up.
        # The key is namespaced so it can't collide with any real BMP filename.
        synthetic_key = f"__tint__/{m.name}"
        m.diffuse = synthetic_key
        resolved[synthetic_key] = tint_path.name
        tints_baked += 1

    write_obj(scene, dst_dir / f"{stem}.obj")
    write_materials_json(scene, dst_dir / f"{stem}.materials.json",
                         tex_prefix=stem, resolved=resolved)

    n_tris  = sum(len(o.faces) for o in scene.objects)
    n_verts = sum(len(o.verts) for o in scene.objects)
    extra = []
    if tints_baked:
        extra.append(f"+{tints_baked} tint")
    if n_overridden:
        extra.append(f"{n_overridden} overridden")
    if tex_misses:
        extra.append(f"{tex_misses} missing")
    if any(abs(v) > 0.5 for v in offset):
        extra.append(f"centred [{offset[0]:.0f}, {offset[1]:.0f}, {offset[2]:.0f}]")
    suffix = f"  ({', '.join(extra)})" if extra else ""
    return True, (f"{n_verts}v / {n_tris}t  "
                  f"objs={len(scene.objects)}  mats={len(scene.materials)}  "
                  f"texs={len(resolved) - tints_baked}{suffix}")


def main() -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--only", nargs="*", default=None,
                   help="explicit ship dir names (e.g. 'demon dralthi')")
    p.add_argument("--list", dest="list_only", action="store_true",
                   help="print the conversion plan and exit")
    args = p.parse_args()

    ships = discover_ships()
    if args.only:
        wanted = set(args.only)
        ships  = [s for s in ships if s.name in wanted]
        missing = wanted - {s.name for s in ships}
        if missing:
            print(f"warning: not found in ships_raw/: {sorted(missing)}",
                  file=sys.stderr)
    if not ships:
        print("nothing to do — no source dirs under "
              f"{SRC_DIR.relative_to(REPO)}", file=sys.stderr)
        return 0

    print(f"plan: convert {len(ships)} ship(s) → {DST_DIR.relative_to(REPO)}/")
    if args.list_only:
        for s in ships:
            print(f"  PLAN  {s.name}")
        return 0

    fails = 0
    for i, s in enumerate(ships, 1):
        ok, msg = convert_one(s, DST_DIR)
        mark = "ok  " if ok else "FAIL"
        print(f"  [{i:3d}/{len(ships)}] {mark} {s.name:<22} {msg}")
        if not ok:
            fails += 1
    print(f"\ndone — {len(ships) - fails} converted, {fails} failed")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

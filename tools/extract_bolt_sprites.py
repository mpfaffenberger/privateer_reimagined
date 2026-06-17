#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# extract_bolt_sprites.py — decode ORIGINAL Privateer gun BOLT sprites from
# DATA/APPEARNC/<GUN>.IFF to PNG.
#
# The prior investigation (bead np-52d) concluded "no per-gun bolt sprites
# exist" — but it only looked at TYPES/*TYPE.IFF (gameplay data) and
# OPTIONS/SHIPART.IFF (gun hardware models). It missed DATA/APPEARNC/, which
# holds a per-gun appearance file (MASS.IFF, PLASMA.IFF, TACHYON.IFF, ...) each
# containing a FORM APPR > FORM BMAP > SHAP sprite set. These ARE the bolt
# sprites. LASER.IFF has no SHAP (procedural bolt); the other 8 guns decode.
#
# Code-only decoder (same legal model as extract_gun_sprites.py). Reads the
# user's own GOG-extracted data; pixels are Origin/EA copyrighted, never
# committed (output dir is gitignored under gog_extracted/).
#
# Usage:
#   python3 tools/extract_bolt_sprites.py [--root gog_extracted/extracted/priv]
#                                         [--out  gog_extracted/bolt_sprites]
# -----------------------------------------------------------------------------
import argparse
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("[bolt-sprites] needs Pillow: pip install pillow")

# Reuse the proven SHP decoder + IFF reader + palette loader.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_gun_sprites import Reader, load_palette, decode_shape

# Per-gun APPEARNC files. Laser has no SHAP (procedural); we still list it so
# the output set is complete (zero frames, logged).
GUN_FILES = [
    ("neutron",   "NEUTRON.IFF"),
    ("meson",     "MESON.IFF"),
    ("ionic",     "IONIC.IFF"),
    ("mass",      "MASS.IFF"),
    ("particle",  "PARTICLE.IFF"),
    ("laser",     "LASER.IFF"),
    ("plasma",    "PLASMA.IFF"),
    ("tachyon",   "TACHYON.IFF"),
    # WEAPONS.IFF is intentionally excluded: it is the "weapons" CARGO
    # COMMODITY sprite (jettisoned box art, has a MFDS/TARG reticle chunk),
    # NOT a gun bolt. Same-name collision with the gun family.
]


def find_shap(b):
    """Locate the SHAP chunk inside FORM APPR > FORM BMAP > SHAP.
    Returns (data_start, size) or (None, None) if absent."""
    # Walk top-level FORM APPR
    if b[0:4] != b"FORM" or b[8:12] != b"APPR":
        return None, None
    # Inner FORM BMAP
    p = 12
    if b[p:p+4] != b"FORM" or b[p+8:p+12] != b"BMAP":
        return None, None
    bmap_start = p + 12
    bmap_end = p + 8 + struct.unpack(">I", b[p+4:p+8])[0]
    # Walk chunks inside BMAP until SHAP
    cp = bmap_start
    while cp + 8 <= bmap_end:
        tag = b[cp:cp+4]
        size = struct.unpack(">I", b[cp+4:cp+8])[0]
        if tag == b"SHAP":
            return cp + 8, size
        cp += 8 + size + (size & 1)
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="gog_extracted/extracted/priv")
    ap.add_argument("--out", default="gog_extracted/bolt_sprites")
    ap.add_argument("--palette", default=None,
                    help="override palette (default: DATA/PALETTE/SPACE.PAL)")
    args = ap.parse_args()

    appear = os.path.join(args.root, "DATA", "APPEARNC")
    pal_path = args.palette or os.path.join(args.root, "DATA", "PALETTE", "SPACE.PAL")
    pal = load_palette(pal_path)
    os.makedirs(args.out, exist_ok=True)

    total = 0
    for gun, fname in GUN_FILES:
        path = os.path.join(appear, fname)
        if not os.path.exists(path):
            print(f"[bolt-sprites] MISSING {fname}, skipping")
            continue
        b = open(path, "rb").read()
        shap_start, shap_size = find_shap(b)
        if shap_start is None:
            print(f"[bolt-sprites] {gun:9s} ({len(b):5d} B) — no SHAP (procedural)")
            continue
        r = Reader(b)
        r.pos = shap_start
        try:
            frames = decode_shape(r)
        except Exception as e:
            print(f"[bolt-sprites] {gun:9s} decode FAILED: {e}")
            continue
        for fi, fr in enumerate(frames):
            img = fr.to_rgba(pal)
            name = f"{gun}_{fi:02d}.png"
            img.save(os.path.join(args.out, name))
            total += 1
            print(f"[bolt-sprites] {gun:9s} frame {fi}/{len(frames)} "
                  f"{fr.w}x{fr.h} -> {name}")

    print(f"\n[bolt-sprites] wrote {total} PNGs to {args.out}")


if __name__ == "__main__":
    main()

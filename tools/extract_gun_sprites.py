#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# extract_gun_sprites.py — decode the ORIGINAL Privateer gun bolt SHAPES to PNG.
#
# Legal model (same as the SFX, see tools/extract_soundfx_pak.py): this is a
# CODE-ONLY decoder. It reads the user's OWN GOG-extracted Privateer data and
# writes RGBA PNGs to a GITIGNORED directory (gog_extracted/ is already ignored).
# The extracted pixels are Origin/EA copyrighted and are NEVER committed; only
# this script is.
#
# Format authority: dpjudas/WCPrivateer (clean-room reimplementation). This file
# mirrors three of its decoders verbatim in spirit:
#   * Sources/FileFormat/WCImage.cpp    — the SHP/SHAPES frame decoder
#                                         (segment + run + RLE, with a 1-bit
#                                          transparency mask).
#   * Sources/FileFormat/WCPalette.cpp  — the VGA palette (6-bit RGB, <<2).
#   * Sources/FileFormat/WCGameData.cpp::LoadShipArt — locates the per-gun bolt
#                                         shapes inside DATA\OPTIONS\SHIPART.IFF
#                                         (FORM SHAR -> FORM <class> -> FORM GUNS
#                                          -> FORM GUNT -> INFO(u8)+DESC+SHAP).
#
# The gun bolt graphics live in SHIPART.IFF, NOT in the tiny *TYPE.IFF projectile
# records (those only hold a 4-byte PROJ/INFO = [shape-id, speed]); the actual
# pixels are inline SHAP images keyed by a gun index. We export every GUNT shape
# and map the gun index back to a GunType via GUNS.IFF unit order.
#
# Usage:
#   python3 tools/extract_gun_sprites.py [--root gog_extracted/extracted/priv]
#                                        [--out  gog_extracted/gun_sprites]
# -----------------------------------------------------------------------------

import argparse
import os
import struct
import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    sys.exit("[gun-sprites] needs Pillow: pip install pillow")


# ----------------------------------------------------------------------------
# Reader — mirrors dpjudas FileEntryReader (little-endian data, big-endian IFF
# chunk sizes, 16-bit chunk alignment on pop).
# ----------------------------------------------------------------------------
class Reader:
    def __init__(self, data: bytes):
        self.b = data
        self.pos = 0
        self.chunks = []  # list of (start, size)

    def u8(self):
        if self.pos >= len(self.b):
            return 0
        v = self.b[self.pos]
        self.pos += 1
        return v

    def u16(self):
        return self.u8() | (self.u8() << 8)

    def i16(self):
        v = self.u16()
        return v - 0x10000 if v & 0x8000 else v

    def u24(self):
        return self.u8() | (self.u8() << 8) | (self.u8() << 16)

    def u32(self):
        return self.u8() | (self.u8() << 8) | (self.u8() << 16) | (self.u8() << 24)

    def u32be(self):
        v0, v1, v2, v3 = self.u8(), self.u8(), self.u8(), self.u8()
        return (v0 << 24) | (v1 << 16) | (v2 << 8) | v3

    def tag(self):
        t = self.b[self.pos:self.pos + 4].decode("latin1")
        self.pos += 4
        return t

    def read(self, n):
        d = self.b[self.pos:self.pos + n]
        self.pos += n
        return d

    def push(self, expect=None):
        t = self.tag()
        if expect is not None and t != expect:
            raise ValueError(f"expected chunk {expect!r} got {t!r} @0x{self.pos-4:x}")
        size = self.u32be()
        self.chunks.append((self.pos, size))
        return t

    def chunk_size(self):
        return self.chunks[-1][1]

    def end_of_chunk(self):
        start, size = self.chunks[-1]
        return self.pos - start == size

    def pop(self, must_read_all=True):
        start, size = self.chunks.pop()
        # Chunks are 16-bit aligned.
        self.pos = start + size + (size & 1)


# ----------------------------------------------------------------------------
# Palette — mirrors WCPalette::ReadPalette. 6-bit VGA RGB scaled <<2.
# Returns list of 256 (r,g,b) and a parallel "is index 0" note; transparency is
# handled by the image mask, not by the palette.
# ----------------------------------------------------------------------------
def load_palette(path):
    r = Reader(open(path, "rb").read())
    start = r.u16()
    count = r.u16()
    pal = [(0, 0, 0)] * 256
    for i in range(start, start + count):
        red = r.u8() << 2
        green = r.u8() << 2
        blue = r.u8() << 2
        pal[i] = (red, green, blue)
    return pal


# ----------------------------------------------------------------------------
# Frame — one decoded SHP image: indexed pixels + 1-bit mask (255 = opaque).
# ----------------------------------------------------------------------------
class Frame:
    __slots__ = ("x", "y", "w", "h", "pixels", "mask")

    def to_rgba(self, pal):
        img = Image.new("RGBA", (self.w, self.h), (0, 0, 0, 0))
        px = img.load()
        for yy in range(self.h):
            row = yy * self.w
            for xx in range(self.w):
                i = row + xx
                if self.mask[i]:
                    r, g, b = pal[self.pixels[i]]
                    px[xx, yy] = (r, g, b, 255)
        return img


# ----------------------------------------------------------------------------
# decode_shape — mirrors WCImage(FileEntryReader&). Reads a multi-frame SHP
# blob starting at the reader's current position. Returns list[Frame].
# ----------------------------------------------------------------------------
def decode_shape(r: Reader):
    baseoffset = r.pos
    sectionsize = r.u32()
    firstoffset = r.u24()
    r.u8()  # unknown
    offsets = [baseoffset + firstoffset]

    count = (firstoffset - 4) // 4
    if count == 0:
        raise ValueError("invalid image header (count==0)")

    for _ in range(1, count):
        offset = r.u24()
        r.u8()  # unknown
        if offset < firstoffset or offset >= sectionsize:
            break
        offsets.append(baseoffset + offset)

    frames = []
    for off in offsets:
        r.pos = off
        ext_right = r.i16()
        ext_left = r.i16()
        ext_top = r.i16()
        ext_bottom = r.i16()

        minx = -ext_left
        miny = -ext_top
        maxx = ext_right + 1
        maxy = ext_bottom + 1
        w = maxx - minx
        h = maxy - miny
        if w <= 0 or h <= 0 or w > 500 or h > 500:
            raise ValueError(f"invalid image header (w={w} h={h})")

        image = bytearray(w * h)
        mask = bytearray(w * h)

        while True:
            seg_width = r.u16()
            if seg_width == 0:
                break
            seg_is_run = (seg_width & 1) != 0
            seg_width >>= 1

            x = r.i16()
            y = r.i16()
            if x < minx or x + seg_width > maxx or y < miny or y >= maxy:
                raise ValueError("invalid image data (segment OOB)")

            dest = (x - minx) + (y - miny) * w

            if seg_is_run:
                while seg_width > 0:
                    run_width = r.u8()
                    run_is_rle = (run_width & 1) != 0
                    run_width >>= 1
                    if run_width > seg_width:
                        raise ValueError("invalid image data (run OOB)")
                    if run_is_rle:
                        color = r.u8()
                        for _ in range(run_width):
                            image[dest] = color
                            mask[dest] = 255
                            dest += 1
                    else:
                        for _ in range(run_width):
                            image[dest] = r.u8()
                            mask[dest] = 255
                            dest += 1
                    seg_width -= run_width
            else:
                for _ in range(seg_width):
                    image[dest] = r.u8()
                    mask[dest] = 255
                    dest += 1

        f = Frame()
        f.x, f.y, f.w, f.h = minx, miny, w, h
        f.pixels = image
        f.mask = mask
        frames.append(f)

    r.pos = baseoffset + sectionsize
    return frames


# ----------------------------------------------------------------------------
# parse_ship_art — mirrors WCGameData::LoadShipArt, collecting GUNT/SCAT shapes.
# Yields dicts: {class, kind, info, frames}.
# ----------------------------------------------------------------------------
def parse_ship_art(path):
    r = Reader(open(path, "rb").read())
    r.push("FORM")
    r.tag()  # SHAR
    out = []
    while not r.end_of_chunk():
        r.push("FORM")
        cls = r.tag()  # e.g. FIGH
        while not r.end_of_chunk():
            r.push("FORM")
            tag2 = r.tag()  # APPR | GUNS | MISC
            if tag2 == "APPR":
                r.push("SHAP")
                decode_shape(r)  # ship appearance; skip
                r.pop()
            elif tag2 in ("GUNS", "MISC"):
                while not r.end_of_chunk():
                    r.push("FORM")
                    tag3 = r.tag()  # GUNT | SCAT
                    r.push("INFO")
                    info = r.u8()
                    r.pop()
                    r.push("DESC")
                    r.read(r.chunk_size())
                    r.pop()
                    r.push("SHAP")
                    frames = decode_shape(r)
                    r.pop()
                    out.append({"class": cls, "kind": tag3, "info": info, "frames": frames})
                    r.pop()  # FORM GUNT/SCAT
            else:
                # unknown sub-form: skip its bytes
                r.pop(False)
                continue
            r.pop()  # FORM tag2
        r.pop()  # FORM class
    r.pop()  # FORM SHAR
    return out


# ----------------------------------------------------------------------------
# GUNS.IFF unit order -> gun short label. The GUNT `info` index references this
# table (the in-game gun id). Parsed straight from the TABL/UNIT records.
# ----------------------------------------------------------------------------
def parse_guns_order(path):
    r = Reader(open(path, "rb").read())
    r.push("FORM")
    r.tag()  # GUNS
    r.push("TABL")
    n = r.chunk_size() // 4
    offsets = [r.u32() for _ in range(n)]
    r.pop()
    r.pop(False)
    order = []
    for off in offsets:
        if off == 0:
            break
        r.pos = off
        r.u32be()  # block size
        r.push("UNIT")
        short = r.read(5).split(b"\0")[0].decode("latin1")
        typ = r.read(8).split(b"\0")[0].decode("latin1")   # e.g. NTRNTYPE
        longn = r.read(8).split(b"\0")[0].decode("latin1")  # e.g. NEUTRON
        order.append({"short": short, "type": typ, "long": longn})
        r.pop()
    return order


def dominant_color(img):
    counts = Counter()
    for r, g, b, a in img.get_flattened_data():
        if a:
            counts[(r, g, b)] += 1
    if not counts:
        return (0, 0, 0)
    return counts.most_common(1)[0][0]


def make_contact_sheet(arts, guns, pal, out_path, cell=72, pad=6):
    """Montage of the 9 player GunType hardware shapes (FIGH/GUNT, info 0..8),
    one row per gun, frames across — cropped to content on a checker bg so the
    alpha reads clearly. Eyeball-only preview; gitignored like the PNGs."""
    rows = []
    for info in range(9):
        match = [a for a in arts if a["class"] == "FIGH" and a["kind"] == "GUNT"
                 and a["info"] == info]
        if not match:
            continue
        rows.append((info, match[0]["frames"]))
    if not rows:
        return None
    ncols = max(len(fr) for _, fr in rows)
    W = pad + (cell + pad) * ncols
    H = pad + (cell + pad) * len(rows)
    sheet = Image.new("RGBA", (W, H), (32, 32, 40, 255))
    for ri, (info, frames) in enumerate(rows):
        for ci, fr in enumerate(frames):
            img = fr.to_rgba(pal)
            bb = img.getbbox()
            if bb:
                img = img.crop(bb)
            img.thumbnail((cell, cell), Image.NEAREST)
            x = pad + ci * (cell + pad) + (cell - img.width) // 2
            y = pad + ri * (cell + pad) + (cell - img.height) // 2
            sheet.alpha_composite(img, (x, y))
    sheet.save(out_path)
    return out_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="gog_extracted/extracted/priv")
    ap.add_argument("--out", default="gog_extracted/gun_sprites")
    args = ap.parse_args()

    root = args.root
    pal_path = os.path.join(root, "DATA", "PALETTE", "SPACE.PAL")
    shipart = os.path.join(root, "DATA", "OPTIONS", "SHIPART.IFF")
    guns_iff = os.path.join(root, "DATA", "TYPES", "GUNS.IFF")

    pal = load_palette(pal_path)
    guns = parse_guns_order(guns_iff)
    arts = parse_ship_art(shipart)

    os.makedirs(args.out, exist_ok=True)

    print(f"[gun-sprites] GUNS.IFF order ({len(guns)} units):")
    for i, g in enumerate(guns):
        print(f"   {i:2d}  {g['short']:6s} {g['type']:9s} {g['long']}")

    print(f"\n[gun-sprites] SHIPART.IFF gun/misc shapes ({len(arts)}):")
    manifest = []
    for a in arts:
        info = a["info"]
        guninfo = guns[info] if info < len(guns) else {"short": f"idx{info}", "long": "?"}
        base = f"{a['class']}_{a['kind']}_{info:02d}_{guninfo['short'].lower()}"
        for fi, fr in enumerate(a["frames"]):
            img = fr.to_rgba(pal)
            name = f"{base}_{fi:02d}.png"
            img.save(os.path.join(args.out, name))
            dc = dominant_color(img)
            manifest.append((a["class"], a["kind"], info, guninfo["short"],
                             name, fr.w, fr.h, len(a["frames"]), dc))
            print(f"   {a['class']} {a['kind']} info={info:2d} "
                  f"({guninfo['short']:6s}) frame {fi}/{len(a['frames'])} "
                  f"{fr.w}x{fr.h} dom={dc} -> {name}")

    # Manifest (CSV) next to the PNGs — also gitignored (lives under out dir).
    with open(os.path.join(args.out, "manifest.csv"), "w") as fh:
        fh.write("class,kind,info,gun,png,w,h,frames,dom_r,dom_g,dom_b\n")
        for cls, kind, info, short, name, w, h, nf, dc in manifest:
            fh.write(f"{cls},{kind},{info},{short},{name},{w},{h},{nf},"
                     f"{dc[0]},{dc[1]},{dc[2]}\n")

    sheet = make_contact_sheet(arts, guns, pal,
                               os.path.join(args.out, "_contact_sheet.png"))
    if sheet:
        print(f"\n[gun-sprites] contact sheet -> {sheet}")
    print(f"[gun-sprites] wrote {len(manifest)} PNGs + manifest.csv to {args.out}")
    return manifest, args.out


if __name__ == "__main__":
    main()

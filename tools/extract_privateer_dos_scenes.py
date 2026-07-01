#!/usr/bin/env python3
"""extract_privateer_dos_scenes.py — decode the ACTUAL 1996 DOS Privateer
base-interior scenes (Bar, Commodity Exchange, Ship Dealer, both Guilds)
straight out of the GOG release's game data, bypassing privateer_wcu's
fan-made (and DXT1-mushy) reinterpretation entirely.

Format reverse-engineered from dpjudas/WCPrivateer (an active from-scratch
Privateer engine recreation) — Sources/FileFormat/{WCPak,WCImage,WCPalette,
WCScene}.cpp + Sources/Game/Screens/Scene/SceneScreen.cpp. Summary:

  DATA/OPTIONS/OPTIONS.IFF   FORM OPTS > TABL (u32 LE offsets) -> each
                             offset is a FORM SCEN: REGN/RECT (clickable
                             zones + labels), BACK (background shape refs),
                             FORE (foreground sprite refs — NPCs etc).
  DATA/OPTIONS/OPTPALS.PAK   WCPak of WCPalette blobs (start/count + RGB666).
  DATA/OPTIONS/OPTSHPS.PAK   WCPak of WCImage/"SHP" blobs — each is a small
                             chunk-table of RLE-encoded, palette-indexed
                             frames (run-length "segments", same convention
                             VPK/PAK animation frames in this codebase use).
                             BACK/FORE shapes reference these by index
                             (optpakIndex) and place them at (offsetX,offsetY)
                             to compose the final screen.

Everything here is a straight, careful port of that C++ — see WCPak/WCImage/
WCPalette/WCSceneList below. No AI, no upscaling: this is literally what the
DOS game had on disk, decoded byte-for-byte.

Usage:
    tools/extract_privateer_dos_scenes.py --dump-all-scenes /tmp/dos_scenes
    tools/extract_privateer_dos_scenes.py --scene 12 -o /tmp/scene12.png
"""
import argparse
import struct
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parent.parent
GOG = REPO / "gog_extracted" / "extracted" / "priv" / "DATA"


# ---- low-level reader (mirrors FileEntryReader.h) --------------------------

class Reader:
    def __init__(self, buf: bytes):
        self.buf = buf
        self.pos = 0
        self.chunks: list[tuple[int, int]] = []  # (data_start, chunksize)

    def tell(self) -> int:
        return self.pos

    def size(self) -> int:
        return len(self.buf)

    def seek(self, p: int) -> None:
        self.pos = p

    def u8(self) -> int:
        v = self.buf[self.pos] if self.pos < len(self.buf) else 0
        self.pos += 1
        return v

    def bytes_(self, n: int) -> bytes:
        v = self.buf[self.pos:self.pos + n]
        self.pos += n
        return v

    def u16(self) -> int:
        return struct.unpack_from("<H", self.buf, self.pos)[0] if self._advance(2) else 0

    def _advance(self, n):
        ok = self.pos + n <= len(self.buf)
        self.pos += n
        return ok

    # simpler explicit readers (no cleverness, matches the C++ 1:1)
    def read_u8(self) -> int:
        return self.u8()

    def read_u16(self) -> int:
        v0, v1 = self.read_u8(), self.read_u8()
        return (v1 << 8) | v0

    def read_i16(self) -> int:
        v = self.read_u16()
        return v - 0x10000 if v >= 0x8000 else v

    def read_u24(self) -> int:
        v0, v1, v2 = self.read_u8(), self.read_u8(), self.read_u8()
        return (v2 << 16) | (v1 << 8) | v0

    def read_u32(self) -> int:
        v0, v1, v2, v3 = self.read_u8(), self.read_u8(), self.read_u8(), self.read_u8()
        return (v3 << 24) | (v2 << 16) | (v1 << 8) | v0

    def read_u32be(self) -> int:
        v0, v1, v2, v3 = self.read_u8(), self.read_u8(), self.read_u8(), self.read_u8()
        return (v0 << 24) | (v1 << 16) | (v2 << 8) | v3

    def read_tag(self, expect: str | None = None) -> str:
        tag = self.read_bytes(4).decode("latin1")
        if expect is not None and tag != expect:
            raise ValueError(f"expected tag {expect!r}, got {tag!r} @0x{self.pos-4:x}")
        return tag

    def read_bytes(self, n: int) -> bytes:
        v = self.buf[self.pos:self.pos + n]
        self.pos += n
        return v

    # chunk stack (IFF-style, 16-bit aligned) --------------------------------
    def push_chunk(self, expect: str | None = None) -> str:
        tag = self.read_tag(expect)
        chunksize = self.read_u32be()
        self.chunks.append((self.tell(), chunksize))
        return tag

    def chunk_size(self) -> int:
        return self.chunks[-1][1]

    def is_end_of_chunk(self) -> bool:
        start, size = self.chunks[-1]
        return self.tell() - start == size

    def pop_chunk(self, must_read_all: bool = True) -> None:
        start, size = self.chunks[-1]
        if must_read_all and not self.is_end_of_chunk():
            raise ValueError(f"unexpected end of chunk (@{self.tell()}, expected @{start+size})")
        self.seek(start + size + (size & 1))
        self.chunks.pop()


# ---- WCPak: an offset-table container of sub-files -------------------------

def decode_pak(buf: bytes) -> list[bytes]:
    r = Reader(buf)
    paksize = r.read_u32()
    if paksize != len(buf):
        raise ValueError(f"not a WCPak (size field {paksize} != file size {len(buf)})")
    firstoffset = r.read_u24()
    r.read_u8()  # unknown flag byte
    offsets = [firstoffset]
    count = (firstoffset - 4) // 4
    for _ in range(1, count):
        offset = r.read_u24()
        r.read_u8()
        if offset < firstoffset or offset >= paksize:
            break
        offsets.append(offset)
    offsets.append(paksize)
    return [buf[offsets[i]:offsets[i + 1]] for i in range(len(offsets) - 1)]


# ---- WCImage / "SHP": RLE-encoded palette-indexed frame(s) -----------------

class ShpFrame:
    def __init__(self, x, y, w, h, pixels, mask):
        self.x, self.y, self.w, self.h = x, y, w, h
        self.pixels, self.mask = pixels, mask


def decode_shp(buf: bytes) -> list[ShpFrame]:
    r = Reader(buf)
    baseoffset = r.tell()
    sectionsize = r.read_u32()
    firstoffset = r.read_u24()
    r.read_u8()
    offsets = [baseoffset + firstoffset]
    count = (firstoffset - 4) // 4
    if count == 0:
        raise ValueError("invalid SHP header")
    for _ in range(1, count):
        offset = r.read_u24()
        r.read_u8()
        if offset < firstoffset or offset >= sectionsize:
            break
        offsets.append(baseoffset + offset)

    frames = []
    for off in offsets:
        r.seek(off)
        extentsRight = r.read_i16()
        extentsLeft = r.read_i16()
        extentsTop = r.read_i16()
        extentsBottom = r.read_i16()
        minx, miny = -extentsLeft, -extentsTop
        maxx, maxy = extentsRight + 1, extentsBottom + 1
        w, h = maxx - minx, maxy - miny
        if w <= 0 or h <= 0 or w > 500 or h > 500:
            raise ValueError("invalid SHP frame header")

        image = bytearray(w * h)
        mask = bytearray(w * h)

        while True:
            segWidth = r.read_u16()
            if segWidth == 0:
                break
            segIsRun = (segWidth & 1) != 0
            segWidth >>= 1
            x, y = r.read_i16(), r.read_i16()
            if x < minx or x + segWidth > maxx or y < miny or y >= maxy:
                raise ValueError("SHP segment out of bounds")
            di = (x - minx) + (y - miny) * w

            if segIsRun:
                remaining = segWidth
                while remaining > 0:
                    runWidth = r.read_u8()
                    runIsRLE = (runWidth & 1) != 0
                    runWidth >>= 1
                    if runWidth > remaining:
                        raise ValueError("SHP run out of bounds")
                    if runIsRLE:
                        color = r.read_u8()
                        for i in range(runWidth):
                            image[di + i] = color
                            mask[di + i] = 255
                    else:
                        for i in range(runWidth):
                            image[di + i] = r.read_u8()
                            mask[di + i] = 255
                    di += runWidth
                    remaining -= runWidth
            else:
                for i in range(segWidth):
                    image[di + i] = r.read_u8()
                    mask[di + i] = 255

        frames.append(ShpFrame(minx, miny, w, h, bytes(image), bytes(mask)))

    return frames


# ---- WCPalette: 256-colour RGB666 palette ----------------------------------

def decode_palette(buf: bytes) -> list[tuple[int, int, int, int]]:
    r = Reader(buf)
    start = r.read_u16()
    count = r.read_u16()
    if start + count > 256:
        raise ValueError("invalid palette")
    pal = [(0, 0, 0, 0)] * 256
    for i in range(start, start + count):
        red = r.read_u8() << 2
        green = r.read_u8() << 2
        blue = r.read_u8() << 2
        pal[i] = (red, green, blue, 255)
    return pal


# ---- OPTIONS.IFF: the scene list -------------------------------------------

class Region:
    def __init__(self):
        self.target = -1
        self.label = ""
        self.coords: list[tuple[int, int]] = []


class BgShape:
    def __init__(self):
        self.unknown16 = 0
        self.optpak_index = 0
        self.offset_x = 0
        self.offset_y = 0


class FgSprite:
    def __init__(self):
        self.target = -1
        self.optpak_index = 0
        self.label = ""
        self.x1 = self.y1 = self.x2 = self.y2 = 0


class Scene:
    def __init__(self):
        self.regions: list[Region] = []
        self.bg_palette = 0
        self.bg_shapes: list[BgShape] = []
        self.fg_palette = 0
        self.fg_sprites: list[FgSprite] = []


def _read_region(r: Reader, is_rect: bool) -> Region:
    region = Region()
    while not r.is_end_of_chunk():
        tag = r.push_chunk()
        if tag == "INFO":
            region.target = r.read_u8()
        elif tag == "LABL":
            text = r.read_bytes(r.chunk_size())
            region.label = text.split(b"\0", 1)[0].decode("latin1", "replace")
        elif tag == "CORD":
            if is_rect:
                left, top, right, bottom = (r.read_i16() for _ in range(4))
                region.coords = [(left, top), (right, top), (right, bottom), (left, bottom)]
            else:
                while not r.is_end_of_chunk():
                    region.coords.append((r.read_i16(), r.read_i16()))
        else:
            raise ValueError(f"unexpected REGN/RECT tag {tag!r}")
        r.pop_chunk()
    return region


def _read_background(r: Reader) -> tuple[int, list[BgShape]]:
    palette = 0
    shapes = []
    while not r.is_end_of_chunk():
        tag = r.push_chunk()
        if tag == "PALT":
            palette = r.read_u16()
        elif tag == "SHAP":
            s = BgShape()
            s.unknown16 = r.read_u16()
            s.optpak_index = r.read_u16()
            s.offset_x = r.read_u16()
            s.offset_y = r.read_u16()
            r.read_bytes(r.chunk_size() - 8)  # trailing "unknown" bytes (unused)
            shapes.append(s)
        else:
            raise ValueError(f"unexpected BACK tag {tag!r}")
        r.pop_chunk()
    return palette, shapes


def _read_foreground(r: Reader) -> tuple[int, list[FgSprite]]:
    palette = 0
    sprites = []
    while not r.is_end_of_chunk():
        tag = r.push_chunk()
        if tag == "PALT":
            palette = r.read_u16()
        elif tag == "FORM":
            r.read_tag("SPRT")
            sp = FgSprite()
            while not r.is_end_of_chunk():
                stag = r.push_chunk()
                if stag == "SHAP":
                    sp.target = r.read_u8()
                    sp.optpak_index = r.read_u8()
                    r.read_bytes(r.chunk_size() - 2)  # trailing "shape" bytes (unused)
                elif stag == "LABL":
                    text = r.read_bytes(r.chunk_size())
                    sp.label = text.split(b"\0", 1)[0].decode("latin1", "replace")
                elif stag == "RECT":
                    sp.x1, sp.y1, sp.x2, sp.y2 = (r.read_u16() for _ in range(4))
                elif stag in ("INFO", "SEQU", "REGN"):
                    r.read_bytes(r.chunk_size())  # not needed for a static export
                elif stag == "CLCK":
                    pass
                else:
                    raise ValueError(f"unexpected SPRT tag {stag!r}")
                r.pop_chunk()
            sprites.append(sp)
        else:
            raise ValueError(f"unexpected FORE tag {tag!r}")
        r.pop_chunk()
    return palette, sprites


def decode_options_iff(buf: bytes) -> list[Scene]:
    r = Reader(buf)
    r.push_chunk("FORM")
    r.read_tag("OPTS")
    r.push_chunk("TABL")
    table = [r.read_u32() for _ in range(r.chunk_size() // 4)]
    r.pop_chunk()
    r.pop_chunk(must_read_all=False)

    scenes = []
    for offset in table:
        if offset == 0:
            break
        r.seek(offset)
        r.read_u32be()  # redundant outer size, unused
        r.push_chunk("FORM")
        r.read_tag("SCEN")

        scene = Scene()
        while not r.is_end_of_chunk():
            r.push_chunk("FORM")
            tag = r.read_tag()
            if tag == "REGN":
                scene.regions.append(_read_region(r, is_rect=False))
            elif tag == "RECT":
                scene.regions.append(_read_region(r, is_rect=True))
            elif tag == "BACK":
                scene.bg_palette, scene.bg_shapes = _read_background(r)
            elif tag == "FORE":
                scene.fg_palette, scene.fg_sprites = _read_foreground(r)
            elif tag in ("SHIP", "LOOK"):
                r.read_bytes(r.chunk_size() - 4)  # not needed for room backgrounds (-4: tag already consumed)
            else:
                raise ValueError(f"unexpected SCEN tag {tag!r}")
            r.pop_chunk()
        r.pop_chunk()
        scenes.append(scene)
    return scenes


# ---- compositing ------------------------------------------------------------

def render_scene(scene: Scene, palettes: list[list], shape_files: list[bytes],
                  include_foreground: bool = True) -> Image.Image:
    """Composite one scene's background (+ optionally foreground sprites,
    frame 0 of each) into an RGBA canvas sized to fit everything."""
    pal = palettes[scene.bg_palette]

    placed = []  # (offset_x, offset_y, ShpFrame)
    for shape in scene.bg_shapes:
        try:
            frames = decode_shp(shape_files[shape.optpak_index])
        except Exception:
            continue
        if frames:
            placed.append((shape.offset_x, shape.offset_y, frames[0]))
    if include_foreground:
        for sprite in scene.fg_sprites:
            try:
                frames = decode_shp(shape_files[sprite.optpak_index])
            except Exception:
                continue
            if frames:
                placed.append((sprite.x1, sprite.y1, frames[0]))

    if not placed:
        return Image.new("RGBA", (1, 1))

    max_x = max(ox + f.x + f.w for ox, oy, f in placed)
    max_y = max(oy + f.y + f.h for ox, oy, f in placed)
    canvas = Image.new("RGBA", (max(max_x, 1), max(max_y, 1)), (0, 0, 0, 0))

    for ox, oy, f in placed:
        frame_im = Image.new("RGBA", (f.w, f.h))
        px = [pal[b][:3] + (255 if m else 0,) for b, m in zip(f.pixels, f.mask)]
        frame_im.putdata(px)
        canvas.alpha_composite(frame_im, (ox + f.x, oy + f.y))
    return canvas


# ---- build the "pleasure" archetype from DOS data --------------------------

# WCTarget (WCScene.h) -> base_screens BaseScreen token, for turning a DOS
# concourse scene's clickable regions into our normalized hub links.
DOS_TARGET_TO_SCREEN = {
    63: "CommodityExchange", 61: "MerchantsGuild", 200: "MissionComputer",
    201: "ShipDealer", 203: "MercenariesGuild", 206: "Bar",
    207: "LandingPad", 208: "Launch", 202: "Concourse",
}

# Pleasure-world concourse: the neon casino hub (scene 42 has the full set of
# service regions; 39-42 share the same background art). Civilian/pleasure
# bases (New Reno et al.) get this instead of the agricultural beach concourse.
PLEASURE_CONCOURSE_SCENE = 42


def _derive_links(scene, include_hangar_sprite=True):
    """Turn a DOS scene's clickable regions (+ the Hangar foreground sprite,
    which is how station concourses reach the landing bay) into normalized
    hub links for base_screens."""
    links = []
    for r in scene.regions:
        tok = DOS_TARGET_TO_SCREEN.get(r.target)
        if not tok or not r.coords:
            continue
        xs = [c[0] for c in r.coords]; ys = [c[1] for c in r.coords]
        l, t, rt, b = min(xs), min(ys), max(xs), max(ys)
        links.append({"target": tok, "rect": [round(l/320,5), round(t/200,5),
                                               round((rt-l)/320,5), round((b-t)/200,5)]})
    if include_hangar_sprite:
        for sp in scene.fg_sprites:
            if sp.target == 207:  # WCSpriteTarget::Hangar -> our LandingPad
                links.append({"target": "LandingPad",
                              "rect": [round(sp.x1/320,5), round(sp.y1/200,5),
                                       round((sp.x2-sp.x1)/320,5), round((sp.y2-sp.y1)/200,5)]})
    return links


def build_dos_archetype(scenes, palettes, shape_files, name, concourse_scene, landing_scene, base_ids):
    """Create assets/concourse/<name>/ from a base's own DOS concourse +
    landing scenes, and point base_ids at it. Seeds structure + DOS service
    rooms from the agricultural folder (they are shared game-wide)."""
    import json, shutil
    root = REPO / "assets" / "concourse"
    agri, dst = root / "agricultural", root / name
    if not agri.exists():
        raise SystemExit("run extract_wcu_concourse.py first")
    shutil.copytree(agri, dst, dirs_exist_ok=True)

    cim = render_scene(scenes[concourse_scene], palettes, shape_files, include_foreground=False)
    cim.save(dst / "concourse_bg.png")
    lim = render_scene(scenes[landing_scene], palettes, shape_files, include_foreground=False)
    lim.save(dst / "landing_bg.png")

    manifest = json.loads((dst / "concourse.json").read_text())
    manifest["type"] = name
    croom = manifest["rooms"]["concourse"]
    croom.update(background="concourse_bg.png", bg_size=[cim.width, cim.height],
                 overlays=[], links=_derive_links(scenes[concourse_scene]))
    lroom = manifest["rooms"]["landing"]
    lroom.update(background="landing_bg.png", bg_size=[lim.width, lim.height],
                 overlays=[], links=_derive_links(scenes[landing_scene], include_hangar_sprite=False))
    (dst / "concourse.json").write_text(json.dumps(manifest, indent=2))

    # Point the base(s) at the new archetype (art only; these bases are
    # military kind, absent from commodity_prices.json -> same graceful
    # economy fallback as before, no pricing change).
    for bid in base_ids:
        bf = REPO / "assets" / "bases" / bid / "base.json"
        if bf.exists():
            txt = bf.read_text()
            import re as _re
            txt = _re.sub(r'("archetype":\s*")[a-z_]+(")', r"\g<1>" + name + r"\g<2>", txt, count=1)
            bf.write_text(txt)
    print(f"[{name}] concourse={cim.width}x{cim.height} ({len(croom['links'])} links), "
          f"landing={lim.width}x{lim.height} ({len(lroom['links'])} links) -> {base_ids}")


# Bespoke single-base concourses the "military" Perry stand-in was wrongly
# covering. Each: (concourse_scene, landing_scene, [base_ids]). Verified via
# the GAMEFLOW.IFF base->startScene map + visual ID of the rendered scenes.
DOS_BASE_ARCHETYPES = {
    "newcon": (17, 20, ["new_constantinople"]),
}


def build_pleasure(scenes, palettes, shape_files):
    """Create assets/concourse/pleasure/ from the DOS pleasure concourse.
    Reuses agricultural's Jolson landing bay (the DOS pleasure & agricultural
    bases share it) and DOS service rooms (installed by --install)."""
    import json, shutil
    root = REPO / "assets" / "concourse"
    agri, pleasure = root / "agricultural", root / "pleasure"
    if not agri.exists():
        raise SystemExit("run extract_wcu_concourse.py first (need agricultural to seed landing)")
    shutil.copytree(agri, pleasure, dirs_exist_ok=True)

    scene = scenes[PLEASURE_CONCOURSE_SCENE]
    im = render_scene(scene, palettes, shape_files, include_foreground=False)
    im.save(pleasure / "concourse_bg.png")

    # Derive hub links from the scene's clickable regions (320x200 -> 0..1).
    links = []
    for r in scene.regions:
        tok = DOS_TARGET_TO_SCREEN.get(r.target)
        if not tok or not r.coords:
            continue
        xs = [c[0] for c in r.coords]; ys = [c[1] for c in r.coords]
        left, top, right, bottom = min(xs), min(ys), max(xs), max(ys)
        links.append({"target": tok, "rect": [round(left / 320, 5), round(top / 200, 5),
                                               round((right - left) / 320, 5),
                                               round((bottom - top) / 200, 5)]})

    cj = pleasure / "concourse.json"
    manifest = json.loads(cj.read_text())
    croom = manifest["rooms"]["concourse"]
    croom["background"] = "concourse_bg.png"
    croom["bg_size"] = [im.width, im.height]
    croom["overlays"] = []           # casino art has its own baked ambience
    croom["links"] = links
    manifest["type"] = "pleasure"
    cj.write_text(json.dumps(manifest, indent=2))
    print(f"[pleasure] concourse={im.width}x{im.height}, {len(links)} hub link(s) -> {pleasure}")


# ---- install into the engine's concourse asset set -------------------------

# Which DOS scene backs each shared service room (identified by their region/
# sprite labels -- see the manifest from --dump-all-scenes). These five rooms
# are single shared scenes in the DOS game, reused across every base, exactly
# like base_screens.cpp treats them. Rendered WITH foreground so their native
# crisp NPCs (bartender, receptionist, guild master) come along baked in --
# no painterly WCU overlay needed on top.
ROOM_SCENES = {
    "commodity":  63,   # Cargo Dealer  (Buy/Sell trade screen)
    "shipdealer": 69,   # Ship Dealer / garage showroom
    "mercguild":  59,   # Mercenaries' Guild -- receptionist + computer
    "merchguild": 61,   # Merchants' Guild -- guild master + computer
    "bar":        5,    # Bar -- bartender + patrons
}


def install_dos_rooms(scenes, palettes, shape_files, scale=1):
    """Overwrite the five shared service-room backgrounds in every
    assets/concourse/<archetype>/ with the crisp DOS originals, and clear
    those rooms' WCU overlays in concourse.json (the DOS art already carries
    its own baked NPCs). Run LAST -- after extract_wcu_concourse.py /
    upscale_concourse.py / sharpen_room_backgrounds.py, which own the rest."""
    import json
    root = REPO / "assets" / "concourse"
    archetypes = sorted(d for d in root.iterdir() if d.is_dir())

    rendered = {}
    for room, sidx in ROOM_SCENES.items():
        im = render_scene(scenes[sidx], palettes, shape_files, include_foreground=True)
        if scale != 1:
            im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
        rendered[room] = im

    for adir in archetypes:
        cj = adir / "concourse.json"
        if not cj.exists():
            continue
        manifest = json.loads(cj.read_text())
        rooms = manifest.get("rooms", {})
        for room, im in rendered.items():
            if room not in rooms:
                continue
            im.save(adir / f"{room}_bg.png")
            rooms[room]["background"] = f"{room}_bg.png"
            rooms[room]["bg_size"] = [im.width, im.height]
            rooms[room]["overlays"] = []
        cj.write_text(json.dumps(manifest, indent=2))
        print(f"  {adir.name}: installed {len(rendered)} DOS room(s)")
    print(f"[install] {len(archetypes)} archetype(s) updated")


# ---- CLI --------------------------------------------------------------------

def load_game_data():
    pal_pak = decode_pak((GOG / "OPTIONS" / "OPTPALS.PAK").read_bytes())
    palettes = [decode_palette(p) for p in pal_pak]
    shape_files = decode_pak((GOG / "OPTIONS" / "OPTSHPS.PAK").read_bytes())
    scenes = decode_options_iff((GOG / "OPTIONS" / "OPTIONS.IFF").read_bytes())
    return scenes, palettes, shape_files


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dump-all-scenes", metavar="DIR",
                     help="render every scene to DIR/scene_NNN.png + a manifest of region labels")
    ap.add_argument("--scene", type=int, help="render one scene index")
    ap.add_argument("-o", "--out", default="/tmp/scene.png")
    ap.add_argument("--no-foreground", action="store_true")
    ap.add_argument("--build-bases", action="store_true",
                     help="build bespoke per-base DOS archetypes (New Constantinople, ...)")
    ap.add_argument("--build-pleasure", action="store_true",
                     help="create assets/concourse/pleasure/ from the DOS pleasure concourse")
    ap.add_argument("--install", action="store_true",
                     help="write the 5 shared DOS room backgrounds into every assets/concourse/<archetype>/")
    ap.add_argument("--scale", type=int, default=1, help="nearest upscale factor for --install (default 1)")
    args = ap.parse_args()

    scenes, palettes, shape_files = load_game_data()
    print(f"[scenes] {len(scenes)} scene(s), {len(palettes)} palette(s), {len(shape_files)} shape blob(s)")

    if args.build_bases:
        for nm, (cs, ls, bids) in DOS_BASE_ARCHETYPES.items():
            build_dos_archetype(scenes, palettes, shape_files, nm, cs, ls, bids)
    elif args.build_pleasure:
        build_pleasure(scenes, palettes, shape_files)
    elif args.install:
        install_dos_rooms(scenes, palettes, shape_files, scale=args.scale)
    elif args.dump_all_scenes:
        out = Path(args.dump_all_scenes)
        out.mkdir(parents=True, exist_ok=True)
        manifest = []
        for i, scene in enumerate(scenes):
            labels = [r.label for r in scene.regions if r.label] + \
                     [s.label for s in scene.fg_sprites if s.label]
            try:
                im = render_scene(scene, palettes, shape_files, include_foreground=not args.no_foreground)
                im.save(out / f"scene_{i:03d}.png")
                status = f"{im.width}x{im.height}"
            except Exception as e:
                status = f"FAILED: {e}"
            manifest.append(f"scene_{i:03d}: {status}  regions/sprites={labels}")
            print(manifest[-1])
        (out / "manifest.txt").write_text("\n".join(manifest))
        print(f"-> {out}")
    elif args.scene is not None:
        im = render_scene(scenes[args.scene], palettes, shape_files, include_foreground=not args.no_foreground)
        im.save(args.out)
        print(f"-> {args.out} ({im.width}x{im.height})")
    else:
        for i, scene in enumerate(scenes):
            labels = [r.label for r in scene.regions if r.label] + \
                     [s.label for s in scene.fg_sprites if s.label]
            if labels:
                print(f"scene {i}: {labels}")


if __name__ == "__main__":
    main()

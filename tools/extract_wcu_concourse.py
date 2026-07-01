#!/usr/bin/env python3
"""Extract canonical Privateer concourse art + animations from the WCU
(Vega Strike) assets into new_privateer's runtime format.

Source (read-only):   ../privateer_wcu/sprites/bases/<type>/...
  * <Name>_Concourse.image   DDS DXT1 background (magenta color-key + black bars)
  * <overlay>.spr            text flipbook descriptor -> a .ani/ frame folder
  * <ani>/<name>NNNNN.image  PNG RGBA frames (true alpha)

Output (written):     assets/concourse/<type>/
  * background.png           cropped/keyed background
  * <overlay>.png            grid atlas of the flipbook's real frames
  * concourse.json           manifest the engine reads

Coordinate model: Vega Strike places sprites by CENTER in x in [-1,1] (left..
right), y in [-1,1] (bottom..top, y-up). We convert to new_privateer's
normalized 0..1 TOP-LEFT rects (x right, y down):
    cx = (vx+1)/2 ; cy = (1-vy)/2 ; w = sw/2 ; h = sh/2
    rect = [cx - w/2, cy - h/2, w, h]
The background stretches to fill the screen and overlays use the same
normalized mapping, so they stay aligned regardless of aspect.

Archetypes are read straight off WCU's own per-base Python (bases/*.py):
mining_base.py, agricultural.py (+ pleasure_land.py for its landing pad),
unit.py (the "refinery" template — Anapolis/Edinburgh art despite the
generic filename), mining_base_pirates.py, and perry.py (used as the
"military" stand-in — New Constantinople/New Detroit/Derelict Base are each
bespoke one-off scripts in WCU with no shared generic template, so Perry's
art is the closest thing to a reusable military set). Commodity Exchange,
Ship Dealer, Equipment and both Guilds are 100% shared art in WCU itself
(every archetype's script imports the exact same commodity_lib/weapons_lib/
mercenary_guild/merchant_guild modules with no per-caller asset override) —
SHARED_ROOMS below is extracted once per archetype from those same shared
sources, not because the art differs, but because base_screens.cpp still
keys every room off assets/concourse/<archetype>/, not a shared pool.
"""
import json, math
from pathlib import Path
from PIL import Image

WCU = Path("/Users/mpfaffenberger/code/privateer_wcu")
SPRITES = WCU / "sprites"
REPO = Path(__file__).resolve().parent.parent
OUT_ROOT = REPO / "assets" / "concourse"
ATLAS_MAX_W = 4096


def crop_background(dds_path, out_png):
    im = Image.open(dds_path).convert("RGB")
    import numpy as np
    a = np.asarray(im)
    magenta = (a[:, :, 0] > 200) & (a[:, :, 1] < 60) & (a[:, :, 2] > 200)
    black = (a[:, :, 0] < 8) & (a[:, :, 1] < 8) & (a[:, :, 2] < 8)
    keep = ~(magenta | black)
    ys, xs = np.where(keep)
    box = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)
    im.crop(box).save(out_png)
    return box, im.crop(box).size


def vs_to_rect(vx, vy, sw, sh):
    cx, cy = (vx + 1) / 2.0, (1 - vy) / 2.0
    w, h = sw / 2.0, sh / 2.0
    return [round(cx - w / 2, 5), round(cy - h / 2, 5), round(w, 5), round(h, 5)]


def parse_spr(spr_path):
    """Return (total_frames, fps, lead_blanks, [real_frame_image_paths], (sw,sh)).

    Two WCU .spr shapes: a flipbook (image-dir line, size, offset, then a
    "<count> <secs/frame> [video]" header + one frame path per line) and a
    bare single-frame static sprite (just "<image_path> true" / size /
    offset — e.g. bases/bar/ag3.spr) with no header/frame-list at all since
    the one frame is already named on line 0.
    """
    lines = [l.strip() for l in Path(spr_path).read_text().splitlines() if l.strip()]
    sw, sh = (float(x) for x in lines[1].split()[:2])
    if len(lines) <= 3:
        return 1, 5.0, 0, [lines[0].split()[0]], (sw, sh)
    hdr = lines[3].split()                     # "<count> <secs/frame> video"
    total = int(hdr[0]); secs = float(hdr[1]); fps = 1.0 / secs if secs else 5.0
    real, lead = [], 0
    started = False
    for ln in lines[4:4 + total]:
        img = ln.split()[0]
        if img.endswith("blackclear.image"):
            if not started:
                lead += 1
        else:
            started = True
            real.append(img)
    return total, fps, lead, real, (sw, sh)


def pack_atlas(frame_paths, out_png):
    frames = [Image.open(SPRITES / p).convert("RGBA") for p in frame_paths]
    fw, fh = frames[0].size
    cols = max(1, min(len(frames), ATLAS_MAX_W // fw))
    rows = math.ceil(len(frames) / cols)
    atlas = Image.new("RGBA", (cols * fw, rows * fh), (0, 0, 0, 0))
    for i, fr in enumerate(frames):
        atlas.paste(fr, ((i % cols) * fw, (i // cols) * fh))
    atlas.save(out_png)
    return {"frame_w": fw, "frame_h": fh, "cols": cols, "rows": rows,
            "atlas_w": cols * fw, "atlas_h": rows * fh, "real_frames": len(frames)}


def vs_link_to_rect(x, y, w, h):
    """Base.Link(x,y,w,h): center (x,y) in [-1,1], (w,h) the VS half-extents.
    Full VS size (2w,2h) -> normalized (w,h); centered."""
    cx, cy = (x + 1) / 2.0, (1 - y) / 2.0
    return [round(cx - w / 2, 5), round(cy - h / 2, 5), round(w, 5), round(h, 5)]


def build_overlays(out_dir, prefix, overlays):
    out = []
    for ov in overlays:
        name = ov["name"]
        total, fps, lead, real, (sw, sh) = parse_spr(SPRITES / ov["spr"])
        if not real:
            print(f"  ! {prefix}/{name}: no real frames, skipped"); continue
        grid = pack_atlas(real, out_dir / f"{prefix}_{name}.png")
        rect = vs_to_rect(ov["x"], ov["y"], sw, sh)
        # Timeline length = blank lead + the frames we actually packed (some
        # WCU .spr headers claim more frames than they list; trusting that
        # count would index past the atlas).
        out.append({"name": name, "atlas": f"{prefix}_{name}.png", "rect": rect,
                    "total_frames": lead + len(real), "lead_blanks": lead,
                    "fps": round(fps, 4), **grid})
        print(f"  {prefix}/{name}: {grid['real_frames']} frames @ {fps:.1f}fps rect={rect}")
    return out


def build_room(out_dir, prefix, spec):
    box, size = crop_background(SPRITES / spec["bg"], out_dir / f"{prefix}_bg.png")
    print(f"  {prefix} background -> {size}")
    room = {"background": f"{prefix}_bg.png", "bg_size": list(size),
            "overlays": build_overlays(out_dir, prefix, spec.get("overlays", []))}
    # Transition links (the editable doors). These are DEFAULTS; the in-game
    # F3 editor saves overrides to links.json which the engine prefers.
    links = []
    if "door" in spec:
        links.append({"target": "Concourse", "rect": vs_link_to_rect(*spec["door"])})
    if "launch" in spec:
        links.append({"target": "Launch", "rect": vs_link_to_rect(*spec["launch"])})
    for extra in spec.get("links", []):
        links.append({"target": extra["target"],
                      "rect": vs_link_to_rect(*extra["rect"])})
    if links:
        room["links"] = links
    return room


def extract_type(type_name, rooms):
    out_dir = OUT_ROOT / type_name
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = {"type": type_name, "rooms": {}}
    for key, spec in rooms.items():
        print(f" [{key}]")
        manifest["rooms"][key] = build_room(out_dir, key, spec)
    (out_dir / "concourse.json").write_text(json.dumps(manifest, indent=2))
    print(f"  -> {out_dir/'concourse.json'}")


# ---- rooms shared identically across every archetype in WCU itself --------
# commodity_lib.py / weapons_lib.py / mercenary_guild.py / merchant_guild.py
# hardcode these exact paths regardless of which base script calls them —
# there's only one Commodity Exchange, one Ship Dealer/Equipment shop, and
# one pair of Guild offices in the whole game. Re-extracted (from identical
# source) into every archetype folder since base_screens.cpp keys rooms off
# assets/concourse/<archetype>/, not a shared pool.
def shared_rooms():
    return {
        "commodity":  {"bg": "bases/Commodity.image"},
        "shipdealer": {
            "bg": "bases/repair_upgrade/shipdealer.image",
            "overlays": [
                {"name": "sd", "spr": "bases/repair_upgrade/sd.spr",
                 "x": 0.002, "y": -0.0775},   # the dealer showing off ships
            ],
            "links": [
                {"target": "OpenMenu", "rect": (0.002, -0.0775, 0.12, 0.18)},
            ],
        },
        "equipment":  {"bg": "bases/repair_upgrade/shipupgrade.image"},
        "mercguild":  {
            "bg": "bases/merchant_guild/mercernaryguild.image",
            "overlays": [
                # "myg" = the Mercenary Guild receptionist (per WCU's own
                # bases/mercenary_guild.py, which textures 'myg' into this room).
                {"name": "myg", "spr": "bases/merchant_guild/myg.spr",
                 "x": 0.125, "y": 0.04},
            ],
            "links": [
                # Click zone for the guild's mission computer (WCU's
                # to_merc_comp link into a separate 'computer' room) — gates
                # the mission board so it only shows after a click.
                {"target": "OpenMenu", "rect": (-0.415, -0.226667, 0.335, 0.293333)},
            ],
        },
        "merchguild": {
            "bg": "bases/merchant_guild/merchantguild.image",
            "overlays": [
                # "mtg" = the Merchants' Guild receptionist (WCU's
                # bases/merchant_guild.py textures 'mtg' into this room).
                {"name": "mtg", "spr": "bases/merchant_guild/mtg.spr",
                 "x": -0.03125, "y": 0.01367},
            ],
            "links": [
                {"target": "OpenMenu", "rect": (-0.3175, -0.103333, 0.195, 0.223333)},
            ],
        },
    }


# ---- mining base (bases/mining_base.py) — concourse + landing + all rooms --
# Room keys match base_screens' room_key(): concourse, landing, bar, commodity,
# shipdealer, equipment, mercguild, merchguild.
MINING = dict(
    type_name="mining",
    rooms={
        "concourse": {
            "bg": "bases/mining_base/MiningBase_Concourse.image",
            "overlays": [
                {"name": "car", "spr": "bases/mining_base/MiningBase_Concourse_car.spr",
                 "x": 0.97255, "y": -0.279296875},
                {"name": "wk0", "spr": "bases/mining_base/MiningBase_Concourse_wk0.spr",
                 "x": -0.23125, "y": 0.00390625},
            ],
        },
        "landing": {
            "bg": "bases/mining_base/MiningBase_LandingPad.image",
            "overlays": [
                {"name": "shp", "spr": "bases/mining_base/MiningBase_LandingPad_shp.spr",
                 "x": 0.2125, "y": 0.6875},
                {"name": "lgt", "spr": "bases/mining_base/MiningBase_LandingPad_lgt.spr",
                 "x": 0.775, "y": 0.375},
            ],
            "door":   (0.5875, -0.36, 0.2975, 0.573333),
            "launch": (-0.3325, -0.58, 0.875, 0.653333),
        },
        "bar": {
            "bg": "bases/bar/MiningBase_Bar.image",
            "overlays": [
                {"name": "btr", "spr": "bases/mining_base/btr.spr",
                 "x": 0.925, "y": -0.259765625},   # bartender
                {"name": "mb0", "spr": "bases/bar/mb0.spr", "x": -0.8125,  "y": -0.19140625},
                {"name": "mb1", "spr": "bases/bar/mb1.spr", "x": -0.46875, "y": -0.20127255},
                {"name": "mb2", "spr": "bases/bar/mb2.spr", "x": -0.1875,  "y": -0.025390625},
            ],
        },
        **shared_rooms(),
    },
)

# ---- agricultural (bases/agricultural.py + pleasure_land.py) --------------
# Every "agricultural" archetype base (Victoria, Matahari, Helen, ...) shares
# this exact set in WCU — it's literally Helen's art + Jolson's pleasure-dome
# landing bay, reused game-wide.
AGRICULTURAL = dict(
    type_name="agricultural",
    rooms={
        "concourse": {
            "bg": "bases/agricultural/Helen_Concourse.image",
            "overlays": [
                {"name": "wtr", "spr": "bases/agricultural/Helen_Concourse_wtr.spr",
                 "x": 0, "y": 0.75},
                {"name": "wk0", "spr": "bases/agricultural/Helen_Concourse_wk0.spr",
                 "x": 0.6875, "y": -0.83},
                {"name": "wk1", "spr": "bases/agricultural/Helen_Concourse_wk1.spr",
                 "x": -0.43125, "y": -0.86},
                {"name": "wk2", "spr": "bases/agricultural/Helen_Concourse_wk2.spr",
                 "x": 0.275, "y": -0.37},
            ],
            # agricultural.py's own concourse-hub doors — same conversion the
            # F3 editor's saved links.json uses, straight off WCU's coords
            # instead of a guess. base_offers() still hides ones a specific
            # base doesn't have.
            "links": [
                {"target": "LandingPad",        "rect": (0.035, -0.346667, 0.2825, 0.27)},
                {"target": "CommodityExchange",  "rect": (0.6275, -0.37, 0.3425, 0.17)},
                {"target": "MissionComputer",    "rect": (0.3725, -0.843333, 0.2825, 0.423333)},
                {"target": "Bar",                "rect": (-0.61, -0.113333, 0.2075, 0.25)},
                {"target": "MerchantsGuild",     "rect": (0.03, 0.0933333, 0.22, 0.176667)},
                {"target": "MercenariesGuild",   "rect": (0.77, 0.0233333, 0.22, 0.226667)},
                {"target": "ShipDealer",         "rect": (-0.5725, -0.583333, 0.315, 0.386667)},
            ],
        },
        "landing": {
            "bg": "bases/pleasure/Jolson_LandingBay.image",
            "overlays": [
                {"name": "wtr", "spr": "bases/pleasure/Jolson_LandingBay_wtr.spr",
                 "x": -0.09375, "y": -0.240234375},
                {"name": "blt", "spr": "bases/pleasure/Jolson_LandingBay_blt.spr",
                 "x": 0.36875, "y": -0.03515625},
            ],
            "door":   (0.6025, -0.463333, 0.29, 0.633333),
            "launch": (-0.3125, -0.543333, 0.8975, 0.54),
        },
        "bar": {
            "bg": "bases/bar/Helen_Bar.image",
            "overlays": [
                {"name": "btr", "spr": "bases/agricultural/btr.spr",
                 "x": 0.925, "y": -0.259765625},
                {"name": "ag0", "spr": "bases/bar/ag0.spr", "x": -0.9,     "y": -0.123046875},
                {"name": "ag1", "spr": "bases/bar/ag1.spr", "x": -0.58125, "y": -0.15234375},
                {"name": "ag2", "spr": "bases/bar/ag2.spr", "x": -0.11875, "y": -0.103515625},
                {"name": "ag3", "spr": "bases/bar/ag3.spr", "x": 0.41875,  "y": -0.03515625},
            ],
        },
        **shared_rooms(),
    },
)

# ---- refinery (bases/unit.py — filename "unit.py", but this IS the shared
# refinery template: Anapolis/Edinburgh art) -------------------------------
REFINERY = dict(
    type_name="refinery",
    rooms={
        "concourse": {
            "bg": "bases/refinery/Anapolis_Concourse.image",
            "overlays": [
                {"name": "str", "spr": "bases/refinery/Anapolis_Concourse_str.spr",
                 "x": 0.6, "y": 0.6875},
                {"name": "sh0", "spr": "bases/refinery/Anapolis_Concourse_sh0.spr",
                 "x": 0.6, "y": 0.6582},
                {"name": "sh1", "spr": "bases/refinery/Anapolis_Concourse_sh1.spr",
                 "x": 0.6, "y": 0.58},
                # unit.py borrows agricultural's walkers verbatim for this room.
                {"name": "wk0", "spr": "bases/agricultural/Helen_Concourse_wk0.spr",
                 "x": 0.6875, "y": -0.83},
                {"name": "wk1", "spr": "bases/agricultural/Helen_Concourse_wk1.spr",
                 "x": -0.43125, "y": -0.86},
                {"name": "wk2", "spr": "bases/agricultural/Helen_Concourse_wk2.spr",
                 "x": 0.275, "y": -0.37},
            ],
            "links": [
                {"target": "LandingPad",        "rect": (0.035, -0.34, 0.28, 0.266667)},
                {"target": "CommodityExchange",  "rect": (0.6475, -0.366667, 0.32, 0.156667)},
                {"target": "MissionComputer",    "rect": (0.4075, -0.85, 0.15, 0.276667)},
                {"target": "Bar",                "rect": (-0.5975, -0.133333, 0.17, 0.25)},
                {"target": "MerchantsGuild",     "rect": (0.03, 0.0933333, 0.22, 0.176667)},
                {"target": "MercenariesGuild",   "rect": (0.73, 0.0466667, 0.235, 0.14)},
                {"target": "ShipDealer",         "rect": (-0.545, -0.563333, 0.255, 0.36)},
            ],
        },
        "landing": {
            "bg": "bases/refinery/Edinburgh_LandingBay.image",
            "overlays": [
                {"name": "lst", "spr": "bases/refinery/Edinburgh_LandingBay_lst.spr",
                 "x": 0.29375, "y": 0.0625},
                {"name": "lit", "spr": "bases/refinery/lit.spr",
                 "x": -0.18866, "y": 0.39393},
            ],
            "door":   (0.6225, -0.416667, 0.2425, 0.5),
            "launch": (-0.33, -0.573333, 0.8475, 0.6),
        },
        "bar": {
            # unit.py reuses Helen_Bar too — refinery bases share agricultural's
            # bar room wholesale, just with different patrons + bartender head.
            "bg": "bases/bar/Helen_Bar.image",
            "overlays": [
                {"name": "btr", "spr": "bases/refinery/btr.spr",
                 "x": 0.925, "y": -0.259765625},
                {"name": "rf0", "spr": "bases/bar/rf0.spr", "x": -0.2375,   "y": -0.17127},
                {"name": "rf1", "spr": "bases/bar/rf1.spr", "x": -0.0875,  "y": 0.013671875},
                {"name": "rf2", "spr": "bases/bar/rf2.spr", "x": -0.575,   "y": -0.18164},
                {"name": "rf3", "spr": "bases/bar/rf3.spr", "x": -0.88125, "y": -0.015625},
            ],
        },
        **shared_rooms(),
    },
)

# ---- pirate (bases/mining_base_pirates.py) — reuses mining's landing pad --
# and bar background verbatim, swaps only the concourse for the pirate-den
# art and thins the bar down to a single patron.
PIRATE = dict(
    type_name="pirate",
    rooms={
        "concourse": {
            "bg": "bases/mining_base_pirates/PirateBase_Concourse.image",
            # mining_base_pirates.py never calls mercenary_guild/merchant_guild/
            # weapons_lib at all (no doors for them exist in WCU's own script),
            # so unlike the other archetypes there's no source coordinate to
            # port for MercenariesGuild/MerchantsGuild/ShipDealer here. A pirate
            # base whose base.json grants one of those services (e.g. Megiddo's
            # Mercenaries' Guild) needs its door hand-placed with the F3 editor
            # once — same as any hotspot artistic tweak.
            "links": [
                {"target": "LandingPad",       "rect": (-0.5, 0.1, 0.3, 0.33)},
                {"target": "CommodityExchange", "rect": (0.705, -0.62, 0.255, 0.423333)},
                {"target": "MissionComputer",   "rect": (0.485, -0.443333, 0.1475, 0.163333)},
                {"target": "Bar",               "rect": (0.2875, -0.29, 0.205, 0.22)},
            ],
        },
        "landing": {
            "bg": "bases/mining_base/MiningBase_LandingPad.image",
            "overlays": [
                {"name": "shp", "spr": "bases/mining_base/MiningBase_LandingPad_shp.spr",
                 "x": 0.2125, "y": 0.6875},
                {"name": "lgt", "spr": "bases/mining_base/MiningBase_LandingPad_lgt.spr",
                 "x": 0.775, "y": 0.375},
            ],
            "door":   (0.5875, -0.36, 0.2975, 0.573333),
            "launch": (-0.5075, -0.58, 0.8025, 0.76),
        },
        "bar": {
            "bg": "bases/bar/MiningBase_Bar.image",
            "overlays": [
                {"name": "btr", "spr": "bases/mining_base/btr.spr",
                 "x": 0.925, "y": -0.259765625},
                {"name": "mb1", "spr": "bases/bar/mb1.spr", "x": -0.46875, "y": -0.20127255},
            ],
        },
        **shared_rooms(),
    },
)

# ---- military (bases/perry.py) — stand-in for the archetype ---------------
# New Constantinople, New Detroit, Perry Naval and Derelict Base are each
# bespoke one-off WCU scripts (Perry even branches into an Admiral's Office
# sub-room) — there's no single shared "military" template the way mining/
# agricultural/refinery/pirate have one. Perry's set is the closest thing
# to a reusable military concourse, so every military-archetype base uses
# it here rather than the flat placeholder. Perry's campaign-only Admiral's
# Office branch is intentionally NOT extracted — out of scope for a shared
# archetype room.
MILITARY = dict(
    type_name="military",
    rooms={
        "concourse": {
            "bg": "bases/perry/Perry_Concourse.image",
            "overlays": [
                {"name": "stt", "spr": "bases/perry/Perry_Concourse_stt.spr",
                 "x": -0.15625, "y": 0.6875},
                {"name": "stb", "spr": "bases/perry/Perry_Concourse_stb.spr",
                 "x": -0.15625, "y": 0.0918},
                {"name": "sh0", "spr": "bases/perry/Perry_Concourse_sh0.spr",
                 "x": -0.15625, "y": 0.482421875},
                {"name": "car", "spr": "bases/perry/Perry_Concourse_car.spr",
                 "x": 0.6, "y": -0.67},
            ],
            "links": [
                {"target": "LandingPad",       "rect": (-0.98, -0.923333, 0.27, 0.856667)},
                {"target": "CommodityExchange", "rect": (0.305, -0.49, 0.245, 0.313333)},
                {"target": "MissionComputer",   "rect": (0.8575, -0.0866667, 0.075, 0.106667)},
                {"target": "Bar",               "rect": (0.71, -0.0666667, 0.105, 0.2)},
                {"target": "MerchantsGuild",    "rect": (0.52, -0.01, 0.06, 0.146667)},
                {"target": "MercenariesGuild",  "rect": (0.6025, -0.0333333, 0.0825, 0.163333)},
                {"target": "ShipDealer",        "rect": (0.6375, -0.753333, 0.3275, 0.446667)},
            ],
        },
        "landing": {
            "bg": "bases/perry/Perry_LandingBay.image",
            "overlays": [
                {"name": "lbl", "spr": "bases/perry/Perry_LandingBay_lbl.spr",
                 "x": 0.1125, "y": -0.5332},
            ],
            "door":   (-0.415, -0.13, 0.2875, 0.383333),
            "launch": (0.24, -0.133333, 0.695, 0.52),
        },
        "bar": {
            "bg": "bases/bar/Helen_Bar.image",   # perry.py reuses Helen_Bar too
            "overlays": [
                {"name": "btr", "spr": "bases/perry/btr.spr",
                 "x": 0.925, "y": -0.259765625},
                {"name": "pe0", "spr": "bases/bar/pe0.spr", "x": -0.09375, "y": 0.0234375},
                {"name": "pe1", "spr": "bases/bar/pe1.spr", "x": -0.88125, "y": -0.015625},
                {"name": "pe2", "spr": "bases/bar/pe2.spr", "x": -0.575,   "y": -0.181640625},
                {"name": "pe3", "spr": "bases/bar/pe3.spr", "x": -0.2375,  "y": -0.181640625},
            ],
        },
        **shared_rooms(),
    },
)

ALL_ARCHETYPES = [MINING, AGRICULTURAL, REFINERY, PIRATE, MILITARY]

if __name__ == "__main__":
    for spec in ALL_ARCHETYPES:
        print(f"[extract] {spec['type_name']} base -> {OUT_ROOT / spec['type_name']}")
        extract_type(**spec)
    print("[extract] done")

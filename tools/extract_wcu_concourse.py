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
"""
import json, os, re, sys, math
from pathlib import Path
from PIL import Image

WCU = Path("/Users/mpfaffenberger/code/privateer_wcu")
SPRITES = WCU / "sprites"
REPO = Path(__file__).resolve().parent.parent
OUT_ROOT = REPO / "assets" / "concourse"
ATLAS_MAX_W = 4096

def is_key(px):
    r, g, b = px[0], px[1], px[2]
    return (r > 200 and g < 60 and b > 200) or (r < 8 and g < 8 and b < 8)

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
    """Return (total_frames, fps, lead_blanks, [real_frame_image_paths], (sw,sh))."""
    lines = [l.strip() for l in Path(spr_path).read_text().splitlines() if l.strip()]
    sw, sh = (float(x) for x in lines[1].split()[:2])
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

# ---- mining base: concourse + landing pad + all service rooms --------------
# Room keys match base_screens' room_key(): concourse, landing, bar, commodity,
# shipdealer, equipment, mercguild, merchguild. Guild/commodity art is shared
# across base types in the source; the bar is per-type.
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
        "mercguild":  {"bg": "bases/merchant_guild/mercernaryguild.image"},
        "merchguild": {"bg": "bases/merchant_guild/merchantguild.image"},
    },
)

if __name__ == "__main__":
    print(f"[extract] mining base -> {OUT_ROOT/'mining'}")
    extract_type(**MINING)
    print("[extract] done")

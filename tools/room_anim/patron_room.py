"""One room of 3D patrons: its camera, lights, paths and patrons (#577).

A room file is JSON:

    {"room": {"name": "Bar",
              "plate": "assets/concourse/mining/bar_bg.png",   # repo-relative
              "build": "build/room_anim/mining/bar",           # raw renders
              "out": "assets/concourse/mining/anim/bar",       # baked layers
              "sources": "tools/room_anim/mining/sources",     # clean-plate edits
              "camera": {"horizon_y": 415.0, "eye": 1.25, "focal_px": 1200.0,
                         "vp_x": 768.0},               # optional: default the plate centre
              "lights": {"key": [r, g, b], "fill": [r, g, b], "key_w": 350.0,
                         "key_size": 0.6, "fill_w": 6.0, "rim_w": 40.0,
                         "ambient": [r, g, b]}},
     "<patron>": {...}, ...}                # back to front; "_" keys are docs

Patron keys are documented in mining/bar_patrons.json's _doc, plus two for
standing patrons who lean on something (#578): `bend` (deg) folds the spine
forward through every key, and `hip_sway` (0-1) scales the hips' travel
(`lean` only damps rotations). Room files name bones the Mixamo way; the
Meshy API's own 24-bone rig is mapped (render_patrons._bone).

render_patrons.py and bake_patrons.py both take `--room <file>`. Pure stdlib,
so it imports both inside Blender and from uv scripts.
"""
import json
from pathlib import Path
from types import SimpleNamespace

from base import REPO


def load(path):
    data = json.loads(Path(path).read_text())
    room = data["room"]
    return SimpleNamespace(
        file=Path(path),
        name=room["name"],
        plate=REPO / room["plate"],
        build=REPO / room["build"],
        out=REPO / room["out"],
        sources=REPO / room["sources"],
        camera=SimpleNamespace(**room["camera"]),
        lights=SimpleNamespace(**room["lights"]),
        patrons={k: v for k, v in data.items() if k != "room" and not k.startswith("_")})

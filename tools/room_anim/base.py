"""Where one base's room-animation files live (#557). Pure stdlib, so it is
importable both from uv scripts and inside Blender.

    paths("mining").plate  -> assets/concourse/mining/concourse_bg.png
    paths("oxford", "shipdealer").plate -> assets/concourse/oxford/shipdealer_bg.png

A room other than the concourse (#682) keeps its bakes in anim/<room>/, its
raw renders in build/room_anim/<base>/<room>/ and its timing in
tools/room_anim/<base>/<room>_layers.json, as the landing pads already do.
"""
from pathlib import Path
from types import SimpleNamespace

TOOLS = Path(__file__).resolve().parent             # tools/room_anim
REPO = TOOLS.parents[1]


def paths(base, room="concourse"):
    root = REPO / "assets/concourse" / base
    concourse = room == "concourse"
    return SimpleNamespace(
        room=root,                                  # concourse.json + painted plates
        plate=root / f"{room}_bg.png",              # the room's hero plate
        anim=root / "anim" if concourse else root / "anim" / room,   # baked engine assets
        build=REPO / "build/room_anim" / base / ("" if concourse else room),   # raw renders
        tools=TOOLS / base,                         # this base's scripts + timing
        timing=TOOLS / base / ("layers.json" if concourse else f"{room}_layers.json"))

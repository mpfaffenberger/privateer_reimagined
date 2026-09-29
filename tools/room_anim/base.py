"""Where one base's room-animation files live (#557). Pure stdlib, so it is
importable both from uv scripts and inside Blender.

    paths("mining").plate  -> assets/concourse/mining/concourse_bg.png
"""
from pathlib import Path
from types import SimpleNamespace

TOOLS = Path(__file__).resolve().parent             # tools/room_anim
REPO = TOOLS.parents[1]


def paths(base):
    room = REPO / "assets/concourse" / base
    return SimpleNamespace(
        room=room,                                  # concourse.json + painted plates
        plate=room / "concourse_bg.png",            # the concourse's hero plate
        anim=room / "anim",                         # baked engine assets
        build=REPO / "build/room_anim" / base,      # raw renders (not committed)
        tools=TOOLS / base,                         # this base's scripts + timing
        timing=TOOLS / base / "layers.json")        # concourse layer loop timing

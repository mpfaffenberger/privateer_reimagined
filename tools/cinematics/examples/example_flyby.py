"""Example cinematic: a pure camera + music flyby (no dialogue).

Proves the builder emits a valid timeline with just camera splines, a music
bed, letterbox and fades \u2014 no portraits, no actors.

    python -m tools.cinematics.examples.example_flyby
    python -m tools.cinematics.validate flyby_demo
"""
from __future__ import annotations

try:
    from ..builder import Cinematic
except ImportError:  # allow direct execution
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from builder import Cinematic  # type: ignore


def build() -> str:
    c = Cinematic("flyby_demo", letterbox=True, skippable=True)

    c.at(0.0).fade_in(2.0)
    c.at(0.0).music("audio/flyby_theme.wav")

    # A sweeping establishing pass that ends looking back at the origin.
    c.at(0.5).camera_path(
        keys=[
            {"pos": [0, 400, 6000], "look_at": [0, 0, 0]},
            {"pos": [2500, 150, 3000]},
            {"pos": [3200, -100, 400], "look_at": [0, 0, 0]},
        ],
        ease="smooth", dur=8.0,
    )
    # A second, tighter arc after the first settles.
    c.at(9.0).camera_path(
        keys=[[3200, -100, 400], [1200, 300, -1800], [0, 500, -3600]],
        look_at=[0, 0, 0], ease="smooth", dur=5.0,
    )

    c.at(13.0).fade_out(2.0)
    c.at(15.0).end()

    path = c.save()
    print(f"[flyby] wrote {path}")
    return c.id


if __name__ == "__main__":
    build()

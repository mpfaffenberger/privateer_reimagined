"""Example cinematic: Grayson vs a pirate \u2014 a dual-portrait exchange.

Proves the full authoring stack end to end:
  * spawn two real ships (hero + pirate),
  * fly the camera tracking them,
  * dual left/right ``.line()`` beats with **auto portrait generation** (in
    placeholder mode, zero-spend) wired straight into each cue,
  * voice/sfx, letterbox, and an ``end`` action that sets a campaign flag.

    python -m tools.cinematics.examples.example_confrontation
    python -m tools.cinematics.validate confrontation_demo
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
    # auto_portraits + placeholder backend => art is generated offline and the
    # returned PNG path is wired into each line cue automatically.
    c = Cinematic("confrontation_demo", letterbox=True, skippable=True,
                  auto_portraits=True, portrait_backend="placeholder")

    c.at(0.0).fade_in(1.5)
    c.at(0.0).music("audio/tension_bed.wav")

    # Two real ships enter the scene.
    c.at(0.2).spawn("hero", cls="tarsus", faction="civilian", pos=[0, 0, 0])
    c.at(0.2).spawn("raider", cls="talon", faction="pirate", pos=[600, 40, 900])

    # Camera holds the standoff, tracking the pirate as it closes.
    c.at(0.5).camera_path(
        keys=[
            {"pos": [-300, 120, -700], "look_at": "ship:raider"},
            {"pos": [-150, 80, -300], "look_at": "ship:raider"},
        ],
        ease="smooth", dur=11.0,
    )
    c.at(0.6).actor_path("raider", keys=[[600, 40, 900], [300, 20, 400]], dur=6.0)

    # The exchange \u2014 alternating sides = an on-screen back-and-forth.
    c.at(2.0).line("pirate",
                   "Cut your engines and drop the cargo. Last warning.",
                   emotion="cold sneer, cybernetic eye glinting",
                   side="right", dur=3.5,
                   voice_file="audio/pirate_warning.wav")
    c.at(5.8).line("grayson",
                   "I didn't sign up for this.",
                   emotion="bitter, jaw set, looking away",
                   side="left", dur=3.2,
                   voice_file="audio/grayson_signup.wav")
    c.at(9.2).line("pirate",
                   "Then you'll die not knowing why.",
                   emotion="vicious grin, leaning in",
                   side="right", dur=3.0)

    # A weapons-hot sfx sting, positional near the pirate.
    c.at(12.4).sfx("audio/blaster_charge.wav", pos=[300, 20, 400])

    c.at(13.0).fade_out(1.5)
    c.at(14.5).end(actions=["set_flag:pirate_confrontation_seen"])

    path = c.save()
    print(f"[confrontation] wrote {path}")
    return c.id


if __name__ == "__main__":
    build()

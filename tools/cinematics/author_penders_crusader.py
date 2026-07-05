#!/usr/bin/env python3
"""Author `penders_crusader` — the Vera "Crusader" Rostova Steltek ambush.

A wave of ten retro talons led by the fanatical ace Vera "Crusader" Rostova
swarms Grayson's Centurion at the Nitir jump of Blockade Point Tango. Vera
accuses Grayson of carrying high-level alien technology — "MACHINES OF
DAMNABLE INTENT". Grayson turns the whole thing into a joke by casually
flirting with Vera, only making her more agitated: "VILE HERETIC!! PREPARE
TO DIE".

Trigger (assets/cinematics/triggers.json): in blockade_point_tango, near the
Nitir Jump nav, requires steltek_gun_owned, has not seen penders_crusader_seen.
Outcome: 10 hostile retro talons near the player at the Nitir Jump nav;
cleared_flag penders_crusader_cleared when the wing is dead.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(__file__))))

from tools.cinematics.builder import Cinematic


def _follow_cam(c: Cinematic, t: float, follow: str, offset,
                dur: float) -> None:
    """Append a follow-mode camera_path cue at ``t``.

    The Cinematic builder's ``camera_path`` only supports spline cameras.
    Follow mode (camera tracks a spawned actor at a fixed offset) is part of
    the DSL but the builder doesn't expose it directly, so we drop down to
    ``_At._add`` with a pre-formed cue dict. ``offset`` is [x, y, z] relative
    to the ship.
    """
    c.at(t)._add({
        "cmd": "camera_path",
        "dur": float(dur),
        "follow": follow,
        "keys": [{"pos": list(offset),
                  "look_at": f"ship:{follow.split(',')[0]}"}],
    })


# ---------------------------------------------------------------------------
# Stage geometry — the Nitir Jump of Blockade Point Tango sits at
# [200000, 66667, -133333]. We drop Grayson on a heading OUT of the jump
# (toward the system interior, -x/-z) and have the talon wing close from
# behind / below to sell the ambush.
# ---------------------------------------------------------------------------
NITIR_JUMP = [200000.0, 66667.0, -133333.0]

# Grayson sits roughly 1500m ahead of the jump, drifting away from it
# (the player just arrived, still on approach).
GRAYSON_POS = [198500.0, 66500.0, -131800.0]

# Vera slides in from the upper-right of Grayson for the cinematic reveal —
# within ~2km so the close-up follows stay tight.
VERA_POS = [199400.0, 67000.0, -130400.0]

# The ten talons spread out behind / below Grayson in two chevrons. Keep all
# of them within ~2km of Grayson so the close-up formation shots can hold
# them on frame.
TALON_FORMATION = [
    # upper chevron — five talons flanking Vera
    [200800.0, 67500.0, -130200.0],
    [201300.0, 67300.0, -129800.0],
    [201900.0, 67000.0, -129200.0],
    [202500.0, 66800.0, -128600.0],
    [203200.0, 66600.0, -128000.0],
    # lower chevron — five talons climbing up from below
    [200600.0, 65800.0, -130800.0],
    [201100.0, 65600.0, -130400.0],
    [201700.0, 65300.0, -129800.0],
    [202300.0, 65000.0, -129200.0],
    [203000.0, 64700.0, -128600.0],
]


def main() -> None:
    c = Cinematic(
        "penders_crusader",
        letterbox=True,
        skippable=True,
        auto_portraits=True,
        auto_voices=True,
    )

    # -- Entry-point teleport + post-cinematic outcome ---------------------
    c.location("blockade_point_tango", "Nitir Jump")
    c.outcome(
        player_at_nav="Nitir Jump",
        spawns=[
            {"class": "talon", "faction": "retro", "count": 10,
             "hostile": True, "cleared_flag": "penders_crusader_cleared"},
        ],
    )

    # =====================================================================
    # PHASE 1 — ESTABLISHING (0 – 5s)
    # =====================================================================
    c.at(0.0).fade_in(1.5)
    c.at(0.0).music("../music/original/combat_06.wav")
    c.at(0.0).sfx("../sfx/engine_hum.wav")

    # Spawn Grayson first so other actors can see him when they spawn.
    c.at(0.5).spawn("grayson_ship", cls="$player", pos=GRAYSON_POS, faction="civilian")

    # Wide establishing — Grayson drifting past the Nitir Jump. Use a
    # SPLINE camera (no follow) here because no dialogue is active yet and
    # we want to see the jump geometry behind him.
    c.at(0.5).camera_path(
        keys=[
            {"pos": [201800.0, 67800.0, -130400.0],
             "look_at": GRAYSON_POS},
            {"pos": [199600.0, 67200.0, -131200.0],
             "look_at": GRAYSON_POS},
        ],
        dur=5.0, ease="smooth",
    )
    c.at(0.7).subtitle(
        "BLOCKADE POINT TANGO — Nitir jump, fresh on approach. Cargo hold is hot.",
        dur=4.5,
    )

    # =====================================================================
    # PHASE 2 — THE WING DROPS OUT (5 – 10s)
    # =====================================================================
    # Spawn Vera first so the talons fan out around her visually.
    c.at(5.0).spawn("vera", cls="talon", pos=VERA_POS, faction="retro")
    for i, pos in enumerate(TALON_FORMATION, start=1):
        c.at(5.0).spawn(f"talon{i}", cls="talon", pos=pos, faction="retro")

    # Bring the wing in on a converging path — talons close on Grayson's six.
    for i, pos in enumerate(TALON_FORMATION, start=1):
        # Move each talon ~700m toward Grayson over 3 seconds.
        target = [
            pos[0] - 700.0, pos[1] + 50.0 * (1 if i <= 5 else -1), pos[2] + 700.0,
        ]
        c.at(5.5).actor_path(
            f"talon{i}",
            keys=[list(pos), list(target)],
            dur=3.0,
        )
    c.at(5.5).actor_path(
        "vera",
        keys=[list(VERA_POS), [198900.0, 66900.0, -131100.0]],
        dur=3.0,
    )

    # Lock-seek SFX at the jump nav (positional, sells the targeting pass).
    c.at(6.0).sfx("../sfx/lock_seeking.wav", pos=NITIR_JUMP)

    # Formation wide shot — follow the talon centroid for 4 seconds, then
    # we'll cut to the Vera close-up. (No dialogue during this beat.)
    _follow_cam(c, 6.0, "vera,talon1,talon2,talon3,talon4,talon5",
                offset=[0.0, 400.0, -800.0], dur=4.0)

    # =====================================================================
    # PHASE 3 — VERA'S ACCUSATION (10 – 22s)
    # =====================================================================
    # Close-up on Vera.
    _follow_cam(c, 10.0, "vera",
                offset=[350.0, 90.0, 320.0], dur=6.0)
    c.at(10.0).line(
        "vera",
        "Halt, freighter. Stand down your drive and submit to a holy inspection.",
        emotion="cold authority, chin raised, eye-patch catching the light",
        side="right", dur=5.5,
    )

    # Grayson close-up — first response.
    _follow_cam(c, 16.0, "grayson_ship",
                offset=[320.0, 80.0, -340.0], dur=6.0)
    c.at(16.0).line(
        "grayson",
        "Holy inspection? That's a new one. Mind telling me which church sends retro fighters these days?",
        emotion="dry amusement, one eyebrow up, easy half-smirk",
        side="left", dur=5.5,
    )

    # Vera close-up — the accusation.
    _follow_cam(c, 22.0, "vera",
                offset=[-340.0, 90.0, 300.0], dur=6.5)
    c.at(22.0).line(
        "vera",
        "Silence, heretic. You carry MACHINES OF DAMNABLE INTENT. Alien filth that will consume us all.",
        emotion="fierce, jaw set, voice rising, leaning forward in the cockpit",
        side="right", dur=6.0,
    )

    # =====================================================================
    # PHASE 4 — GRAYSON'S FLIRT (28.5 – 40s)
    # =====================================================================
    _follow_cam(c, 28.5, "grayson_ship",
                offset=[-310.0, 70.0, 320.0], dur=6.0)
    c.at(28.5).line(
        "grayson",
        "Machines of damnable intent, huh. Is that what they're calling hard-light emitters at the retrofit shop?",
        emotion="playful, lazy smile, eyebrows up",
        side="left", dur=5.5,
    )

    _follow_cam(c, 34.5, "grayson_ship",
                offset=[330.0, 80.0, -310.0], dur=6.0)
    c.at(34.5).line(
        "grayson",
        "For what it's worth, that eye-patch really brings out your cheekbones. The burn's a nice touch too.",
        emotion="open flirtation, easy grin, slight lean-in toward the comm",
        side="left", dur=5.5,
    )

    _follow_cam(c, 40.5, "vera",
                offset=[340.0, 100.0, 320.0], dur=4.5)
    c.at(40.5).line(
        "vera",
        "You DARE — you think this is a JOKE?!",
        emotion="seething, fist clenched, voice cracking with rage",
        side="right", dur=4.0,
    )

    # =====================================================================
    # PHASE 5 — THE WING OPENS FIRE (45 – 52s)
    # =====================================================================
    # Vera close-up — the verdict.
    _follow_cam(c, 45.0, "vera",
                offset=[-330.0, 100.0, 300.0], dur=5.5)
    c.at(45.0).line(
        "vera",
        "VILE HERETIC!! PREPARE TO DIE.",
        emotion="absolutely furious, shouting, hand on weapons release",
        side="right", dur=5.0,
    )

    # The wing opens fire — laser + explosion SFX, then a wide pan as the
    # talons break formation and dive on Grayson.
    c.at(50.5).sfx("../sfx/laser_fire.wav", pos=[199400.0, 66900.0, -131100.0])
    c.at(50.8).sfx("../sfx/impact_armor.wav", pos=GRAYSON_POS)

    # Drive the talons onto attack runs — converging on Grayson.
    for i, pos in enumerate(TALON_FORMATION, start=1):
        target = [
            pos[0] - 1800.0,
            pos[1] + 50.0 * (1 if i <= 5 else -1),
            pos[2] + 1800.0,
        ]
        c.at(50.5).actor_path(
            f"talon{i}",
            keys=[list(pos), list(target)],
            dur=3.0,
        )
    c.at(50.5).actor_path(
        "vera",
        keys=[[198900.0, 66900.0, -131100.0],
              [197800.0, 67200.0, -130200.0]],
        dur=3.0,
    )

    # Wide battle pan for the cinematic finale (no dialogue after this).
    _follow_cam(c, 50.5,
                "vera,talon1,talon2,talon3,talon4,talon5,talon6,talon7,talon8,talon9,talon10",
                offset=[0.0, 500.0, -1100.0], dur=4.5)

    # =====================================================================
    # PHASE 6 — HANDOFF TO COMBAT (55 – 57.5s)
    # =====================================================================
    # One last hard cut on Grayson — close-up, defiant.
    _follow_cam(c, 55.0, "grayson_ship",
                offset=[320.0, 70.0, -320.0], dur=3.5)
    c.at(55.0).line(
        "grayson",
        "Worth a try.",
        emotion="rueful half-grin, glancing at the warbook, hands on the stick",
        side="left", dur=2.5,
    )

    # Final big SFX — wing engaging as the cinematic hands off.
    c.at(56.0).sfx("../sfx/explosion_big.wav", pos=[197500.0, 66500.0, -131000.0])

    # =====================================================================
    # END
    # =====================================================================
    c.at(58.0).fade_out(1.0)
    c.at(59.0).end(actions=["set_flag:penders_crusader_seen"])

    path = c.save()
    print(f"Saved: {path}")


if __name__ == "__main__":
    main()
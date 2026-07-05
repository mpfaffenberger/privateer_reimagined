#!/usr/bin/env python3
"""Author penders_haulers: a merchant gives Grayson a tour of the Troy system."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__)))))

from tools.cinematics.builder import Cinematic

NAV = {
    "pyrenees":    [100000,   66667,  133333],
    "penders":     [100000,  133333, -133333],
    "hector":     [-166667, -133333, -200000],
    "regallis":   [-150000, -100000,  -66667],
    "helen":      [-166667,  -66667,  100000],
    "war":          [0,          0,  150000],
    "nav8":        [50000,  -100000,   33333],
}

# Ship spawn positions near Achilles [200000, -133333, 0]
GRAYSON_POS = [200000, -133333, 6000]
MERCHANT_POS = [204000, -132800, 6000]

# Close-up camera offsets (~400m from each ship)
CAM_MERCHANT = [204000, -132500, 6400]   # ~400m from merchant
CAM_GRAYSON  = [200000, -132500, 6400]   # ~400m from grayson
# Establishing shot: between/above both ships
CAM_ESTAB    = [202000, -132000, 9000]

def tour_keys(nav_pos, offset=3000):
    """Two camera keys orbiting a nav point for a fly-by feel."""
    x, y, z = nav_pos
    return [
        {"pos": [x + offset, y + offset//2, z + offset], "look_at": nav_pos},
        {"pos": [x - offset//2, y - offset, z + offset//2], "look_at": nav_pos},
    ]

def main():
    c = Cinematic(
        "penders_haulers",
        letterbox=True,
        skippable=True,
        auto_portraits=True,
        auto_voices=True,
    )

    # -- Location + Outcome --
    c.location("troy", "Achilles Mining Base")
    c.outcome(
        player_at_nav="Achilles Mining Base",
        spawns=[
            {"class": "drayman", "faction": "merchant", "count": 2},
            {"class": "talon", "faction": "militia", "count": 2},
        ],
    )

    # ================================================================
    # PHASE 1 — ESTABLISHING (0 – 2s)
    # ================================================================
    c.at(0.0).fade_in(1.5)
    c.at(0.0).music("../music/original/basetune_00.wav")
    c.at(0.0).sfx("../sfx/engine_hum.wav")
    c.at(0.0).spawn("grayson_ship", cls="$player", pos=GRAYSON_POS, faction="civilian")
    c.at(0.0).spawn("merchant_ship", cls="drayman", pos=MERCHANT_POS, faction="merchant")
    # Wide establishing: both ships visible near Achilles
    c.at(0.0).camera_path(
        keys=[
            {"pos": CAM_ESTAB, "look_at": [202000, -133333, 6000]},
            {"pos": [201500, -131800, 8500], "look_at": [202000, -133333, 6000]},
        ],
        dur=2.0, ease="smooth",
    )

    # ================================================================
    # PHASE 2 — HAIL DIALOGUE (2 – 15.5s)
    # ================================================================
    # Comm channel open
    c.at(2.0).sfx("../sfx/ui_click.wav")

    # Vance hails — close-up on merchant_ship
    c.at(2.0).camera_path(
        keys=[
            {"pos": CAM_MERCHANT, "look_at": "ship:merchant_ship"},
            {"pos": [204200, -132400, 6500], "look_at": "ship:merchant_ship"},
        ],
        dur=3.5, ease="smooth",
    )
    c.at(2.0).line(
        "vance", "Hey there! New to the Troy system?",
        emotion="friendly, warm grin, leaning toward the comm",
        side="left", dur=3.5,
    )

    # Grayson responds — close-up on grayson_ship
    c.at(5.5).camera_path(
        keys=[
            {"pos": CAM_GRAYSON, "look_at": "ship:grayson_ship"},
            {"pos": [200200, -132400, 6500], "look_at": "ship:grayson_ship"},
        ],
        dur=3.5, ease="smooth",
    )
    c.at(5.5).line(
        "grayson", "Just pulled in. Still getting my bearings.",
        emotion="calm, slightly wary, half-smile",
        side="right", dur=3.5,
    )

    # Vance offers the tour — close-up on merchant_ship
    c.at(9.0).camera_path(
        keys=[
            {"pos": [203800, -132600, 6300], "look_at": "ship:merchant_ship"},
            {"pos": CAM_MERCHANT, "look_at": "ship:merchant_ship"},
        ],
        dur=4.0, ease="smooth",
    )
    c.at(9.0).line(
        "vance", "I don't mind giving you a quick tour if you like. Call it a welcome.",
        emotion="generous, easy smile, gesturing broadly",
        side="left", dur=4.0,
    )

    # Grayson accepts — close-up on grayson_ship
    c.at(13.0).camera_path(
        keys=[
            {"pos": [199800, -132600, 6300], "look_at": "ship:grayson_ship"},
            {"pos": CAM_GRAYSON, "look_at": "ship:grayson_ship"},
        ],
        dur=2.5, ease="smooth",
    )
    c.at(13.0).line(
        "grayson", "Sure. I'm listening.",
        emotion="intrigued, slight nod, settling in",
        side="right", dur=2.5,
    )

    # ================================================================
    # PHASE 3 — TOUR (15.5 – 50.5s)  7 nav points, 5s each
    # ================================================================
    tour = [
        ("pyrenees", "Pyrenees Jump — the lane out to the Humboldt border. "
                     "Militia patrol it heavy, but Retro fanatics love to hit convoys here."),
        ("penders",  "Pender's Star Jump — a rough corridor. Bounty hunters work it, "
                     "and pirates hit the merchant lanes hard. Watch your six."),
        ("hector",   "Hector Mining Base — the other dig site. Militia keep a wing there, "
                     "but Retros have been testing the defenses. Ore flows, blood too."),
        ("regallis", "Regallis Jump — quiet on paper, but pirates shadow the ore shipments. "
                     "Don't drift here without guns hot."),
        ("helen",    "Helen — the agricultural planet. Feeds half the quadrant. "
                     "Bounty hunters orbit looking for smugglers running food off-world."),
        ("war",      "War Jump — the military corridor. Militia run in strength here. "
                     "Civilians are... not encouraged to linger."),
        ("nav8",     "Troy Nav 8 — the dead-center waypoint. Militia use it as a rally point. "
                     "Good place to get scanned, bad place to hide anything."),
    ]
    t = 15.5
    for key, text in tour:
        nav_pos = NAV[key]
        c.at(t).camera_path(keys=tour_keys(nav_pos), dur=5.0, ease="smooth")
        c.at(t).subtitle(text, dur=5.0)
        t += 5.0

    # ================================================================
    # PHASE 4 — WRAP-UP DIALOGUE (50.5 – 57.5s)
    # ================================================================
    # Vance says goodbye — close-up on merchant_ship
    c.at(50.5).camera_path(
        keys=[
            {"pos": CAM_MERCHANT, "look_at": "ship:merchant_ship"},
            {"pos": [204100, -132300, 6600], "look_at": "ship:merchant_ship"},
        ],
        dur=4.0, ease="smooth",
    )
    c.at(50.5).line(
        "vance", "That's the grand tour. Achilles is home — berth, refuel, and don't fly drunk. Safe skies, kid.",
        emotion="fatherly, warm smirk, two-finger salute",
        side="left", dur=4.0,
    )

    # Grayson thanks him — close-up on grayson_ship
    c.at(54.5).camera_path(
        keys=[
            {"pos": CAM_GRAYSON, "look_at": "ship:grayson_ship"},
            {"pos": [200100, -132300, 6600], "look_at": "ship:grayson_ship"},
        ],
        dur=3.0, ease="smooth",
    )
    c.at(54.5).line(
        "grayson", "Appreciate it. Safe skies.",
        emotion="grateful, genuine small smile, nodding",
        side="right", dur=3.0,
    )

    # ================================================================
    # END
    # ================================================================
    c.at(57.5).fade_out(1.0)
    c.at(58.5).end(actions=["set_flag:penders_haulers_seen"])

    path = c.save()
    print(f"Saved: {path}")


if __name__ == "__main__":
    main()

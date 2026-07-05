"""Author `studio_cinematic` — Pender's Star asteroid-field ambush.
# STALE MIRROR WARNING: assets/cinematics/studio_cinematic.json has since been
# hand-tuned (real audio paths, $player hero ship, chase cam, +100k corridor
# translation, outcome player_pos, talons hostile=false). Do NOT regenerate
# from this script without re-applying those; the JSON is the runtime truth.

Two draymen cruise in autopilot formation, inbound from the Troy jump gate
toward the asteroid field. Grayson's tarsus catches up and tucks into the
formation. The merchant crews trade uneasy talk about ships vanishing in this
field; after a beat of silence at the field's edge, five pirate talons swoop
out of the rocks: "You've fallen right into our trap, little piggies!"

Run from the repo root:
    python -m tools.cinematics.author_studio_cinematic
"""
from tools.cinematics.builder import Cinematic


def main() -> str:
    c = Cinematic(
        "studio_cinematic",
        letterbox=True,
        skippable=True,
        auto_portraits=True,
        auto_voices=True,
    )

    # -- open --------------------------------------------------------------
    c.at(0.0).fade_in(1.5)
    c.at(0.0).music("audio/penders_ambush_theme.wav")

    # -- the two-drayman convoy in autopilot formation ---------------------
    c.at(0.5).spawn("hauler1", cls="drayman", pos=[34000, 0, 36000],
                    faction="merchant")
    c.at(0.5).spawn("hauler2", cls="drayman", pos=[30000, 300, 39000],
                    faction="merchant")
    # Both haul forward down the lane (Troy jump -> asteroid field) for the
    # whole scene, holding a loose side-by-side formation.
    c.at(0.5).actor_path("hauler1", keys=[
        [34000, 0, 36000], [22000, 0, 22000],
        [10000, 0, 8000], [2000, 0, -2000],
    ], dur=44.0)
    c.at(0.5).actor_path("hauler2", keys=[
        [30000, 300, 39000], [18000, 300, 25000],
        [6000, 300, 11000], [-2000, 300, 1000],
    ], dur=44.0)

    # -- Grayson's tarsus overtakes from behind and joins formation --------
    c.at(5.0).spawn("grayson_ship", cls="tarsus", pos=[50000, -2500, 54000],
                    faction="civilian")
    c.at(5.0).actor_path("grayson_ship", keys=[
        [50000, -2500, 54000], [38000, -1200, 42000],
        [26000, -300, 28000], [14000, 0, 12000], [6000, 0, 2000],
    ], dur=40.0)

    # -- cameras (non-overlapping) -----------------------------------------
    # 1) establishing wide of the convoy cruising the lane
    c.at(0.5).camera_path(keys=[
        {"pos": [40000, 4200, 30000]},
        {"pos": [30000, 2600, 26000]},
    ], look_at="ship:hauler1", ease="smooth", dur=8.0)
    # 2) Grayson sliding into formation
    c.at(9.0).camera_path(keys=[
        {"pos": [42000, -1800, 46000]},
        {"pos": [24000, 900, 24000]},
    ], look_at="ship:grayson_ship", ease="smooth", dur=8.0)
    # 3) over-the-formation two-shot while the crews talk
    c.at(17.5).camera_path(keys=[
        {"pos": [26000, 1600, 30000]},
        {"pos": [16000, 1200, 18000]},
    ], look_at="ship:hauler2", ease="smooth", dur=12.0)
    # 4) ominous push toward the silent asteroid field (the held-breath beat)
    c.at(30.0).camera_path(keys=[
        {"pos": [16000, 900, 16000], "look_at": [9000, 0, 6000]},
        {"pos": [12000, 500, 9000], "look_at": [-16667, 0, -16667]},
    ], ease="smooth", dur=4.5)
    # 5) ambush reveal — talons boil up out of the rocks onto the convoy
    c.at(35.0).camera_path(keys=[
        {"pos": [14000, 2200, 8000]},
        {"pos": [12000, 1200, 12000]},
        {"pos": [10000, 400, 15000]},
    ], look_at="ship:hauler1", ease="smooth", dur=8.5)

    # -- dialogue ----------------------------------------------------------
    c.at(2.0).subtitle(
        "PENDER'S STAR - inbound from the Troy jump gate, autopilot locked "
        "for the asteroid field.", dur=4.5)

    c.at(10.0).line(
        "grayson",
        "Pender's Star haulers, this is Grayson. Mind if I tuck into your "
        "formation? This lane's been eating ships.",
        emotion="easy, watchful; a wry half-smile over the comm",
        side="left", dur=5.0)

    c.at(15.5).line(
        "vance",
        "Plenty of room, privateer. Truth be told - we're glad of the extra "
        "guns out here.",
        emotion="gruff, tired warmth; relieved to see a fighter",
        side="right", dur=4.5)

    c.at(20.5).line(
        "kort",
        "Third convoy this month gone dark out here, Cap. No wreckage. No "
        "beacon. Nothing.",
        emotion="anxious, wide-eyed; glancing at the sensor board",
        side="left", dur=5.0)

    c.at(26.0).line(
        "vance",
        "Ships don't just vanish. Somebody's out here picking us off the "
        "lane, one at a time.",
        emotion="grim, jaw set; a hard uneasy stare into the dark",
        side="right", dur=5.0)

    c.at(31.5).line(
        "kort",
        "I don't like this field, Cap. I've had a bad feeling since we "
        "jumped in.",
        emotion="frightened, voice tight; shrinking into the seat",
        side="left", dur=4.0)

    # ...a beat of silence at the field's edge... then they hit.
    c.at(35.5).sfx("audio/proximity_alarm.wav", pos=[9000, 0, 6000])

    # -- the trap springs: five pirate talons out of the rocks -------------
    c.at(34.5).spawn("talon1", cls="talon", pos=[-2000, 2000, -4000],
                     faction="pirate")
    c.at(34.5).spawn("talon2", cls="talon", pos=[4000, -1500, -8000],
                     faction="pirate")
    c.at(34.5).spawn("talon3", cls="talon", pos=[-8000, 800, 1000],
                     faction="pirate")
    c.at(34.5).spawn("talon4", cls="talon", pos=[12000, 2500, -6000],
                     faction="pirate")
    c.at(34.5).spawn("talon5", cls="talon", pos=[0, -2500, -10000],
                     faction="pirate")
    # three of them dive straight onto the convoy
    c.at(35.0).actor_path("talon1", keys=[
        [-2000, 2000, -4000], [4000, 800, 2000], [8000, 0, 5000],
    ], dur=8.0)
    c.at(35.0).actor_path("talon2", keys=[
        [4000, -1500, -8000], [6000, -500, -1000], [8000, 0, 4000],
    ], dur=8.0)
    c.at(35.0).actor_path("talon3", keys=[
        [-8000, 800, 1000], [0, 300, 4000], [7000, 0, 6000],
    ], dur=8.0)

    c.at(37.5).line(
        "pirate",
        "You've fallen right into our trap, little piggies!",
        emotion="gleeful cruelty; a broken-toothed sneer into the comm",
        side="right", dur=4.5)

    # -- land the player straight into the furball -------------------------
    c.at(43.5).end(actions=["set_flag:penders_ambush_seen"])

    # entry-point: the scene is authored against Pender's Star's asteroid
    # field — play teleports the player there first (cross-system if needed).
    c.location("penders_star", "Asteroid Field")

    # post-cinematic world state: player at the asteroid field, terrified
    # merchants and five pirate talons already firing.
    c.outcome(
        player_at_nav="Asteroid Field",
        spawns=[
            {"class": "drayman", "faction": "merchant", "count": 2},
            {"class": "talon", "faction": "pirate", "count": 5,
             "hostile": True},
        ],
    )

    path = c.save()
    print(f"saved -> {path}")
    return c.id


if __name__ == "__main__":
    print(main())

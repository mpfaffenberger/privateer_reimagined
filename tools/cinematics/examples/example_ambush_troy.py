"""Capstone cinematic: "Ambush at Troy".

Brief: Grayson gets ambushed leaving Troy — a pirate hails him with a taunt,
Grayson fires back a defiant line, a short firefight, and Grayson barely
escapes.

This is the first real end-to-end exercise of the cinematic system. It proves
the whole authoring stack:
  * spawn two real ships (Grayson's tarsus + a pirate's talon),
  * three back-to-back (non-overlapping) tracking camera segments on splines,
  * dual left/right ``.line()`` beats with AUTO portrait generation wired
    straight into each cue (placeholder backend => zero spend, offline),
  * positional firefight sfx, letterbox, fade in/out,
  * an ``end`` action that sets the campaign flag ``ambush_troy_seen``.

    python -m tools.cinematics.examples.example_ambush_troy
    python -m tools.cinematics.validate ambush_troy
    python -m tools.cinematics.preview  ambush_troy   # needs the game running
"""
from __future__ import annotations

try:
    from ..builder import Cinematic
    from .. import qc as qc_mod
except ImportError:  # allow direct execution
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from builder import Cinematic  # type: ignore
    import qc as qc_mod  # type: ignore


def director_qc(portrait_path, ref_path) -> dict:
    """Identity-consistency gate passed to the portrait pipeline.

    The pipeline calls this synchronously during generation, so it runs the
    dependency-free structural + perceptual-hash check (right size, not blank,
    plausibly the same framing/character as the blessed ``_ref.png``). The
    Cinematic Director then performs the REAL vision identity QC out-of-band by
    loading each generated PNG alongside the character's ``_ref.png`` and
    eyeballing them for same-face/hair/wardrobe consistency across every line.
    """
    result = qc_mod.default_check(portrait_path, ref_path)
    print(f"[director_qc] {portrait_path.name}: pass={result['pass']} "
          f":: {result['reason']}")
    return result


# Troy's sun sits at world (-4167, -41667, 2083). The cinematic is authored in
# a local frame around the origin, then translated so the whole scene plays
# right next to the star — turning the sun into a lateral key light we can
# compose into the shots. Ships AND cameras shift together, so the choreography
# (all look_at:"ship:...") is byte-for-byte identical; only the sun's on-screen
# direction changes. Chosen so the sun sits up/left/ahead of the action,
# ~29k units away so it's a strong key light without blowing out the frame
# (the first pass at ~11k put the star practically in the cockpit).
SCENE_OFFSET = (12333, -58167, 21083)


def _translate_scene(path, off):
    """Add ``off`` to every world position vector in the saved cinematic.

    Shifts spawn ``pos``, sfx ``pos``, and camera/actor keyframe ``pos``;
    leaves ``look_at:"ship:..."`` strings untouched (they resolve at runtime).
    """
    import json
    d = json.loads(open(path).read())

    def shift(v):
        return [v[0] + off[0], v[1] + off[1], v[2] + off[2]]

    for cue in d["timeline"]:
        if isinstance(cue.get("pos"), list):
            cue["pos"] = shift(cue["pos"])
        for k in cue.get("keys", []):
            if isinstance(k, dict) and isinstance(k.get("pos"), list):
                k["pos"] = shift(k["pos"])
    # ensure_ascii=False: keep literal UTF-8 (em-dashes) rather than \uXXXX
    # escapes, matching builder.save()'s canonical form.
    open(path, "w", encoding="utf-8").write(
        json.dumps(d, indent=2, ensure_ascii=False))


def build() -> str:
    # portrait_backend=None -> auto: gpt-image-2 when OPENAI_API_KEY is set,
    # else the offline placeholder. auto_voices -> MiniMax TTS when
    # MINIMAX_API_KEY is set, else the lines play silent (subtitle + portrait).
    c = Cinematic("ambush_troy", letterbox=True, skippable=True,
                  auto_portraits=True, portrait_backend=None,
                  qc_fn=director_qc, auto_voices=True)

    # --- open on black, swell the tension bed ------------------------------
    c.at(0.0).fade_in(1.5)
    c.at(0.0).music("../music/original/combat_05.wav")

    # --- the two ships: Grayson outbound, a raider lurking off his six -----
    c.at(0.2).spawn("hero", cls="tarsus", faction="civilian", pos=[0, 0, 0])
    c.at(0.2).spawn("raider", cls="talon", faction="pirate",
                    pos=[1400, 80, 1800])

    # =======================================================================
    # SEGMENT A (0.5 -> 5.0): establishing. Grayson cruises away from Troy.
    # =======================================================================
    c.at(0.5).subtitle("Troy jump lane - departure", dur=2.5)
    # Locked-off establishing angle (HARD CUT). The camera holds and tracks
    # Grayson's tarsus as it cruises out toward the lane — no camera glide;
    # the SHIP's motion through the static frame carries the shot.
    c.at(0.5).camera_path(
        keys=[{"pos": [900, -200, 400], "look_at": "ship:hero"}], dur=0.0)
    # Hero glides outbound along the lane.
    c.at(0.5).actor_path("hero", keys=[[0, 0, 0], [-400, 0, -1600]], dur=8.0)

    # =======================================================================
    # SEGMENT B (5.0 -> 11.5): the raider swoops in and hails. The exchange,
    # cut as a SHOT / REVERSE-SHOT with hard cuts instead of gliding cameras.
    # =======================================================================
    # Raider streaks in from the dark and settles onto the ~3 km standoff by
    # t~5.9, then holds station with a slow menacing drift through the exchange
    # (kept gently moving so it stays a live, correctly-oriented ship).
    c.at(5.0).actor_path("raider",
                         keys=[[1400, 80, 1800], [1500, 120, 750]], dur=0.9)
    c.at(6.0).actor_path("raider",
                         keys=[[1500, 120, 750], [1430, 110, 560]], dur=9.0)

    # HARD CUTS: a single-key, dur=0 camera cue snaps instantly instead of
    # gliding. Pirate speaks -> CUT to a side profile of his talon; Grayson
    # answers -> CUT back to a front 3/4 of the tarsus. look_at tracks the
    # ship so it stays framed even as it drifts (a locked-off coverage shot).
    c.at(6.0).camera_path(
        keys=[{"pos": [1880, 180, 720], "look_at": "ship:raider"}], dur=0.0)
    # (A ~2 s silent beat holds on the drifting talon after the taunt before
    # we cut back to Grayson — lets the threat land.)
    c.at(13.3).camera_path(
        keys=[{"pos": [-650, 110, -2150], "look_at": "ship:hero"}], dur=0.0)

    # The two-hander: pirate taunt (right) -> Grayson's defiant reply (left).
    c.at(6.0).line("pirate",
                   "Wrong lane to be flying alone, Troy-boy \u2014 cargo or corpse. Pick.",
                   emotion="cold sneer, cybernetic eye glinting, leaning into the comm",
                   side="right", dur=5.0)
    c.at(13.3).line("grayson",
                   "You picked the wrong tarsus. Come and get it.",
                   emotion="defiant, jaw set, half a grim smirk, eyes hard",
                   side="left", dur=3.3)

    # =======================================================================
    # SEGMENT C (11.5 -> 18.5): short firefight, Grayson barely escapes.
    # =======================================================================
    c.at(14.9).sfx("../sfx/cruise_windup.wav", pos=[1500, 120, 750])
    # HARD CUT to a chase angle from BEHIND the ships: Grayson and the raider
    # burn away from camera straight toward Troy's sun — fleeing into the light,
    # both ships rim-lit against the star.
    c.at(15.0).camera_path(
        keys=[{"pos": [500, -300, -300], "look_at": "ship:hero"}], dur=0.0)
    # Grayson breaks hard and burns for the horizon; the raider gives chase
    # from ~3 km back, closing to ~2 km — a distant pursuit, never a wingman.
    c.at(15.0).actor_path("hero",
                          keys=[[-400, 0, -1600], [-900, 40, -2200],
                                [-2000, 0, -3400]], dur=6.0)
    c.at(15.0).actor_path("raider",
                          keys=[[1430, 110, 560], [400, 90, -600],
                                [-500, 50, -1800]], dur=5.5)

    # Trading fire on the run.
    c.at(15.8).sfx("../sfx/laser_fire.wav", pos=[1100, 110, 300])
    c.at(16.6).sfx("../sfx/laser_fire.wav", pos=[500, 90, -500])
    c.at(17.4).sfx("../sfx/impact_armor.wav",   pos=[-900, 40, -2200])

    # One last defiant beat as he redlines the engines and slips away.
    # HARD CUT: tarsus silhouetted against the sun for the final line.
    c.at(17.9).camera_path(
        keys=[{"pos": [-515, -345, -1759], "look_at": "ship:hero"}], dur=0.0)
    c.at(17.9).line("grayson",
                    "Not today, scavenger.",
                    emotion="teeth gritted, sweat-lit, burning the engines past redline",
                    side="left", dur=2.6)
    c.at(19.1).sfx("../sfx/engine_hum.wav", pos=[-1300, 20, -2900])

    c.at(20.3).fade_out(1.5)
    c.at(22.0).end(actions=["set_flag:ambush_troy_seen"])

    # Post-cinematic world state (Studio Phase A2): Grayson pops out at
    # Troy Nav 8 mid-furball — 3 Confed Stilettos vs 3 Kilrathi Dralthi
    # (faction stance makes them brawl; neither side aggros the player).
    c.outcome(player_at_nav="Troy Nav 8",
              spawns=[{"class": "stiletto", "faction": "confed",   "count": 3},
                      {"class": "dralthi",  "faction": "kilrathi", "count": 3}])

    path = c.save()
    _translate_scene(path, SCENE_OFFSET)
    print(f"[ambush_troy] wrote {path} (translated next to Troy's sun)")
    return c.id


if __name__ == "__main__":
    build()

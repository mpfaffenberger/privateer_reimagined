"""Author Captain Seelig's Roman Lynch dismissal ambush (#388)."""
from __future__ import annotations

from pathlib import Path

from PIL import Image

from tools.cinematics.audio_timing import mp3_duration_seconds
from tools.cinematics.builder import Cinematic, repo_root
from tools.cinematics.publish import publish

CID = "m06_seelig_message"
ROOT = repo_root()
CIN_DIR = ROOT / "assets" / "cinematics"
VOICE_TAIL_S = 0.35
DIALOGUE_GAP_S = 0.25


def portrait_qc(candidate: Path, reference: Path) -> dict:
    """Reject technically unusable portraits before visual identity review."""
    try:
        with Image.open(candidate) as cand, Image.open(reference) as ref:
            if cand.size != (512, 640):
                return {"pass": False, "reason": f"candidate size is {cand.size}, expected 512x640"}
            if cand.mode != "RGBA":
                return {"pass": False, "reason": f"candidate mode is {cand.mode}, expected RGBA"}
            if ref.width < 1 or ref.height < 1:
                return {"pass": False, "reason": "reference image is empty"}
    except Exception as exc:
        return {"pass": False, "reason": f"portrait could not be loaded: {exc}"}
    return {"pass": True, "reason": "technical pass; visual identity QC follows"}


def add_follow_camera(c: Cinematic, t: float, actor: str,
                      offset: list[float], dur: float) -> dict:
    c.at(t).camera_path([offset], look_at=f"ship:{actor}", ease="linear", dur=dur)
    cue = c._timeline[-1]
    cue["follow"] = actor
    return cue


def add_line(c: Cinematic, *, t: float, min_dur: float, actor: str,
             offset: list[float], character: str, text: str, voice_text: str,
             emotion: str, voice_emotion: str, side: str, speed: float) -> float:
    """Add a voiced close-up and return the next non-overlapping cue time."""
    camera = add_follow_camera(c, t, actor, offset, min_dur)
    c.at(t).line(character, text, dur=min_dur, emotion=emotion, side=side,
                 voice_text=voice_text, voice_emotion=voice_emotion,
                 voice_speed=speed)
    line = c._timeline[-1]
    voice_path = CIN_DIR / line["voice_file"]
    duration = max(min_dur, mp3_duration_seconds(voice_path) + VOICE_TAIL_S)
    camera["dur"] = duration
    line["dur"] = duration
    return t + duration + DIALOGUE_GAP_S


def verify_assets(c: Cinematic) -> None:
    for index, cue in enumerate(item for item in c._timeline if item["cmd"] == "line"):
        for field in ("portrait", "voice_file"):
            relative = cue.get(field)
            path = CIN_DIR / relative if relative else None
            if path is None or not path.is_file() or path.stat().st_size == 0:
                raise RuntimeError(f"line {index} missing mandatory {field}: {path}")


TRIGGER = {
    "cinematic": CID,
    "once": True,
    "cooldown_s": 0,
    "when": {
        "system": "pentonville",
        "near_nav": {"nav": "119CE Jump", "radius_m": 20000},
        "requires_flags": ["m06_active"],
        "forbids_flags": [
            "m06_message_delivered",
            "killed:seelig",
            f"{CID}_seen",
        ],
    },
}


def main() -> None:
    output = CIN_DIR / f"{CID}.json"
    if output.exists():
        raise RuntimeError(f"refusing to reuse existing cinematic id: {CID}")

    c = Cinematic(CID, letterbox=True, skippable=True,
                  auto_portraits=True, auto_voices=True, qc_fn=portrait_qc)
    c.location("pentonville", "119CE Jump")
    c.outcome(
        player_at_nav="119CE Jump",
        spawns=[{
            "class": "talon",
            "faction": "pirate",
            "count": 1,
            "hostile": True,
            "cleared_flag": "killed:seelig",
        }],
    )

    player_pos = [133333, -61667, 81333]
    seelig_pos = [133333, -61200, 85000]
    c.at(0.0).spawn("grayson_ship", cls="$player", faction="civilian", pos=player_pos)
    c.at(0.0).spawn("seelig_ship", cls="talon", faction="pirate", pos=seelig_pos)
    c.at(0.0).fade_in(1.2).music("../music/original/combat_05.wav").sfx("../sfx/engine_hum.wav")
    add_follow_camera(c, 0.0, "seelig_ship", [0, 850, -2100], 3.5)

    t = 3.5
    t = add_line(
        c, t=t, min_dur=4.0, actor="seelig_ship", offset=[270, 95, -125],
        character="seelig",
        text="Burrows. Roman said you'd be carrying a message. Let's hear it.",
        voice_text="Burrows.<#0.3#>Roman said you'd be carrying a message.<#0.5#>Let's hear it.",
        emotion="controlled professional confidence, cool direct gaze",
        voice_emotion="neutral", side="right", speed=0.95)
    t = add_line(
        c, t=t, min_dur=5.0, actor="grayson_ship", offset=[-255, 90, -130],
        character="grayson",
        text="Roman Lynch says: ‘I am profoundly disappointed in you.’ That's the whole message. He really knows how to make a man feel valued.",
        voice_text="Roman Lynch says:<#0.5#>I am profoundly disappointed in you.<#0.7#>That's the whole message.<#0.3#>He really knows how to make a man feel valued.",
        emotion="dry sarcasm, faint crooked smile, watching Seelig carefully",
        voice_emotion="neutral", side="left", speed=0.95)
    t = add_line(
        c, t=t, min_dur=4.5, actor="seelig_ship", offset=[-275, 105, 120],
        character="seelig",
        text="Profoundly disappointed. After ten years. You can carry my reply.",
        voice_text="Profoundly disappointed.<#0.7#>After ten years.<#0.5#>You can carry my reply.",
        emotion="professional restraint snapping into cold focused fury",
        voice_emotion="angry", side="right", speed=0.92)

    c.at(t - 0.25).sfx("../sfx/lock_seeking.wav")
    add_follow_camera(c, t, "seelig_ship", [0, 650, -1800], 3.0)
    c.at(t + 0.2).sfx("../sfx/laser_fire.wav", pos=seelig_pos)
    c.at(t + 0.65).sfx("../sfx/laser_fire.wav", pos=seelig_pos)
    c.at(t + 1.05).sfx("../sfx/impact_shield.wav", pos=player_pos)
    t += 3.0

    t = add_line(
        c, t=t, min_dur=5.0, actor="grayson_ship", offset=[250, 85, 135],
        character="grayson",
        text="Some people take performance reviews way too personally. Oh well... guess I'm gonna have to blast one more idiot...",
        voice_text="Some people take performance reviews way too personally.<#0.5#>Oh well...<#0.5#>guess I'm gonna have to blast one more idiot...",
        emotion="deadpan resignation turning into a dangerous half-smile",
        voice_emotion="disgusted", side="left", speed=0.95)

    c.at(t).fade_out(1.2)
    c.at(t + 1.2).end(actions=[
        f"set_flag:{CID}_seen",
        "set_flag:m06_message_delivered",
    ])
    verify_assets(c)
    # Cinematic + trigger land together or not at all (#368).
    publish(c, TRIGGER)
    print(CID)


if __name__ == "__main__":
    main()

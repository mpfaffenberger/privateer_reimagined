"""Author the New Constantinople M04 customs-fleet interception (#347)."""
from __future__ import annotations

import json
from pathlib import Path

from PIL import Image

from tools.cinematics.builder import Cinematic, repo_root

CID = "studio_req_1785636336"
ROOT = repo_root()
CIN_DIR = ROOT / "assets" / "cinematics"


def portrait_qc(candidate: Path, reference: Path) -> dict:
    """Reject structurally unusable art before director visual identity QC."""
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
    return {"pass": True, "reason": "technical pass; director performs visual identity/expression QC"}


def add_follow_camera(c: Cinematic, t: float, actor: str, offset: list[float], dur: float) -> None:
    c.at(t).camera_path([offset], look_at=f"ship:{actor}", ease="linear", dur=dur)
    c._timeline[-1]["follow"] = actor


def add_line(c: Cinematic, *, t: float, dur: float, actor: str, offset: list[float],
             character: str, text: str, voice_text: str, emotion: str,
             voice_emotion: str, side: str, speed: float) -> None:
    add_follow_camera(c, t, actor, offset, dur)
    c.at(t).line(character, text, dur=dur, emotion=emotion, side=side,
                 voice_text=voice_text, voice_emotion=voice_emotion,
                 voice_speed=speed)


def main() -> None:
    out_path = CIN_DIR / f"{CID}.json"
    if out_path.exists():
        raise RuntimeError(f"refusing to reuse existing cinematic id: {CID}")

    c = Cinematic(CID, letterbox=True, skippable=True,
                  auto_portraits=True, auto_voices=True, qc_fn=portrait_qc)
    c.location("new_constantinople", "44-P-1M Jump")
    c.outcome(
        player_at_nav="44-P-1M Jump",
        spawns=[
            {"class": "broadsword", "faction": "confed", "count": 3, "hostile": True},
            {"class": "stiletto", "faction": "confed", "count": 5, "hostile": True},
            {"class": "paradigm", "faction": "confed", "count": 3, "hostile": True},
        ],
    )

    actors = [
        ("grayson_ship", "$player", [-66667, 33333, 64667]),
        ("paradigm_lead", "paradigm", [-66667, 33933, 71667]),
        ("paradigm_port", "paradigm", [-70667, 34333, 72667]),
        ("paradigm_starboard", "paradigm", [-62667, 34333, 72667]),
        ("broadsword_port", "broadsword", [-70667, 32633, 70667]),
        ("broadsword_center", "broadsword", [-66667, 32333, 70667]),
        ("broadsword_starboard", "broadsword", [-62667, 32633, 70667]),
        ("stiletto_far_port", "stiletto", [-71667, 33333, 69167]),
        ("stiletto_port", "stiletto", [-69167, 33733, 69667]),
        ("stiletto_center", "stiletto", [-66667, 32933, 69167]),
        ("stiletto_starboard", "stiletto", [-64167, 33733, 69667]),
        ("stiletto_far_starboard", "stiletto", [-61667, 33333, 69167]),
    ]
    for actor, cls, pos in actors:
        c.at(0.0).spawn(actor, cls=cls, faction="civilian" if cls == "$player" else "confed", pos=pos)

    c.at(0.0).fade_in(1.2).music("../music/original/combat_06.wav").sfx("../sfx/engine_hum.wav")
    fleet = ",".join(actor for actor, cls, _ in actors if cls != "$player")
    c.at(0.0).camera_path([[0, 5200, -10000]], look_at="ship:paradigm_lead",
                          ease="linear", dur=4.0)
    c._timeline[-1]["follow"] = fleet

    add_line(c, t=4.0, dur=4.2, actor="paradigm_lead", offset=[1250, 500, -700],
             character="rourke",
             text="Unidentified privateer, cut thrust and prepare to be searched. This is a full-spectrum customs inspection.",
             voice_text="Unidentified privateer, cut thrust and prepare to be searched.<#0.5#>This is a full-spectrum customs inspection.",
             emotion="cold authority, controlled suspicion, speaking a rehearsed inspection order",
             voice_emotion="neutral", side="right", speed=0.95)

    add_line(c, t=8.3, dur=4.0, actor="grayson_ship", offset=[255, 90, -125],
             character="grayson",
             text="A full fleet for one cargo hold? I'm flattered. Tayla said the route was clear.",
             voice_text="A full fleet for one cargo hold?<#0.3#>I'm flattered. Tayla said the route was clear.",
             emotion="dry amusement masking alarm, one eyebrow raised at the impossible odds",
             voice_emotion="surprised", side="left", speed=1.0)

    c.at(12.15).sfx("../sfx/lock_seeking.wav")
    add_line(c, t=12.4, dur=4.3, actor="paradigm_lead", offset=[-1250, 430, -760],
             character="rourke",
             text="Your hold contains twenty-five units of Brilliance. Bribery has a shelf life, Mr. Burrows.",
             voice_text="Your hold contains twenty-five units of Brilliance.<#0.5#>Bribery has a shelf life, Mr. Burrows.",
             emotion="grim confirmation, faint contempt, eyes fixed on scan results",
             voice_emotion="disgusted", side="right", speed=0.95)

    add_line(c, t=16.8, dur=5.0, actor="grayson_ship", offset=[-250, 105, -135],
             character="grayson",
             text="Don't you guys have Kilrathi to shoot at? No wonder we're losing the war...",
             voice_text="Don't you guys have Kilrathi to shoot at?<#0.5#>No wonder we're losing the war...",
             emotion="reckless deadpan sarcasm, cornered but unable to resist the jab",
             voice_emotion="disgusted", side="left", speed=0.95)

    add_line(c, t=21.9, dur=4.1, actor="paradigm_lead", offset=[1150, 520, 820],
             character="rourke",
             text="We do. They complain less. All units, mark the smuggler hostile and open fire.",
             voice_text="We do.<#0.3#>They complain less.<#0.5#>All units, mark the smuggler hostile and open fire.",
             emotion="icy anger held under military discipline, issuing a lethal fleet command",
             voice_emotion="angry", side="right", speed=1.05)

    add_line(c, t=26.1, dur=3.5, actor="grayson_ship", offset=[245, 80, 145],
             character="grayson",
             text="There it is—the famous Confed sense of proportion.",
             voice_text="There it is.<#0.3#>The famous Confed sense of proportion.",
             emotion="black humor and braced determination as weapons lock on",
             voice_emotion="fearful", side="left", speed=1.05)

    c.at(29.7).camera_path([[0, 4200, -8500]], look_at="ship:grayson_ship",
                           ease="linear", dur=4.3)
    c._timeline[-1]["follow"] = fleet
    c.at(29.9).sfx("../sfx/laser_fire.wav", pos=[-71667, 33333, 69167])
    c.at(30.25).sfx("../sfx/laser_fire.wav", pos=[-66667, 32333, 70667])
    c.at(30.6).sfx("../sfx/laser_fire.wav", pos=[-61667, 33333, 69167])
    c.at(31.0).sfx("../sfx/impact_shield.wav", pos=[-66667, 33333, 64667])
    c.at(32.8).fade_out(1.2)
    c.at(34.0).end(actions=[f"set_flag:{CID}_seen"])

    lines = [cue for cue in c._timeline if cue["cmd"] == "line"]
    for index, cue in enumerate(lines):
        for field in ("portrait", "voice_file"):
            if not cue.get(field):
                raise RuntimeError(f"line {index} missing mandatory {field}; refusing to save")
            asset = CIN_DIR / cue[field]
            if not asset.is_file() or asset.stat().st_size == 0:
                raise RuntimeError(f"line {index} {field} does not resolve: {asset}")

    c.save()

    triggers_path = CIN_DIR / "triggers.json"
    triggers_doc = json.loads(triggers_path.read_text(encoding="utf-8"))
    triggers = triggers_doc.get("triggers")
    if not isinstance(triggers, list):
        raise RuntimeError("triggers.json has no triggers array; refusing to clobber it")
    if any(item.get("cinematic") == CID for item in triggers):
        raise RuntimeError(f"trigger for {CID} already exists; refusing duplicate merge")
    triggers.append({
        "cinematic": CID,
        "once": True,
        "cooldown_s": 0,
        "when": {
            "system": "new_constantinople",
            "cargo": [{"commodity": "brilliance", "min_units": 25}],
            "near_nav": {"nav": "44-P-1M Jump", "radius_m": 20000},
            "requires_flags": ["m04_active"],
            "forbids_flags": [f"{CID}_seen"],
        },
    })
    triggers_path.write_text(json.dumps(triggers_doc, indent=2, ensure_ascii=False) + "\n",
                             encoding="utf-8")
    print(CID)


if __name__ == "__main__":
    main()

"""MiniMax text-to-speech for cinematic dialogue lines.

The C++ engine never touches this code — it only loads pre-generated audio by
path (assets/cinematics/audio/*.mp3), exactly like the portrait PNGs. This
module renders each ``line`` cue's spoken audio via MiniMax ``t2a_v2`` using the
character's cloned voice_id.

It deliberately mirrors the game's EXISTING MiniMax pipeline
(``tools/synth_scenario_voices.py``, ``tools/synth_comms.py``): same endpoint
(``/v1/t2a_v2``), same model (``speech-2.8-hd``), and the same cloned cast
voice_ids. Auth is ``MINIMAX_API_KEY``.

MiniMax TTS markup (https://platform.minimax.io/docs/api-reference/speech-t2a-http):
* **Pauses:** ``<#N.N#>`` inserts a pause of N.N seconds in the spoken audio.
  Example: ``'Pender's Star Jump.<#0.5#>Rough corridor.'`` pauses 0.5s after
  the name. The engine's subtitle renderer strips ``<#...#>`` tags automatically,
  so they never appear on screen. Use ``<#0.3#>`` for a beat, ``<#0.5#>`` for a
  noticeable pause, ``<#1.0#>`` for a dramatic beat.
* **Emotion:** pass ``--emotion`` with one of: happy, sad, angry, fearful,
  disgusted, surprised, neutral. Match the emotion to the scene.
* **Interjections:** inline markers like ``[laughter]``, ``[sigh]``, ``[cough]``,
  ``[breath]``, ``[gasps]`` are spoken as sounds, not read as words. Use sparingly.
* **Speed:** ``--speed 0.9`` for slower/calmer, ``--speed 1.1`` for urgent/fast.

Behaviour:
* content-hash cached + idempotent — re-runs with the same (voice, text) are
  free (no API spend);
* degrades to a silent no-op (returns ``None``) when ``MINIMAX_API_KEY`` is
  absent, so the offline authoring flow still works — the engine just plays the
  line with subtitle + portrait and no audio.

Delivery direction (Studio Phase C note): the per-line ``emotion`` text is
deliberately NOT sent to the TTS call. ``t2a_v2``'s ``voice_setting`` only
accepts a small fixed emotion enum, it behaves unreliably with cloned voices
on ``speech-2.8-hd``, and the Studio's emotions are free-form prose ("cold
sneer, leaning into the comm") that can't map onto it. The supported delivery
knobs are ``voice_id`` + ``speed`` — emotion drives the PORTRAIT only.
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
from typing import Optional

T2A_URL = "https://api.minimax.io/v1/t2a_v2"
MODEL = "speech-2.8-hd"

# character-bible id -> cloned MiniMax voice_id. These are the game's cloned
# cast voices (see tools/synth_scenario_voices.py VOICE_MAP). A character may
# override this with a "voice_id" field in assets/data/characters.json.
# NOTE: the named bar cast map to their ORIGINAL 1993 actors, cloned from the
# extracted CONV/*.VPK bar speech (assets/speech/bar_by_speaker/<voice>/, see
# assets/speech/clone_tests/results.json). Do NOT point these at the generic
# PrivFlight* pilot-chatter voices — those are anonymous NPC wingmen and three
# of the women previously collided on a single synth voice.
DEFAULT_VOICE_MAP = {
    "grayson":       "PrivBarPc01",       # the player-character
    "sandoval":      "PrivBarMonte01",    # original actor ('monte')
    "tayla":         "PrivBarTayla01",    # original actor
    "lynch":         "PrivBarRoman01",    # original actor ('roman')
    "masterson":     "PrivBarMastersn01", # original actor
    "murphy":        "PrivBarLynn01",     # original actor ('lynn')
    "monkhouse":     "PrivBarMonkhous01", # original actor
    "cross":         "PrivBarTaryn01",    # original actor ('taryn')
    "terrell":       "PrivBarTerrel01",   # original actor
    "goodin":        "PrivBarSandra01",   # original actor ('sandra')
    "miggs":         "PrivBarMiggs01",    # original actor
    "pirate":        "PrivFlightV1501",   # raider
    "militia":       "PrivFlightV0101",
    "confed":        "PrivFlightV0801",
    "bounty_hunter": "PrivFlightV0501",
    "archive_computer": "ttv-voice-2026072706022926-M38Ep5YM",  # designed Oxford terminal
    # Phase-2 cast (#176 / #178 auditions, 2026-07)
    "quist":         "PrivBarMercgirl01", # flashy ace (won audition - shared w/ krieg)
    "sian":          "PrivCustomSian02",  # real-life clone (noise-reduced), Drayman captain
    "chen_wl":       "female_1",          # preset: plague doctor supplier
    "ferao":         "PrivFlightV0601",  # fast-talking smuggler (won audition)
}
_FALLBACK_VOICE = "PrivFlightV0801"

# MiniMax rejects unknown emotions by SILENTLY returning empty audio (cost a
# whole audition session to discover). Validate loudly instead.
VALID_EMOTIONS = {"happy", "sad", "angry", "fearful", "disgusted",
                  "surprised", "neutral"}

_CACHE_DIR = Path(__file__).resolve().parent / ".cache_voice"


def _repo_root() -> Path:
    # tools/cinematics/voices.py -> repo root is three parents up.
    return Path(__file__).resolve().parents[2]


def available() -> bool:
    """True when a MiniMax key is present (real synthesis possible)."""
    return bool(os.environ.get("MINIMAX_API_KEY"))


def voice_for(character: str, bible: Optional[dict] = None) -> str:
    """Resolve the cloned voice_id for a character (bible override wins)."""
    if bible:
        entry = bible.get(character) or {}
        if entry.get("voice_id"):
            return entry["voice_id"]
    return DEFAULT_VOICE_MAP.get(character, _FALLBACK_VOICE)


def _t2a(key: str, text: str, voice_id: str, speed: float,
         timeout: float, emotion: str = "") -> Optional[bytes]:
    import httpx

    voice_setting = {"voice_id": voice_id, "speed": speed, "vol": 1, "pitch": 0}
    if emotion:
        voice_setting["emotion"] = emotion
    body = {
        "model": MODEL,
        "text": text,
        "voice_setting": voice_setting,
        "audio_setting": {"sample_rate": 32000, "bitrate": 128000,
                          "format": "mp3", "channel": 1},
        "output_format": "hex",
    }
    headers = {"Authorization": f"Bearer {key}", "Content-Type": "application/json"}
    with httpx.Client(timeout=timeout) as client:
        r = client.post(T2A_URL, json=body, headers=headers)
        if r.status_code != 200:
            raise RuntimeError(f"MiniMax t2a error {r.status_code}: {r.text[:300]}")
        d = r.json()
    h = (d.get("data") or {}).get("audio", "")
    return bytes.fromhex(h) if h else None


def gen_line_voice(character: str, text: str, out_rel: Optional[str] = None, *,
                   voice_id: Optional[str] = None, speed: float = 1.0,
                   emotion: str = "",
                   bible: Optional[dict] = None, cache: bool = True,
                   force: bool = False, timeout: float = 120.0) -> Optional[str]:
    """Synthesize ``text`` in ``character``'s cloned voice.

    Returns the cinematics-relative path ("audio/xxx.mp3") to drop into a
    ``line`` cue's ``voice_file`` field, or ``None`` if it couldn't be produced
    (no key / API failure) so the caller can leave the line silent.

    ``emotion`` sets the MiniMax voice emotion (happy, sad, angry, fearful,
    disgusted, surprised, neutral). Empty string = no emotion override.

    Idempotent + content-hash cached on (model, voice_id, speed, emotion, text).
    """
    if emotion and emotion not in VALID_EMOTIONS:
        print(f"[voice] INVALID emotion {emotion!r} for {character!r} — MiniMax "
              f"would silently return no audio. Valid: {sorted(VALID_EMOTIONS)}")
        return None
    vid = voice_id or voice_for(character, bible)
    digest = hashlib.sha256(
        f"{MODEL}|{vid}|{speed}|{emotion}|{text}".encode("utf-8")).hexdigest()[:16]
    if out_rel is None:
        out_rel = f"audio/{character}_{digest}.mp3"

    out_path = _repo_root() / "assets" / "cinematics" / out_rel
    out_path.parent.mkdir(parents=True, exist_ok=True)

    # cache / idempotency ----------------------------------------------------
    cache_file = _CACHE_DIR / f"{digest}.mp3"
    if cache and cache_file.is_file():
        out_path.write_bytes(cache_file.read_bytes())
        print(f"[voice] CACHE HIT ({digest}) -> {out_rel}  (no API spend)")
        return out_rel
    if not force and out_path.is_file() and out_path.stat().st_size > 0:
        print(f"[voice] exists, skip -> {out_rel}")
        return out_rel

    key = os.environ.get("MINIMAX_API_KEY")
    if not key:
        print(f"[voice] MINIMAX_API_KEY not set -> no audio for {character!r} "
              f"(line plays silent with subtitle + portrait)")
        return None

    try:
        audio = _t2a(key, text, vid, speed, timeout, emotion)
    except Exception as e:  # noqa: BLE001 - degrade, never crash authoring
        print(f"[voice] synth failed for {character!r}: {e}")
        return None
    if not audio:
        print(f"[voice] synth returned no audio for {character!r}")
        return None

    out_path.write_bytes(audio)
    if cache:
        _CACHE_DIR.mkdir(parents=True, exist_ok=True)
        cache_file.write_bytes(audio)
    print(f"[voice] {character} ({vid}) -> {out_rel}  ({len(audio)} bytes)")
    return out_rel


# --------------------------------------------------------------------------
# tiny CLI: python -m tools.cinematics.voices <character> "<line>" [--speed S]
# --------------------------------------------------------------------------
def _main() -> None:
    import argparse

    ap = argparse.ArgumentParser(description="Synthesize one cinematic voice line via MiniMax.")
    ap.add_argument("character")
    ap.add_argument("text")
    ap.add_argument("--out", default=None, help="cinematics-relative path (audio/..mp3)")
    ap.add_argument("--voice-id", default=None)
    ap.add_argument("--speed", type=float, default=1.0)
    ap.add_argument("--emotion", default="", help="happy, sad, angry, fearful, disgusted, surprised, neutral")
    ap.add_argument("--no-cache", action="store_true", help="Bypass cache AND force regenerate")
    args = ap.parse_args()

    bible = None
    try:
        from . import portraits as portraits_mod  # reuse bible loader
        bible = portraits_mod.load_bible()
    except Exception:
        pass

    out = gen_line_voice(args.character, args.text, out_rel=args.out,
                         voice_id=args.voice_id, speed=args.speed,
                         emotion=args.emotion,
                         bible=bible, cache=not args.no_cache,
                         force=args.no_cache)
    if out:
        print(f"OK -> assets/cinematics/{out}")
    else:
        raise SystemExit(1)


if __name__ == "__main__":
    _main()

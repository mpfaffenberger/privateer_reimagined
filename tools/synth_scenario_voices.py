#!/usr/bin/env python3
"""Synthesize per-line voice clips for scripted scenario dialogue.

Each scenario in assets/data/scripted_encounters.json has a `dialogue` list of
{voice, line}. This renders every line with the cloned voice mapped from its
`voice` tag and writes:
  - assets/speech/scenarios/<scenario_id>_t<NN>.mp3
  - assets/data/scenario_voice.json  ->  { scenario_id: [mp3 path per turn] }

The engine (scripted_encounters.cpp) plays clip N when it shows dialogue turn N.
Idempotent: skips lines whose mp3 already exists. Auth: MINIMAX_API_KEY.
"""
from __future__ import annotations
import json
import os
import subprocess
from pathlib import Path

T2A = "https://api.minimax.io/v1/t2a_v2"
SCN = Path("assets/data/scripted_encounters.json")
OUT_DIR = Path("assets/speech/scenarios")
OUT_MANIFEST = Path("assets/data/scenario_voice.json")

# dialogue `voice` tag -> cloned voice_id. Faction tags use the primary cast
# voice; *_f variants use the female placeholder.
VOICE_MAP = {
    "confed": "PrivFlightV0801",
    "confed_f": "PrivMerchantFem01",
    "kilrathi": "PrivFlightV0901",
    "militia": "PrivFlightV0101",
    "merchant": "PrivMerchantFem01",
    "merchant_m": "PrivFlightV1401",
    "pirate": "PrivFlightV1501",
    "bounty_hunter": "PrivFlightV0501",
    "retro": "PrivFlightV0401",
    "steltek": "PrivFlightV1001",
    "player": "PrivBarPc01",
}


def t2a(key, text, voice_id):
    body = {
        "model": "speech-2.8-hd", "text": text,
        "voice_setting": {"voice_id": voice_id, "speed": 1, "vol": 1, "pitch": 0},
        "audio_setting": {"sample_rate": 32000, "bitrate": 128000,
                          "format": "mp3", "channel": 1},
        "output_format": "hex",
    }
    r = subprocess.run(
        ["curl", "-s", "--request", "POST", "--url", T2A,
         "--header", f"Authorization: Bearer {key}",
         "--header", "Content-Type: application/json",
         "--data", json.dumps(body)], capture_output=True)
    d = json.loads(r.stdout.decode())
    h = (d.get("data") or {}).get("audio", "")
    return bytes.fromhex(h) if h else None


def main():
    key = os.environ.get("MINIMAX_API_KEY")
    if not key:
        raise SystemExit("MINIMAX_API_KEY not set")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    scn = json.loads(SCN.read_text())
    manifest = {}
    made = skipped = 0
    for s in scn["scenarios"]:
        sid = s["id"]
        paths = []
        for i, turn in enumerate(s.get("dialogue", [])):
            vid = VOICE_MAP.get(turn.get("voice", ""), "PrivFlightV0801")
            out = OUT_DIR / f"{sid}_t{i:02d}.mp3"
            if not out.exists():
                audio = t2a(key, turn["line"], vid)
                if audio:
                    out.write_bytes(audio); made += 1
                else:
                    print(f"  ! synth failed: {sid} t{i}"); continue
            else:
                skipped += 1
            paths.append(str(out))
        manifest[sid] = paths
    OUT_MANIFEST.write_text(json.dumps(manifest, indent=1))
    total = sum(len(v) for v in manifest.values())
    print(f"[scenario-voice] {len(manifest)} scenarios, {total} clips "
          f"({made} new, {skipped} cached) -> {OUT_MANIFEST}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Synthesize the player's comm-menu lines in the pilot voice (PrivBarPc01).

Reads assets/data/player_comms.json ({friendly:[...], hostile:[...]}) and renders
each line, writing:
  - assets/speech/player_comms/<sha1>.mp3
  - assets/data/player_comms_voice.json  ->  { "<line text>": "<mp3 path>" }

comms_menu plays the clip matching the chosen line. Idempotent. Auth: MINIMAX_API_KEY.
"""
from __future__ import annotations
import hashlib
import json
import os
import subprocess
from pathlib import Path

T2A = "https://api.minimax.io/v1/t2a_v2"
VOICE = "PrivBarPc01"
SRC = Path("assets/data/player_comms.json")
OUT_DIR = Path("assets/speech/player_comms")
MANIFEST = Path("assets/data/player_comms_voice.json")


def t2a(key, text):
    body = {"model": "speech-2.8-hd", "text": text,
            "voice_setting": {"voice_id": VOICE, "speed": 1, "vol": 1, "pitch": 0},
            "audio_setting": {"sample_rate": 32000, "bitrate": 128000,
                              "format": "mp3", "channel": 1},
            "output_format": "hex"}
    r = subprocess.run(["curl", "-s", "--request", "POST", "--url", T2A,
                        "--header", f"Authorization: Bearer {key}",
                        "--header", "Content-Type: application/json",
                        "--data", json.dumps(body)], capture_output=True)
    h = (json.loads(r.stdout.decode()).get("data") or {}).get("audio", "")
    return bytes.fromhex(h) if h else None


def main():
    key = os.environ.get("MINIMAX_API_KEY")
    if not key:
        raise SystemExit("MINIMAX_API_KEY not set")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    data = json.loads(SRC.read_text())
    lines = []
    for bucket in ("friendly", "hostile"):
        lines += data.get(bucket, [])
    manifest = {}
    made = skipped = 0
    for line in lines:
        h = hashlib.sha1(line.encode()).hexdigest()[:12]
        out = OUT_DIR / f"{h}.mp3"
        if not out.exists():
            audio = t2a(key, line)
            if audio:
                out.write_bytes(audio); made += 1
            else:
                print(f"  ! failed: {line}"); continue
        else:
            skipped += 1
        manifest[line] = str(out)
    # ensure_ascii=False: the engine's hand-rolled json.h reader does NOT decode
    # \uXXXX escapes, so non-ASCII (em-dash, smart quotes) must be raw UTF-8.
    MANIFEST.write_text(json.dumps(manifest, indent=1, ensure_ascii=False))
    print(f"[player-comms] {len(manifest)} lines ({made} new, {skipped} cached) -> {MANIFEST}")


if __name__ == "__main__":
    main()

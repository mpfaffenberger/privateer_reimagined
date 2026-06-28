#!/usr/bin/env python3
"""Synthesize rumor responses in EVERY cloned voice.

When the player asks a ship/base for rumors, the recipient replies in ITS OWN
voice with either a "no news" line (the common case) or a "lead" line (when a
lead was generated). This renders assets/data/rumor_lines.json {no_news, lead}
once per voice in voice_bank.json, and writes:

  assets/speech/rumors/<voice>_<bucket><i>.mp3
  assets/data/rumor_responses.json
    { "<voice_id>": { "no_news": [{text,clip}], "lead": [{text,clip}] } }

Idempotent. Raw UTF-8 (engine json.h has no \\uXXXX). Auth: MINIMAX_API_KEY.
"""
from __future__ import annotations
import json
import os
import subprocess
from pathlib import Path

T2A = "https://api.minimax.io/v1/t2a_v2"
LINES = Path("assets/data/rumor_lines.json")
BANK = Path("assets/data/voice_bank.json")
OUT_DIR = Path("assets/speech/rumors")
OUT = Path("assets/data/rumor_responses.json")


def t2a(key, text, voice_id):
    body = {"model": "speech-2.8-hd", "text": text,
            "voice_setting": {"voice_id": voice_id, "speed": 1, "vol": 1, "pitch": 0},
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
    lines = json.loads(LINES.read_text())
    voices = sorted(json.loads(BANK.read_text())["by_voice"].keys())
    manifest = {}
    made = skipped = 0
    for vid in voices:
        manifest[vid] = {}
        for bucket in ("no_news", "lead"):
            entries = []
            for i, text in enumerate(lines[bucket]):
                out = OUT_DIR / f"{vid}_{bucket}{i}.mp3"
                if not out.exists():
                    audio = t2a(key, text, vid)
                    if audio:
                        out.write_bytes(audio); made += 1
                    else:
                        print(f"  ! fail {vid} {bucket}{i}"); continue
                else:
                    skipped += 1
                entries.append({"text": text, "clip": str(out)})
            manifest[vid][bucket] = entries
    OUT.write_text(json.dumps(manifest, indent=1, ensure_ascii=False))
    print(f"[rumor-responses] {len(voices)} voices ({made} new, {skipped} cached) -> {OUT}")


if __name__ == "__main__":
    main()

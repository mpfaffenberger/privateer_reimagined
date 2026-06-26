#!/usr/bin/env python3
"""Synthesize the generated comms corpus via MiniMax t2a_v2.

- standalone lines -> assets/speech/generated/audio/<id>.mp3
- conversations    -> per-turn clips + a stitched scene MP3 (small gaps between
                      turns) in assets/speech/generated/scenes/
Tracks billed usage via each response's extra_info.usage_characters and prints
a running + final total. Idempotent: skips clips whose mp3 already exists.

Auth: MINIMAX_API_KEY env var.  Options: --limit N (lines), --convs N.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

T2A = "https://api.minimax.io/v1/t2a_v2"
GEN = Path("assets/speech/generated")
AUDIO = GEN / "audio"
SCENES = GEN / "scenes"


def t2a(key, text, voice_id):
    body = {
        "model": "speech-2.8-hd", "text": text, "stream": False,
        "voice_setting": {"voice_id": voice_id, "speed": 1, "vol": 1, "pitch": 0},
        "audio_setting": {"sample_rate": 32000, "bitrate": 128000,
                          "format": "mp3", "channel": 1},
        "output_format": "hex",
    }
    r = subprocess.run(
        ["curl", "-s", "--request", "POST", "--url", T2A,
         "--header", f"Authorization: Bearer {key}",
         "--header", "Content-Type: application/json",
         "--data", json.dumps(body)],
        capture_output=True)
    try:
        d = json.loads(r.stdout.decode())
    except Exception:
        return None, 0, "bad json"
    h = (d.get("data") or {}).get("audio", "")
    used = (d.get("extra_info") or {}).get("usage_characters", 0)
    if not h:
        return None, used, str((d.get("base_resp") or {}).get("status_msg", "?"))
    return bytes.fromhex(h), used, "ok"


def stitch(turn_files, out_path, gap=0.45):
    """Concatenate turn mp3s with `gap` seconds of silence between them."""
    sil = SCENES / "_gap.mp3"
    if not sil.exists():
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi",
                        "-i", "anullsrc=r=32000:cl=mono", "-t", str(gap),
                        "-b:a", "128k", str(sil)], capture_output=True)
    seq = []
    for i, tf in enumerate(turn_files):
        seq.append(tf)
        if i < len(turn_files) - 1:
            seq.append(sil)
    lst = SCENES / "_list.txt"
    lst.write_text("".join(f"file '{Path(p).resolve()}'\n" for p in seq))
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat",
                    "-safe", "0", "-i", str(lst), "-c", "copy", str(out_path)],
                   capture_output=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--limit", type=int, default=None, help="max standalone lines")
    ap.add_argument("--convs", type=int, default=None, help="max conversations")
    args = ap.parse_args()
    key = os.environ.get("MINIMAX_API_KEY")
    if not key:
        print("MINIMAX_API_KEY not set", file=sys.stderr)
        return 1
    AUDIO.mkdir(parents=True, exist_ok=True)
    SCENES.mkdir(parents=True, exist_ok=True)
    corpus = json.loads((GEN / "comms.json").read_text())

    total_used = ok = fail = 0
    lines = corpus["lines"][:args.limit] if args.limit else corpus["lines"]
    for i, l in enumerate(lines):
        out = AUDIO / f"{l['id']}.mp3"
        if out.exists():
            ok += 1
            continue
        audio, used, st = t2a(key, l["text"], l["voice_id"])
        total_used += used
        if audio:
            out.write_bytes(audio)
            ok += 1
        else:
            fail += 1
            print(f"  ! {l['id']} ({l['voice_id']}): {st}", file=sys.stderr)
        if (i + 1) % 50 == 0:
            print(f"  ...lines {i+1}/{len(lines)}  used={total_used} chars")

    convs = corpus["conversations"][:args.convs] if args.convs else corpus["conversations"]
    for c in convs:
        scene = SCENES / f"{c['id']}.mp3"
        if scene.exists():
            continue
        turn_files = []
        for ti, t in enumerate(c["turns"]):
            tf = SCENES / f"{c['id']}_t{ti:02d}_{t['faction']}.mp3"
            if not tf.exists():
                audio, used, st = t2a(key, t["text"], t["voice_id"])
                total_used += used
                if audio:
                    tf.write_bytes(audio)
                else:
                    fail += 1
                    print(f"  ! {c['id']} turn {ti}: {st}", file=sys.stderr)
                    continue
            turn_files.append(tf)
        if turn_files:
            stitch(turn_files, scene)
            print(f"  scene {c['id']} '{c['title']}' ({len(turn_files)} turns)")

    print(f"\n[synth] lines ok={ok} fail={fail}, scenes={len(convs)}")
    print(f"[synth] TOTAL billed usage_characters = {total_used:,}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

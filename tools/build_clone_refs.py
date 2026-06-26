#!/usr/bin/env python3
"""Build per-speaker reference audio for MiniMax voice cloning.

For every distinct speaker with >=10 distinct lines (bar characters + flight
voice-actor clusters), concatenate several distinct clips into one clean
~15-40s mono MP3 suitable for voice-clone upload. Writes a manifest.

Outputs: assets/speech/clone_refs/<id>.mp3  +  assets/speech/clone_refs/manifest.json
"""
from __future__ import annotations

import json
import re
import subprocess
import wave
from collections import defaultdict
from pathlib import Path

OUT = Path("assets/speech/clone_refs")
MIN_DISTINCT = 10
TARGET_SECS = 28.0
MAX_CLIPS = 14


def dur(path: Path) -> float:
    try:
        w = wave.open(str(path), "rb")
        d = w.getnframes() / w.getframerate()
        w.close()
        return d
    except Exception:
        return 0.0


def safe(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_")


def gather(manifest_path, audio_dir, text_key, speaker_key, extra_keys):
    """Group clips by speaker -> list of (wav_path, text, dur)."""
    data = json.loads(Path(manifest_path).read_text())
    groups = defaultdict(list)
    meta = {}
    for wav, info in data.items():
        spk = info.get(speaker_key)
        if not spk:
            continue
        p = Path(audio_dir) / wav
        if not p.exists():
            continue
        groups[spk].append((p, info.get(text_key, "").strip()))
        meta.setdefault(spk, {k: info.get(k) for k in extra_keys})
    return groups, meta


def pick_clips(clips):
    """Dedup by text (keep longest per text), prefer longer clips, ~TARGET_SECS."""
    best_by_text = {}
    for p, t in clips:
        d = dur(p)
        key = t.lower()
        if key not in best_by_text or d > best_by_text[key][1]:
            best_by_text[key] = (p, d, t)
    cand = sorted(best_by_text.values(), key=lambda x: -x[1])
    chosen, total = [], 0.0
    for p, d, t in cand:
        if d < 0.4:
            continue
        chosen.append((p, t))
        total += d
        if total >= TARGET_SECS or len(chosen) >= MAX_CLIPS:
            break
    return chosen, total


def concat_mp3(clips, out_path):
    inputs = []
    for p, _ in clips:
        inputs += ["-i", str(p)]
    n = len(clips)
    fc = "".join(f"[{i}:a]" for i in range(n)) + f"concat=n={n}:v=0:a=1[a]"
    cmd = ["ffmpeg", "-y", "-loglevel", "error", *inputs,
           "-filter_complex", fc, "-map", "[a]",
           "-ar", "32000", "-ac", "1", "-b:a", "128k", str(out_path)]
    r = subprocess.run(cmd, capture_output=True)
    return r.returncode == 0, r.stderr.decode()[:200]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    sources = [
        ("bar", "assets/speech/bar_speakers.json", "assets/speech/bar",
         "asr_text", "speaker", ["speaker"]),
        ("flight", "assets/speech/flight_voices.json", "assets/speech/original",
         "text", "voice_id", ["gender", "faction"]),
    ]
    manifest = []
    for src, mpath, adir, tkey, skey, extra in sources:
        groups, meta = gather(mpath, adir, tkey, skey, extra)
        for spk, clips in groups.items():
            distinct = {t.lower() for _, t in clips if t}
            if len(distinct) < MIN_DISTINCT:
                continue
            chosen, total = pick_clips(clips)
            if total < 8.0 or len(chosen) < 3:
                continue
            sid = f"priv_{src}_{safe(spk)}"
            out_path = OUT / f"{sid}.mp3"
            ok, err = concat_mp3(chosen, out_path)
            if not ok:
                print(f"  ! {sid}: ffmpeg failed: {err}")
                continue
            entry = {
                "id": sid, "source": src, "speaker": spk,
                "n_clips": len(chosen), "duration_s": round(total, 1),
                "ref": str(out_path), "samples": [t for _, t in chosen[:3]],
                **meta.get(spk, {}),
            }
            manifest.append(entry)
            print(f"  {sid:34} {len(chosen):2d} clips {total:5.1f}s")
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(f"\n[refs] built {len(manifest)} reference clips -> {OUT}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Organize the bar-speech WAVs into per-speaker folders.

Reads assets/speech/bar_speakers.json (from classify_bar_speakers.py) and
builds assets/speech/bar_by_speaker/<speaker>/<wav>. Uses relative symlinks by
default (no duplication); pass --copy to make real copies instead.

Usage:
  python3 tools/organize_bar_by_speaker.py [--copy]
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
from pathlib import Path


def safe(name: str) -> str:
    """Filesystem-safe speaker folder name (rand_npc/randcu_3 -> rand_npc__randcu_3)."""
    return name.replace("/", "__").replace("\\", "__")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", type=Path, default=Path("assets/speech/bar"))
    ap.add_argument("--speakers", type=Path,
                    default=Path("assets/speech/bar_speakers.json"))
    ap.add_argument("--out", type=Path, default=Path("assets/speech/bar_by_speaker"))
    ap.add_argument("--copy", action="store_true", help="copy files instead of symlink")
    args = ap.parse_args()

    spk = json.loads(args.speakers.read_text())

    # Fresh start so re-runs don't accumulate stale links.
    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)

    counts: dict[str, int] = {}
    missing = 0
    for wav, info in sorted(spk.items()):
        src = args.src / wav
        if not src.exists():
            missing += 1
            continue
        speaker = safe(info.get("speaker", "unknown"))
        dest_dir = args.out / speaker
        dest_dir.mkdir(exist_ok=True)
        dest = dest_dir / wav
        if args.copy:
            shutil.copy2(src, dest)
        else:
            # relative symlink: out/<speaker>/<wav> -> ../../bar/<wav>
            rel = os.path.relpath(src.resolve(), dest_dir.resolve())
            dest.symlink_to(rel)
        counts[speaker] = counts.get(speaker, 0) + 1

    # Write a manifest of speaker -> line text for quick browsing.
    for speaker in counts:
        lines = []
        for wav, info in sorted(spk.items()):
            if safe(info.get("speaker", "unknown")) == speaker and (args.src / wav).exists():
                txt = (info.get("asr_text") or info.get("text") or "").strip()
                disp = info.get("disposition")
                tag = f"[{disp}] " if disp else ""
                lines.append(f"{wav}\t{tag}{txt}")
        (args.out / speaker / "_lines.txt").write_text("\n".join(lines) + "\n")

    mode = "copied" if args.copy else "symlinked"
    print(f"[organize] {mode} into {len(counts)} speaker folders under {args.out}"
          + (f"  ({missing} missing wavs)" if missing else ""))
    for s in sorted(counts, key=lambda k: -counts[k]):
        print(f"  {s:22} {counts[s]:4d}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

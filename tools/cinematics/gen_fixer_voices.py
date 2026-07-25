"""Synthesize per-paragraph voice clips for every bar fixer conversation.

Each fixer entry in assets/data/fixers.json gets a ``voice`` array parallel to
its ``dialogue`` array (plus an optional ``voice_offer``), rendered in that
character's CLONED ORIGINAL ACTOR voice -- the 1993 cast, cloned from the
extracted CONV/*.VPK bar speech (see tools/cinematics/voices.py's
DEFAULT_VOICE_MAP and assets/speech/clone_tests/results.json).

Everything is content-hash cached on (model, voice_id, speed, emotion, text),
so re-running after editing one line only re-synthesizes that line. Without a
MINIMAX_API_KEY the whole thing degrades to a no-op and simply leaves entries
unvoiced -- the engine treats a missing/short ``voice`` array as silence.

Usage::

    python -m tools.cinematics.gen_fixer_voices --dry-run
    python -m tools.cinematics.gen_fixer_voices                 # whole cast
    python -m tools.cinematics.gen_fixer_voices --fixer tayla   # id prefix
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from . import voices


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _fixers_path() -> Path:
    return _repo_root() / "assets" / "data" / "fixers.json"


# fixer-id prefix -> character-bible id (whose cloned voice we use)
SPEAKER = {
    "sandoval": "sandoval",
    "tayla": "tayla",
    "lynch": "lynch",
    "masterson": "masterson",
    "murphy": "murphy",
    "monkhouse": "monkhouse",
    "cross": "cross",
    "terrell": "terrell",
    "goodin": "goodin",
    "oxford": None,   # the library scene is narration, not a speaker
}

# The $NM/$CS substitution tokens are expanded at RUNTIME by
# fixers::expand_tokens, but TTS has to say something concrete. Speak the same
# values the engine renders so the audio matches the on-screen text.
_TOKENS = {"$NM": "Burrows", "$CS": "Grayson"}


def speakable(text: str) -> str:
    for token, value in _TOKENS.items():
        text = text.replace(token, value)
    # Collapse the ellipsis-heavy 1993 punctuation into something TTS paces
    # sensibly; "..." at a clause boundary otherwise reads as a long dead stop.
    text = text.replace("...", ", ")
    return re.sub(r"\s+", " ", text).strip()


def voice_rel_path(fixer_id: str, idx: int | str) -> str:
    return f"audio/fixers/{fixer_id}_{idx}.mp3"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fixer", help="only ids starting with this prefix")
    ap.add_argument("--dry-run", action="store_true",
                    help="print what would be synthesized, call nothing")
    ap.add_argument("--force", action="store_true",
                    help="bypass the content-hash cache")
    args = ap.parse_args(argv)

    if not args.dry_run and not voices.available():
        print("[fixer-voices] MINIMAX_API_KEY not set -- nothing to do. "
              "Entries stay unvoiced (silent), which the engine handles.")
        return 0

    path = _fixers_path()
    doc = json.loads(path.read_text())
    bible = None   # voices.voice_for falls back to DEFAULT_VOICE_MAP

    made = skipped = failed = 0
    for entry in doc["fixers"]:
        fid = entry["id"]
        if args.fixer and not fid.startswith(args.fixer):
            continue
        speaker = SPEAKER.get(fid.split("_")[0])
        if not speaker:
            skipped += 1
            continue

        # Two-hander support: speaker[i] == "pc" means Grayson answers, so the
        # line is rendered in the player-character's cloned voice instead of
        # the fixer's. A short/absent speaker array means all-fixer.
        speakers = entry.get("speaker", [])

        clips: list[str] = []
        for i, line in enumerate(entry.get("dialogue", [])):
            is_pc = i < len(speakers) and speakers[i] == "pc"
            who = "grayson" if is_pc else speaker
            rel = voice_rel_path(fid, f"{i:02d}")
            text = speakable(line)
            if args.dry_run:
                print(f"  {who:<10} {rel}  {text[:70]}")
                clips.append(rel)
                made += 1
                continue
            out = voices.gen_line_voice(
                who, text, out_rel=rel, bible=bible, force=args.force
            )
            if out:
                clips.append(out)
                made += 1
            else:
                clips.append("")     # keep the array index-aligned with dialogue
                failed += 1

        if clips and any(clips):
            entry["voice"] = clips

        # NOTE: `offer` is deliberately NOT voiced. It is UI-summary text --
        # "Haul 40 units of iron to Liverpool (Newcastle system) ... Deal?" --
        # full of parentheticals and mechanical numbers that read as a menu
        # prompt, not speech. The dialogue paragraphs carry the performance;
        # the offer stays a written confirmation. voice_offer remains in the
        # schema for hand-authored exceptions.

    if not args.dry_run:
        path.write_text(json.dumps(doc, indent=2) + "\n")

    verb = "would synthesize" if args.dry_run else "synthesized"
    print(f"[fixer-voices] {verb} {made} clip(s); "
          f"{failed} failed; {skipped} entr(ies) had no mapped speaker")
    return 0


if __name__ == "__main__":
    sys.exit(main())

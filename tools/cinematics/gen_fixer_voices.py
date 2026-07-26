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

# Extracted 1993 recordings beat synthesis every time. Key by exact authored
# text so inserting another paragraph cannot silently shift the binding.
ORIGINAL_TEXT_CLIPS = {
    "You don't wanna talk to me, 'cause I don't wanna talk to you.": "MIGGS_000",
    "And anyone that makes me do what I don't wanna do gets hurt, painwise, get me?": "MIGGS_001",
    "Mr. Lynch, sitting over there, HE'S the one you wanna talk to...": "MIGGS_002",
    "...so either state your bidness or take a hike, buddy.": "MIGGS_003",
    "Ah, $NM. You look just like your picture. Too bad.": "S1MAC1_000",
    "After a few drinks you'll change your mind.": "S1MAC1_001",
    "$CS, you look like hell.": "S1MDC1_000",
    "I can't understand why a woman as attractive as you...": "S1MDC1_003",
    "...has to pay for the company of men in bars.": "S1MDC1_004",
    "I see it as an investment...with a penalty for early withdrawal.": "S1MDC1_005",
    "Let me guess...I'm running a shipment of catnip to Kilrah.": "S1MDC1_010",
    "Nope, I save the really lucrative jobs for myself.": "S1MDC1_011",
    "You must be joking. I'm no assassin.": "S2MAC1_009",
    "You want I should take $NM outside the airlock and teach him how to suck vacuum?": "S2MAC1_010",
    "Gentlemen, please, let us remain professional.": "S2MAC1_011",
    "Another chump job, Lynch?": "S2MBC1_010",
    "Ever seen your lungs? Keep crackin' wise, I'll show them to ya...up-close, like.": "S2MBC1_011",
    "Jeez, where do you get your dialogue, Thugs-R-Us?": "S2MBC1_012",
    "Enough, Miggs.": "S2MBC1_013",
    "Mr. Lynch did you a favor, pal.": "S2MCC1_006",
    "Better get grateful quick-like...while you can still walk.": "S2MCC1_007",
    "I could always get crutches, Miggs...but there's no cure for ugliness.": "S2MCC1_008",
    "I urge you to observe caution with Miggs. My control over him extends only so far.": "S2MCC1_009",
    "I hate to run off without giving Miggs a kiss. Where is he?": "S2MDC1_015",
    "Miggs is currently eliminating a...labor difficulty. I'll convey your regards.": "S2MDC1_016",
    "Yeah, I can tell how strapped Oxford is for capital...": "S3MBC1_012",
    "...ever since you started letting smugglers like me make endowments.": "S3MBC1_013",
    "Hell, you better name an entire wing after me.": "S3MDC1_010",
    "Stop stalling and save that freighter, damn it!": "S3MDC1_011",
    "Touchy, touchy...": "S3MDC1_012",
}
ORIGINAL_TEXT_CLIPS = {
    text: f"../speech/bar/{stem}.wav" for text, stem in ORIGINAL_TEXT_CLIPS.items()
}


def speakable(text: str) -> str:
    for token, value in _TOKENS.items():
        text = text.replace(token, value)
    # Collapse the ellipsis-heavy 1993 punctuation into something TTS paces
    # sensibly; "..." at a clause boundary otherwise reads as a long dead stop.
    text = text.replace("...", ", ")
    return re.sub(r"\s+", " ", text).strip()


def voice_rel_path(fixer_id: str, idx: int | str) -> str:
    return f"audio/fixers/{fixer_id}_{idx}.mp3"


def is_beat(text: str) -> bool:
    """A BEAT is a silent reaction line -- '...' -- not speech.

    Written into the script where a character says nothing but the moment
    needs to land (Grayson being handed an alien artifact, Cross being shown
    her best pilot's gun-camera footage). TTS would read it aloud as "dot dot
    dot", so beats are never synthesized; the engine holds them for a short
    read-time instead.
    """
    return text.strip().strip(".… ") == ""


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
            speaker_key = speakers[i] if i < len(speakers) else ""
            who = "grayson" if speaker_key == "pc" else speaker_key or speaker
            rel = ORIGINAL_TEXT_CLIPS.get(line, voice_rel_path(fid, f"{i:02d}"))
            text = speakable(line)
            if line in ORIGINAL_TEXT_CLIPS:
                clips.append(rel)
                skipped += 1
                continue
            if is_beat(line):
                clips.append("")     # silent beat: index-aligned, no audio
                skipped += 1
                continue
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

        # ---- post-decision exchanges (accept_/refuse_) ---------------------
        # Same parallel-array contract as the main conversation, just keyed
        # under a different prefix so the phases can't collide on disk.
        for phase in ("accept", "refuse"):
            lines = entry.get(f"{phase}_dialogue", [])
            if not lines:
                continue
            ph_speakers = entry.get(f"{phase}_speaker", [])
            ph_clips: list[str] = []
            for i, line in enumerate(lines):
                speaker_key = ph_speakers[i] if i < len(ph_speakers) else ""
                who = "grayson" if speaker_key == "pc" else speaker_key or speaker
                rel = voice_rel_path(fid, f"{phase}_{i:02d}")
                if is_beat(line):
                    ph_clips.append("")
                    skipped += 1
                    continue
                if args.dry_run:
                    print(f"  {who:<10} {rel}  {speakable(line)[:60]}")
                    ph_clips.append(rel)
                    made += 1
                    continue
                out = voices.gen_line_voice(
                    who, speakable(line), out_rel=rel, bible=bible,
                    force=args.force
                )
                if out:
                    ph_clips.append(out)
                    made += 1
                else:
                    ph_clips.append("")
                    failed += 1
            if any(ph_clips):
                entry[f"{phase}_voice"] = ph_clips

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

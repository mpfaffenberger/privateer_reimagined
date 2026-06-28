#!/usr/bin/env python3
"""Build the NPC comm-response bank from the generated comms corpus.

When the player hails a ship/base, the recipient replies with one of its own
faction lines (which we already authored + synthesized). This reuses
assets/speech/generated/comms.json (text + voice + category, with audio at
assets/speech/generated/audio/<id>.mp3) to emit:

  assets/data/comm_responses.json
    { "<faction>": { "<category>": [ {"text": "...", "clip": "assets/.../comm_XXXX.mp3"} ] } }

comms_menu loads this and, on a hail, plays a random response for the
recipient's (faction, category) — greeting when friendly, hostile when not.
Written as raw UTF-8 (the engine json.h reader has no \\uXXXX decoding).
"""
from __future__ import annotations
import collections
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from place_filter import is_location_specific  # noqa: E402

COMMS = Path("assets/speech/generated/comms.json")
AUDIO_DIR = "assets/speech/generated/audio"
OUT = Path("assets/data/comm_responses.json")

# Only these categories are useful as hail responses (skip long monologues).
KEEP = {"greeting", "hostile", "low_hp", "kill", "demand"}


def main():
    corpus = json.loads(COMMS.read_text())
    by_faction = collections.defaultdict(lambda: collections.defaultdict(list))
    by_voice = collections.defaultdict(lambda: collections.defaultdict(list))
    for l in corpus["lines"]:
        cat = l["category"]
        if cat not in KEEP:
            continue
        if is_location_specific(l["text"]):
            continue  # skip lines naming a specific place (wrong system)
        clip = f"{AUDIO_DIR}/{l['id']}.mp3"
        if not os.path.exists(clip):
            continue
        entry = {"text": l["text"].strip(), "clip": clip}
        by_faction[l["faction"]][cat].append(entry)
        if l.get("voice_id"):
            by_voice[l["voice_id"]][cat].append(entry)
    # The engine keys replies by the speaker's voice_id (per-ship voice), with
    # by_faction as a fallback when a voice has no line in a category.
    out = {
        "by_faction": {f: dict(cats) for f, cats in by_faction.items()},
        "by_voice": {v: dict(cats) for v, cats in by_voice.items()},
    }
    OUT.write_text(json.dumps(out, indent=1, ensure_ascii=False))
    nf = sum(len(v) for cats in out["by_faction"].values() for v in cats.values())
    print(f"[comm-responses] {len(out['by_faction'])} factions, "
          f"{len(out['by_voice'])} voices, {nf} lines -> {OUT}")


if __name__ == "__main__":
    main()

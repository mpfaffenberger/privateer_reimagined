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
from pathlib import Path

COMMS = Path("assets/speech/generated/comms.json")
AUDIO_DIR = "assets/speech/generated/audio"
OUT = Path("assets/data/comm_responses.json")

# Only these categories are useful as hail responses (skip long monologues).
KEEP = {"greeting", "hostile", "low_hp", "kill", "demand"}


def main():
    corpus = json.loads(COMMS.read_text())
    bank = collections.defaultdict(lambda: collections.defaultdict(list))
    for l in corpus["lines"]:
        cat = l["category"]
        if cat not in KEEP:
            continue
        clip = f"{AUDIO_DIR}/{l['id']}.mp3"
        if not os.path.exists(clip):
            continue
        bank[l["faction"]][cat].append({"text": l["text"].strip(), "clip": clip})
    # plain dict for json
    out = {f: {c: v for c, v in cats.items()} for f, cats in bank.items()}
    OUT.write_text(json.dumps(out, indent=1, ensure_ascii=False))
    total = sum(len(v) for cats in out.values() for v in cats.values())
    print(f"[comm-responses] {len(out)} factions, {total} response lines -> {OUT}")
    for f, cats in out.items():
        print(f"  {f:14} " + ", ".join(f"{c}:{len(v)}" for c, v in cats.items()))


if __name__ == "__main__":
    main()

"""Restore the player-character's half of the bar conversations.

Vanilla Privateer bar scenes are TWO-HANDERS: the fixer talks, Grayson answers,
back and forth (see any S*.PFC conversation in the extracted speech --
S0MAC1 runs monte -> pc -> monte -> pc ...). Our authored fixers.json flattened
that into fixer-only monologue, which reads as a briefing rather than a scene
and leaves the 379 extracted `pc` clips unused.

This inserts short Grayson responses at the natural beats and marks them with a
parallel ``speaker`` array ("pc" vs "" for the fixer). Lines are written to fit
THIS script rather than lifted verbatim from vanilla, since our dialogue was
already rewritten -- but they are modelled on the vanilla register: dry,
transactional, a working pilot who has heard a lot of pitches.

Usage::

    python -m tools.cinematics.add_pc_lines --dry-run
    python -m tools.cinematics.add_pc_lines
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


# fixer id -> {index in the ORIGINAL dialogue list: Grayson's reply AFTER it}
# Kept deliberately sparse: a reply at every beat turns a scene into ping-pong.
# Aim for one or two per conversation, at the moment a real person would react.
PC_LINES: dict[str, dict[int, str]] = {
    "sandoval_offer": {
        0: "Depends on the work. And the man offering it.",
        2: "An advance? Or do I fly this on trust?",
    },
    "tayla_artifact_handoff": {
        0: "Dead. And the fifteen thousand he owed me?",
        3: "You're telling me I'm carrying a murder motive.",
    },
    "tayla_m02_offer": {
        1: "Falsified manifest. That's a jump up from hauling iron.",
    },
    "tayla_m03_offer": {
        1: "Militia scanning everything, and I'm carrying Brilliance.",
    },
    "tayla_m04_offer": {
        1: "Bribed patrols. You'll forgive me if I keep my guns hot.",
    },
    "tayla_m04_debrief": {
        0: "Clerical error. Right.",
    },
    "tayla_m05_offer": {
        1: "Riordian. Should I be watching my back?",
    },
    "tayla_m05_debrief": {
        1: "Every owner you can name is dead, Tayla.",
    },
    "lynch_m06_offer": {
        1: "A hologram. And you'd know that how?",
        2: "A message. That's all it is?",
    },
    "lynch_m07_offer": {
        0: "People keep dying around it. You say that like it's weather.",
    },
    "lynch_m08_offer": {
        0: "Alien. Not Kilrathi.",
        1: "Your cousin. What did he do?",
    },
    "lynch_m09_offer": {
        1: "A taxi run. Nothing about this has been a taxi run.",
    },
    "masterson_m10_offer": {
        1: "Four favors for a library card. That's quite a fee.",
    },
    "masterson_m12_offer": {
        1: "So the freighter is bait, and I'm the target.",
    },
    "murphy_m14_offer": {
        1: "People are starving so a corporation can move a decimal point.",
    },
    "murphy_m16_offer": {
        1: "Volunteers. Against a blockade.",
    },
    "monkhouse_m17_offer": {
        1: "Kidnapped. And they're still looking for you.",
        2: "Someone with fur. You're saying Kilrathi.",
    },
    "cross_m19_offer": {
        0: "Missing three weeks. You want a survey or a search party?",
    },
    "cross_m20_offer": {
        1: "A corvette guarding an empty system.",
    },
    "terrell_offer": {
        0: "It's not Kilrathi. Whatever it is, it's older than that.",
        1: "So I'm bait. Say it plainly, Admiral.",
    },
    "goodin_offer": {
        1: "The Admiral wants a word. Do I get a choice?",
    },
    "oxford_library_scene": {},   # narration -- no speaker
}

PC_PORTRAIT = "portraits/grayson/_ref.png"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    path = _repo_root() / "assets" / "data" / "fixers.json"
    doc = json.loads(path.read_text())

    grayson_ref = _repo_root() / "assets" / "cinematics" / PC_PORTRAIT
    have_pc_art = grayson_ref.is_file()

    touched = added = 0
    for entry in doc["fixers"]:
        inserts = PC_LINES.get(entry["id"])
        if not inserts:
            continue
        dialogue = entry.get("dialogue", [])
        # Rebuild both arrays together so indices can never drift apart.
        new_dialogue: list[str] = []
        new_speaker: list[str] = []
        for i, line in enumerate(dialogue):
            new_dialogue.append(line)
            new_speaker.append("")
            if i in inserts:
                new_dialogue.append(inserts[i])
                new_speaker.append("pc")
                added += 1
        entry["dialogue"] = new_dialogue
        entry["speaker"] = new_speaker
        # The voice array is now stale (indices shifted); drop it so
        # gen_fixer_voices re-renders cleanly against the new list.
        entry.pop("voice", None)
        if have_pc_art:
            entry["portrait_pc"] = PC_PORTRAIT
        touched += 1
        if args.dry_run:
            print(f"=== {entry['id']} ===")
            for line, who in zip(new_dialogue, new_speaker):
                tag = "GRAYSON" if who == "pc" else "fixer  "
                print(f"  [{tag}] {line[:78]}")

    if not args.dry_run:
        path.write_text(json.dumps(doc, indent=2) + "\n")

    verb = "would add" if args.dry_run else "added"
    print(f"[pc-lines] {verb} {added} Grayson line(s) across {touched} conversation(s)")
    if not have_pc_art:
        print("[pc-lines] NOTE: portraits/grayson/_ref.png missing -- no portrait_pc set")
    return 0


if __name__ == "__main__":
    sys.exit(main())

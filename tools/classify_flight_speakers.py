#!/usr/bin/env python3
"""Classify the in-flight comm speakers (SPEECH.PAK -> assets/speech/original).

Unlike the bar speech, flight clips carry no per-line metadata -- but SPEECH.PAK
is organized in contiguous faction blocks, and the dialogue content betrays the
faction. We classify each clip by:
  1. human ground-truth labels (docs/speech_labels.json) where present  [anchor]
  2. distinctive content keywords on the Whisper transcript            [content]
  3. block forward/back-fill: generic combat barks inherit the faction
     of the nearest confident neighbour (the PAK's block layout)        [block]

Output: assets/speech/flight_speakers.json  (wav -> {speaker, method, ...}).
"""
from __future__ import annotations

import argparse
import collections
import json
import re
from pathlib import Path

# UNAMBIGUOUS, high-precision phrases only. Generic combat barks, friendly
# greetings, and shared patrol/contraband lines are deliberately excluded --
# those are resolved by nearest-anchor block fill instead.
FACTION_KEYWORDS = {
    "retro": ["repent", "heresy", "heathen", "church of man", "sinner",
              "will of god", "purifying fire", "condemns", "die devil",
              "damnable", "technological burden", "knowledge belongs",
              "hoard data", "weapons you adore", "righteous", "the lord",
              "o lord", "destruction is the will", "unto the most high"],
    "kilrathi": ["apeling", "monkhouse", "my claws", "sheathed", "my belly",
                 "bones in my hall", "in our power"],
    "confed": ["confederation", "admiral", "riceman", "smuggling scum",
               "criminal scum", "accessed your file", "quite a record",
               "life sentence", "transporting", "your file, pirate"],
    "militia": ["militia", "malisha", "malaysia forces", "flight path",
                "brilliance runner", "our sector", "keep your nose clean",
                "sector bottle", "sector-bottle"],
    "merchant": ["merchant", "honest dealer", "guild will hear",
                 "dissatisfied customer", "scaring away my business",
                 "friendly ship", "dreadful retro", "gracious me"],
    "bounty_hunter": ["resume", "professional", "death is my living",
                      "happy hunting", "kill for a fee", "my list",
                      "smoking out", "distasteful", "advantage is mine",
                      "nothing personal", "death is my ga", "in the black"],
    "pirate": ["suck void", "dump your cargo", "drop your cargo", "loose end",
               "lynch", "artifact", "errand boy"],
    "flight_controller": ["automatic landing zone"],
    "salman_kreitz": ["salman", "kreitz", "extraordinaire", "operatives and i",
                      "bolster our resumes"],
    "william_reordion": ["reordion", "steal my business", "steal a man's business",
                         "two private tiers", "take over my route"],
}

# Disposition rules, checked in PRECEDENCE order (first match wins). These are
# the comm "triggers": friendly greeting, hostile taunt, low-hp distress,
# kill/victory, and the patrol contraband-check.
DISPOSITION_RULES = [
    ("low_hp", ["ripping me apart", "about to explode", "crate's about",
                "creates a bow", "creates about", "mayday", "massive damage",
                "we're finished", "we're in trouble", "breaking up", "retreat",
                "can't end like this", "it can't end", "this is not good",
                "not good!", "stay away", "back off", "no, no", "this crate",
                "in need of assistance", "sustained massive"]),
    ("kill", ["another day, another kill", "one more for the resume",
             "kill achieved", "target eliminated", "bogey terminated",
             "enemy target destroyed", "you're dead", "you were dead",
             "you are dead", "nailed, pal", "you nailed", "clean kill",
             "death is my", "dead, chump", "dead, jump", "massacre",
             "enemy dispatched", "another for the resume", "your career",
             "in the black", "a clean kill"]),
    ("contraband", ["contraband", "prepare to be searched",
                    "maintain speed and course", "may proceed",
                    "prepare to be search"]),
    ("friendly", ["how's it going", "how's the going", "good to see a friendly",
                  "passing merchant", "not looking for trouble",
                  "clear to pass", "clear for clicks", "looking sharp",
                  "catch you on the next run", "do business", "happy hunting",
                  "profile is clear", "you may proceed", "you're clear",
                  "you are clear", "keep your distance", "keep your vidcom",
                  "thank you", "friendly ship", "maybe we can do business",
                  "stick to", "keep your nose clean", "continue on course"]),
    ("hostile", ["last run", "suck void", "loose end", "advantage is mine",
                 "you're finished", "you are finished", "your luck",
                 "not leaving", "busted", "dump your cargo", "drop your cargo",
                 "repent", "heresy", "die devil", "die,", "prepare to die",
                 "sentenced to death", "execution", "will of god", "wrath",
                 "you're a dead man", "watch your back", "smuggling scum",
                 "criminal scum", "it ends here", "end your career",
                 "now you die"]),
]


def disposition_from_label(label: str):
    L = label.lower()
    if "low hp" in L or "low_hp" in L:
        return "low_hp"
    if "kill" in L:
        return "kill"
    if "winning" in L:
        return "hostile"
    if "contraband" in L:
        return "contraband"
    if "friendly" in L:
        return "friendly"
    if "hostile" in L:
        return "hostile"
    return None


def disposition_from_text(text: str):
    t = text.lower()
    for dispo, kws in DISPOSITION_RULES:
        if any(k in t for k in kws):
            return dispo
    return None


# Map a human label string to a canonical faction.
LABEL_FACTION = [
    ("bounty", "bounty_hunter"), ("retro", "retro"), ("confed", "confed"),
    ("confied", "confed"), ("kilrathi", "kilrathi"), ("merch", "merchant"),
    ("steltek", "steltek"), ("flight controller", "flight_controller"),
]


def faction_from_label(label: str):
    L = label.lower()
    for kw, fac in LABEL_FACTION:
        if kw in L:
            return fac
    return None


def faction_from_text(text: str):
    t = text.lower()
    for fac, kws in FACTION_KEYWORDS.items():
        if any(k in t for k in kws):
            return fac
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--transcripts", type=Path,
                    default=Path("assets/speech/flight_transcripts.json"))
    ap.add_argument("--labels", type=Path, default=Path("docs/speech_labels.json"))
    ap.add_argument("--out", type=Path, default=Path("assets/speech/flight_speakers.json"))
    args = ap.parse_args()

    tr = json.loads(args.transcripts.read_text())
    raw = json.loads(args.labels.read_text())
    labels = {k: v["label"].strip() for k, v in raw.items()
              if not k.startswith("_")}

    # Ordered list of clips by index.
    idxs = sorted(int(w.split("_")[1].split(".")[0]) for w in tr)
    recs = []  # (idx, key, text, faction, method, label)
    for i in idxs:
        key = f"speech_{i:04d}"
        text = tr.get(key + ".wav", {}).get("text", "").strip()
        lab = labels.get(key, "")
        lab = "" if lab in ("", ".") else lab
        fac, method = None, None
        if lab:
            fac = faction_from_label(lab)
            if fac:
                method = "label"
        if fac is None:
            fac = faction_from_text(text)
            if fac:
                method = "content"
        recs.append([i, key, text, fac, method, lab])

    # Nearest-anchor fill: each unanchored clip inherits the faction of the
    # closest anchored clip by index distance (ties -> the earlier/left one).
    anchors = [(j, r[3]) for j, r in enumerate(recs) if r[3] is not None]
    if anchors:
        for j, r in enumerate(recs):
            if r[3] is None:
                best = min(anchors, key=lambda a: (abs(a[0] - j), a[0]))
                r[3], r[4] = best[1], "block"

    result = {}
    for i, key, text, fac, method, lab in recs:
        dispo = disposition_from_label(lab) if lab else None
        dmethod = "label" if dispo else None
        if dispo is None:
            dispo = disposition_from_text(text)
            if dispo:
                dmethod = "content"
        result[key + ".wav"] = {
            "speaker": fac or "unknown",
            "disposition": dispo or "other",
            "method": method or "none",
            "disposition_method": dmethod or "none",
            "label": lab,
            "text": text,
        }
    args.out.write_text(json.dumps(result, indent=1, sort_keys=True))

    spk = collections.Counter(v["speaker"] for v in result.values())
    meth = collections.Counter(v["method"] for v in result.values())
    disp = collections.Counter(v["disposition"] for v in result.values())
    print(f"[flight] {len(result)} clips, {len(spk)} speakers")
    print(f"[flight] speaker methods: {dict(meth)}")
    print(f"[flight] dispositions: {dict(disp)}")
    print(f"[flight] manifest -> {args.out}\n")
    # speaker x disposition matrix
    dorder = ["friendly", "hostile", "low_hp", "kill", "contraband", "other"]
    print(f"  {'speaker':18} " + " ".join(f"{d[:5]:>6}" for d in dorder))
    for s, _ in spk.most_common():
        row = collections.Counter(v["disposition"] for v in result.values()
                                  if v["speaker"] == s)
        print(f"  {s:18} " + " ".join(f"{row.get(d,0):6d}" for d in dorder))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

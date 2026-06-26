#!/usr/bin/env python3
"""Assign a speaker label to every bar-speech WAV using the PFC metadata.

Privateer's CONV/*.PFC files tag each dialogue line with its NPC class (the
named character: taryn, mastersn, terrel, lynn, monkhous, sandra, roman, pc,
rand_npc, ...), a voice id (randcu_1/2/3, shpdlr_1) and a variant byte. That is
ground-truth speaker classification -- better than acoustic diarization.

We don't trust positional index alignment (audio vs text counts drift). Instead
we fuzzy-match each clip's Whisper transcript to the best PFC line *within the
same conversation group* and inherit that line's speaker.

Output: a JSON manifest  wav -> {speaker, npc, voice, variant, pfc_text,
asr_text, score}.  Unmatched clips (no PFC / low score) get speaker "unknown".
"""
from __future__ import annotations

import argparse
import glob
import json
import re
from difflib import SequenceMatcher
from pathlib import Path


def parse_pfc(data: bytes):
    """Yield (npc_class, voice_id, variant, text) per dialogue record.

    Robust to the per-record marker/terminator bytes varying across files
    (\x9f/\xaa/\xac... and \xe0/\xe5...). Anchors on the invariant layout:
        <voice_id>\x00 <marker>\x00 <variant>\x00 <dialogue text>\x00 <term>\x00
    Detect each dialogue text by that 5-byte preamble signature, then read the
    NPC class as the token following the previous record's terminator.
    """
    n = len(data)

    def prun(s):
        e = s
        while e < n and 32 <= data[e] < 127:
            e += 1
        return e

    recs = []  # [text_start, text_end, voice, variant, npc]
    i = 5
    while i < n:
        if (data[i - 1] == 0 and data[i - 3] == 0 and data[i - 5] == 0
                and data[i - 2] != 0 and data[i - 4] != 0 and 32 <= data[i] < 127):
            e = prun(i)
            if e - i >= 2 and e < n and data[e] == 0:
                variant = data[i - 2]
                vend = i - 5
                vstart = data.rfind(b"\x00", 0, vend)
                voice = data[vstart + 1:vend].decode("latin1", "replace")
                recs.append([i, e, voice, variant, None, vstart])
                i = e + 1
                continue
        i += 1

    def first_letter_token(a, b):
        j = a
        while j < b:
            c = data[j]
            if 65 <= c <= 90 or 97 <= c <= 122:
                s = j
                while j < b and 32 <= data[j] < 127:
                    j += 1
                return data[s:j].decode("latin1", "replace")
            j += 1
        return ""

    # NPC class = the first letter-token in the gap between the previous
    # record's text and this record's voice id (order: npc, state, voice).
    for k, r in enumerate(recs):
        gap_start = 0 if k == 0 else recs[k - 1][1] + 1
        r[4] = first_letter_token(gap_start, r[5])

    return [(r[4], r[2], r[3], data[r[0]:r[1]].decode("latin1", "replace"))
            for r in recs]


def norm(s: str) -> str:
    return " ".join(re.sub(r"[^a-z0-9 ]", " ", s.lower()).split())


def speaker_label(npc: str, voice: str) -> str:
    """Named characters use their name; generic NPCs split by voice bank."""
    if npc and npc != "rand_npc":
        return npc
    return f"rand_npc/{voice}" if voice else "rand_npc"


def group_of(wav_name: str) -> str:
    m = re.match(r"^(.*?)_\d{3}(?:_|\.wav$)", wav_name)
    return m.group(1) if m else wav_name[:-4]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--conv", default="gog_extracted/extracted/priv/DATA/CONV")
    ap.add_argument("--transcripts", type=Path,
                    default=Path("assets/speech/bar_transcripts.json"))
    ap.add_argument("--out", type=Path,
                    default=Path("assets/speech/bar_speakers.json"))
    ap.add_argument("--threshold", type=float, default=0.45)
    args = ap.parse_args()

    asr = json.loads(args.transcripts.read_text())
    # group wav -> list of (wav_name, asr_text)
    by_group: dict[str, list] = {}
    for wav, info in asr.items():
        by_group.setdefault(group_of(wav), []).append((wav, info.get("text", "")))

    # group -> pfc records
    pfc_by_group: dict[str, list] = {}
    for pfc in glob.glob(f"{args.conv}/*.PFC"):
        stem = Path(pfc).stem
        pfc_by_group[stem] = parse_pfc(Path(pfc).read_bytes())

    def emit(wav, rec, score, method, atext):
        npc, voice, variant, text = rec
        return {
            "speaker": speaker_label(npc, voice), "npc": npc, "voice": voice,
            "variant": variant, "pfc_text": text, "asr_text": atext,
            "score": round(score, 3), "method": method,
        }

    result: dict[str, dict] = {}
    for group, wavs in by_group.items():
        recs = pfc_by_group.get(group, [])
        swavs = sorted(wavs)
        used = set()
        assign: dict[str, int] = {}      # wav -> rec index (text-matched)
        scores: dict[str, float] = {}
        for wav, atext in swavs:
            best_i, best_score = -1, 0.0
            na = norm(atext)
            for i, (_n, _v, _va, text) in enumerate(recs):
                if i in used:
                    continue
                sc = SequenceMatcher(None, na, norm(text)).ratio()
                if sc > best_score:
                    best_score, best_i = sc, i
            scores[wav] = best_score
            if best_i >= 0 and best_score >= args.threshold:
                assign[wav] = best_i
                used.add(best_i)
        # Order-preserving positional fallback for clips that didn't text-match:
        # zip the leftover clips with the leftover PFC records, both in order.
        leftover_recs = [i for i in range(len(recs)) if i not in used]
        leftover_wavs = [w for w, _ in swavs if w not in assign]
        pos_assign = dict(zip(leftover_wavs, leftover_recs))
        for wav, atext in swavs:
            if wav in assign:
                result[wav] = emit(wav, recs[assign[wav]], scores[wav], "text", atext)
            elif wav in pos_assign:
                result[wav] = emit(wav, recs[pos_assign[wav]], scores[wav],
                                   "position", atext)
            else:
                result[wav] = {
                    "speaker": "unknown", "npc": "", "voice": "", "variant": None,
                    "pfc_text": "", "asr_text": atext, "score": round(scores[wav], 3),
                    "method": "none",
                }

    args.out.write_text(json.dumps(result, indent=1, sort_keys=True))

    # Summary
    import collections
    spk = collections.Counter(v["speaker"] for v in result.values())
    known = sum(1 for v in result.values() if v["speaker"] != "unknown")
    print(f"[speakers] {len(result)} clips, {known} labeled "
          f"({100*known/len(result):.0f}%), {len(spk)} distinct speakers")
    print(f"[speakers] manifest -> {args.out}\n")
    for s, c in spk.most_common():
        print(f"  {s:18} {c:4d}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

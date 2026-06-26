#!/usr/bin/env python3
"""Cluster speech clips into distinct voice actors, then tag gender + faction.

Input: ECAPA embeddings (embed_speech.py) + the faction manifest
(classify_flight_speakers.py) + the human labels (for gender anchors).

- distinct voice actor  = agglomerative cluster of ECAPA embeddings (cosine).
- gender                = embedding similarity to male/female anchor prototypes,
                          tie-broken by cluster median pitch.
- faction               = majority faction of the cluster's clips.

Output: assets/speech/flight_voices.json  (per-clip: voice_id, gender, faction)
and prints a per-voice-actor summary table.
"""
from __future__ import annotations

import argparse
import collections
import json
from pathlib import Path

import numpy as np
from sklearn.cluster import AgglomerativeClustering


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--emb", type=Path, default=Path("assets/speech/flight_embeddings.npz"))
    ap.add_argument("--speakers", type=Path, default=Path("assets/speech/flight_speakers.json"))
    ap.add_argument("--labels", type=Path, default=Path("docs/speech_labels.json"))
    ap.add_argument("--out", type=Path, default=Path("assets/speech/flight_voices.json"))
    ap.add_argument("--threshold", type=float, default=0.6)
    args = ap.parse_args()

    d = np.load(args.emb, allow_pickle=True)
    names = list(d["names"])
    emb = d["emb"].astype(np.float64)
    f0 = d["f0"]
    emb /= np.linalg.norm(emb, axis=1, keepdims=True) + 1e-9
    idx = {n: i for i, n in enumerate(names)}

    spk = json.loads(args.speakers.read_text())
    raw = json.loads(args.labels.read_text())
    labels = {k + ".wav": v["label"].strip() for k, v in raw.items()
              if not k.startswith("_")}

    # Gender anchors from human labels.
    male_i, female_i = [], []
    for n in names:
        L = labels.get(n, "").lower()
        if not L or L == ".":
            continue
        if "female" in L:
            female_i.append(idx[n])
        elif L.startswith("m ") or "male" in L.split():
            male_i.append(idx[n])
    male_proto = emb[male_i].mean(0)
    male_proto /= np.linalg.norm(male_proto)
    female_proto = emb[female_i].mean(0)
    female_proto /= np.linalg.norm(female_proto)

    # Cluster distinct voice actors.
    cl = AgglomerativeClustering(n_clusters=None, metric="cosine", linkage="average",
                                 distance_threshold=args.threshold).fit_predict(emb)
    nclusters = len(set(cl))

    # Per-cluster gender + faction.
    voice_of = {}      # cluster -> info
    for c in sorted(set(cl)):
        members = [i for i in range(len(names)) if cl[i] == c]
        cmean = emb[members].mean(0)
        cmean /= np.linalg.norm(cmean)
        sm = float(cmean @ male_proto)
        sf = float(cmean @ female_proto)
        med_f0 = float(np.median([f0[i] for i in members if f0[i] > 0]) or 0)
        # Embedding decides gender; pitch only breaks near-ties.
        margin = sf - sm
        if margin > 0.04:
            gender = "F"
        elif margin < -0.04:
            gender = "M"
        else:
            gender = "F" if med_f0 >= 175 else "M"
        # Confidence: embedding margin + pitch agreement.
        pitch_says_f = med_f0 >= 185
        agree = (gender == "F") == pitch_says_f
        gconf = "high" if abs(margin) > 0.08 and agree else (
            "low" if abs(margin) < 0.04 or not agree else "med")
        facs = collections.Counter(spk.get(names[i], {}).get("speaker", "unknown")
                                   for i in members)
        voice_of[c] = {"gender": gender, "gconf": gconf,
                       "faction": facs.most_common(1)[0][0],
                       "factions": dict(facs), "n": len(members),
                       "med_f0": round(med_f0), "sm": round(sm, 3),
                       "sf": round(sf, 3)}

    # Stable voice ids: V00.. ordered by size.
    order = sorted(voice_of, key=lambda c: -voice_of[c]["n"])
    vid = {c: f"V{rank:02d}" for rank, c in enumerate(order)}

    result = {}
    for i, n in enumerate(names):
        c = cl[i]
        fac = spk.get(n, {}).get("speaker", "unknown")
        result[n] = {
            "voice_id": vid[c],
            "gender": voice_of[c]["gender"],
            "gender_conf": voice_of[c]["gconf"],
            "faction": fac,
            "disposition": spk.get(n, {}).get("disposition", "other"),
            "speaker": f"{vid[c]}_{voice_of[c]['gender']}_{voice_of[c]['faction']}",
            "text": spk.get(n, {}).get("text", ""),
        }
    args.out.write_text(json.dumps(result, indent=1, sort_keys=True))

    print(f"[voices] {len(names)} clips -> {nclusters} distinct voice actors "
          f"(threshold {args.threshold})")
    g = collections.Counter(v["gender"] for v in voice_of.values())
    print(f"[voices] voice-actor genders: {dict(g)}")
    print(f"[voices] manifest -> {args.out}\n")
    print(f"  {'id':4} {'gen':3} {'n':>4} {'F0':>4}  faction (distribution)")
    for c in order:
        v = voice_of[c]
        facs = ", ".join(f"{k}:{n}" for k, n in
                         sorted(v["factions"].items(), key=lambda x: -x[1]))
        print(f"  {vid[c]:4} {v['gender']:3} {v['n']:4d} {v['med_f0']:4d}  {facs}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

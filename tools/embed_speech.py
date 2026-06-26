#!/usr/bin/env python3
"""Compute ECAPA-TDNN speaker embeddings for a directory of WAVs.

Saves an .npz with `names` (filenames) and `emb` (N x 192 float32 embeddings),
plus per-clip median pitch `f0`. Used to cluster distinct voice actors and to
classify gender/faction (see cluster_voices.py).

Usage:
  python3 tools/embed_speech.py --dir assets/speech/original --out assets/speech/flight_embeddings.npz
"""
from __future__ import annotations

import argparse
import wave
from pathlib import Path

import numpy as np
import torch
import torchaudio


def median_f0(path, fmin=70, fmax=320):
    w = wave.open(str(path), "rb")
    sr = w.getframerate()
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
    w.close()
    if len(x) < sr // 10:
        return 0.0
    x /= (np.abs(x).max() or 1)
    fl = int(0.04 * sr)
    hop = fl // 2
    lo, hi = int(sr / fmax), int(sr / fmin)
    f0s = []
    for i in range(0, len(x) - fl, hop):
        fr = x[i:i + fl]
        if np.sqrt((fr ** 2).mean()) < 0.06:
            continue
        fr = fr - fr.mean()
        ac = np.correlate(fr, fr, "full")[len(fr) - 1:]
        if ac[0] <= 0:
            continue
        seg = ac[lo:hi]
        if len(seg) == 0:
            continue
        lag = lo + int(np.argmax(seg))
        if ac[lag] / ac[0] < 0.3:
            continue
        f0s.append(sr / lag)
    return float(np.median(f0s)) if f0s else 0.0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    from speechbrain.inference.speaker import EncoderClassifier
    print("[embed] loading ECAPA-TDNN (speechbrain/spkrec-ecapa-voxceleb)...")
    enc = EncoderClassifier.from_hparams(
        source="speechbrain/spkrec-ecapa-voxceleb",
        savedir="re/.speechbrain/ecapa",
        run_opts={"device": "cpu"},
    )
    resamplers: dict[int, torchaudio.transforms.Resample] = {}

    def load_wav(path):
        w = wave.open(str(path), "rb")
        sr = w.getframerate()
        nch = w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
        w.close()
        x = x.astype(np.float32) / 32768.0
        if nch > 1:
            x = x.reshape(-1, nch).mean(1)
        return torch.from_numpy(x).unsqueeze(0), sr

    wavs = sorted(args.dir.glob("*.wav"))
    names, embs, f0s = [], [], []
    for i, wav in enumerate(wavs):
        sig, sr = load_wav(wav)
        if sr != 16000:
            if sr not in resamplers:
                resamplers[sr] = torchaudio.transforms.Resample(sr, 16000)
            sig = resamplers[sr](sig)
        if sig.shape[1] < 1600:  # < 0.1s -> pad so the model is happy
            sig = torch.nn.functional.pad(sig, (0, 1600 - sig.shape[1]))
        with torch.no_grad():
            e = enc.encode_batch(sig).squeeze().cpu().numpy().astype(np.float32)
        names.append(wav.name)
        embs.append(e)
        f0s.append(median_f0(wav))
        if (i + 1) % 50 == 0 or i + 1 == len(wavs):
            print(f"  ...{i + 1}/{len(wavs)} embedded")

    np.savez(args.out, names=np.array(names), emb=np.stack(embs),
             f0=np.array(f0s, dtype=np.float32))
    print(f"[embed] wrote {len(names)} embeddings -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

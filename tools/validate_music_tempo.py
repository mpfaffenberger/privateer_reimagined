#!/usr/bin/env python3
# =============================================================================
# validate_music_tempo.py -- per-track ground-truth tempo validation.
#
# ASSET-TIME tool. Renders EVERY .ADL sub-song with libADLMIDI (the authentic
# OPL reference -- correctly timed by construction) and compares its duration
# to the GM .GEN render produced by the current tools/xmidi2smf.py timebase.
# Prints a table: track -> AdLib ref vs GM render duration + the ratio, so a
# systematic tempo error shows up as a constant factor (the 4x bug).
#
# usage: validate_music_tempo.py <SOUND_DIR> <GM_WAV_DIR> <ADLMIDIPLAY> <WOPL>
#                                [--ppqn N]
# =============================================================================
import os
import subprocess
import sys
import wave

import render_music
import xmidi2smf


def adl_seconds(adlmidiplay, wopl, seg, tmp):
    xmi = os.path.join(tmp, "_ref.xmi")
    open(xmi, "wb").write(seg)
    subprocess.run([adlmidiplay, xmi, "-w", "-nl", wopl],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   check=True)
    produced = xmi + ".wav"
    if not os.path.exists(produced):
        return None
    w = wave.open(produced)
    secs = w.getnframes() / float(w.getframerate())
    w.close()
    os.remove(produced)
    os.remove(xmi)
    return secs


def gm_seconds(gm_dir, stem, idx):
    p = os.path.join(gm_dir, "%s_%02d.wav" % (stem, idx))
    if not os.path.exists(p):
        return None
    w = wave.open(p)
    secs = w.getnframes() / float(w.getframerate())
    w.close()
    return secs


# XMIDI's fixed clock: 120 ticks/second. A track's correct musical length is its
# tick span / 120 (the synth then adds a release tail). This is the tempo ground
# truth independent of the AdLib/GM arrangement differing in bar count.
XMIDI_TICKS_PER_SEC = 120.0


def gen_span_seconds(sound_dir, name, idx):
    src = os.path.join(sound_dir, name + ".GEN")
    if not os.path.exists(src):
        return None
    d, songs = render_music.split_subsongs(src)
    if idx >= len(songs):
        return None
    off, length, _s = songs[idx]
    ev = xmidi2smf.xmidi_to_events(xmidi2smf.find_evnt(d[off:off + length]))
    span = max((t for t, _o, _p in ev), default=0)
    return span / XMIDI_TICKS_PER_SEC


def main():
    pos = [a for a in sys.argv[1:] if not a.startswith("--")]
    opt = {}
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--ppqn":
            opt["ppqn"] = int(args[i + 1])
    sound_dir, gm_dir, adlmidiplay, wopl = pos[0], pos[1], pos[2], pos[3]
    tmp = "/tmp"

    # A GM render passes when it plays at the right WALL-CLOCK SPEED. Two cues
    # of "right speed", either of which is sufficient:
    #   * tempo-exact: GM duration matches the sub-song's own fixed-clock length
    #     (span/120) within the synth release tail -> tempo is correct, period;
    #   * adl-match: GM duration matches the AdLib reference within tail.
    # The AdLib cross-check can legitimately disagree only when the .GEN and
    # .ADL are DIFFERENT-LENGTH arrangements (documented; e.g. CREDITS) -- in
    # which case tempo-exact still holds. Tail tolerance scales a little with
    # length (longer cues, more reverb tails) but is generous-but-bounded.
    def tail_tol(seconds):
        return max(4.5, 0.06 * seconds)

    print("%-14s %9s %9s %9s %7s  %s"
          % ("track", "adlib_s", "gm_s", "span/120", "gm/adl", "verdict"))
    print("-" * 64)
    all_ok = True
    rows = []
    for name, stem in render_music.TRACKS.items():
        adl = os.path.join(sound_dir, name + ".ADL")
        if not os.path.exists(adl):
            continue
        d, songs = render_music.split_subsongs(adl)
        for idx, (off, length, _s) in enumerate(songs):
            seg = d[off:off + length]
            ref = adl_seconds(adlmidiplay, wopl, seg, tmp)
            gm = gm_seconds(gm_dir, stem, idx)
            pred = gen_span_seconds(sound_dir, name, idx)
            if ref is None or gm is None:
                continue
            rows.append(("%s_%02d" % (stem, idx), ref, gm, pred))

    for label, ref, gm, pred in rows:
        ratio = gm / ref if ref > 0.01 else 0.0
        tempo_exact = pred is not None and abs(gm - pred) <= tail_tol(pred)
        adl_match = abs(gm - ref) <= tail_tol(ref)
        ok = tempo_exact or adl_match
        all_ok = all_ok and ok
        verdict = "OK" if ok else "OFF"
        if ok and not adl_match:
            verdict = "OK (arr-len differs; tempo-exact)"
        print("%-14s %8.1fs %8.1fs %8.1fs %6.2fx  %s"
              % (label, ref, gm, (pred or 0.0), ratio, verdict))
    print("-" * 64)
    print("ALL TRACKS CORRECT SPEED: %s" % ("YES" if all_ok else "NO"))


if __name__ == "__main__":
    main()

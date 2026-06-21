#!/usr/bin/env python3
# =============================================================================
# render_music.py -- render Privateer's original music to WAV.
#
# ASSET-TIME tool only (NOT a runtime dependency). Two render paths:
#
#   HQ  (default): the richer **General MIDI** soundtrack -- the game's `.GEN`
#       XMIDI sequences played through FluidSynth + a GM SoundFont. This is the
#       "16-bit"/real-instruments sound (strings, choir, bells), NOT the bleepy
#       AdLib FM. Pipeline:
#         <TRACK>.GEN --(split np-xa2)--> per-sub-song XMIDI container
#                     --(xmidi2smf)-----> SMF type-0 .mid
#                     --(fluidsynth)-----> raw WAV
#                     --(ffmpeg)---------> peak-normalised <stem>_NN.wav
#
#   ADLIB (fallback): the original AdLib/OPL2 FM path via libADLMIDI + the
#       game's TIMBRES.AD bank. Used automatically when no FluidSynth/SoundFont
#       is available, so a fresh checkout still produces *something*. Pipeline:
#         TIMBRES.AD  --(ail2wopl)--> privateer.wopl
#         <TRACK>.ADL --(split)-----> per-sub-song XMIDI .xmi
#                     --(adlmidiplay)-> <stem>_NN.wav
#
# A NOTE ON THE THIRD PATH (MT-32): Privateer also shipped a Roland MT-32 mix
# (the same `.GEN` sequences + the custom timbre bank TIMBRES.MT, uploaded to
# the synth via SysEx, driven by ROLAND.DRV). That is the *most* authentic
# Roland sound, but it needs the Munt emulator + the user's own legal MT-32
# control/PCM ROMs, neither of which we ship. When those are present, render
# the same SMFs (see tools/xmidi2smf.py) with mt32emu instead of FluidSynth.
# Until then the GM path above is already a large upgrade over OPL FM.
#
# MULTI-SONG (np-xa2): a Privateer `.ADL`/`.GEN` is NOT a single tune -- it is
# a *concatenation* of complete XMIDI containers, one per sub-song. Each
# container is `FORM..XDIR` (a directory whose INFO chunk names the sequence
# count) immediately followed by `CAT..XMID` holding that many `FORM..XMID`
# sequences. BASETUNE packs 11 such sub-songs (main in-flight theme + per-base
# tunes), COMBAT packs 13. We enumerate them by parsing the XDIR/CAT directory
# ourselves -- the only method that yields the full set -- and render EVERY one.
#
# Rendered WAVs are LOCAL-ONLY (gitignored) -- same legal model as the SFX.
# SoundFonts / MT-32 ROMs are user-supplied and also never committed.
# =============================================================================
import os
import re
import shutil
import struct
import subprocess
import sys
import wave

import ail2wopl
import xmidi2smf

# Track base name (file in DATA/SOUND, sans extension) -> output stem. Each
# holds one OR MANY sub-songs; we render every one as <stem>_NN.wav.
TRACKS = {
    "BASETUNE": "basetune",
    "COMBAT": "combat",
    "OPENING": "opening",
    "VICTORY": "victory",
    "CREDITS": "credits",
}

RATE = 44100

# The per-sub-song stems the runtime music DIRECTOR (src/music.{h,cpp}, np-ida)
# actually loads from asset_dir. We mirror exactly these into asset_dir so the
# game has the bed/sting set it needs -- the F8 labeler reads out_dir (the full
# set), so it's unaffected. Keep in sync with k_track_file in music.cpp.
DIRECTOR_STEMS = {
    "combat_04", "combat_05", "combat_06", "combat_07",   # flight beds
    "combat_08", "combat_09", "combat_10",                 # event stings
    "basetune_00", "basetune_04",                          # landed per-base
    "basetune_01",                                         # menu (New Constantinople)
    "opening_00",                                          # opening cue
}

# A render whose loudest 16-bit sample never crosses this is treated as
# silence/empty (an aborted stinger or a directory-only container) and flagged.
SILENCE_PEAK = 64

# Per-track peak-normalisation ceiling (dBFS). FluidSynth leaves a lot of
# headroom on a multi-channel GM mix, so we lift each render to a consistent,
# full-but-unclipped level instead of shipping quiet tracks.
NORM_TARGET_DB = -1.5

# macOS ships a perfectly good GM bank (the Roland-derived QuickTime/Sound
# Canvas instruments) -- a zero-download default SoundFont. The user can point
# at a nicer .sf2 (FluidR3_GM, GeneralUser GS, Arachno) via $PRIVATEER_SOUNDFONT.
MAC_SYSTEM_DLS = ("/System/Library/Components/CoreAudio.component"
                  "/Contents/Resources/gs_instruments.dls")

DEVNULL = subprocess.DEVNULL


# ---------------------------------------------------------------------------
# Sub-song enumeration (np-xa2) -- shared by BOTH render paths.
# ---------------------------------------------------------------------------
def split_subsongs(path):
    """Parse a `.ADL`/`.GEN` as a run of concatenated XMIDI containers and
    return (data, [(offset, length, seq_count), ...]).

    Each container starts at `FORM` with `XDIR` at +8 and runs to the next
    such marker (or EOF). The XDIR INFO chunk carries the sequence count
    (almost always 1 in Privateer -- the multi-song-ness comes from many
    *containers*, not many sequences inside one)."""
    d = open(path, "rb").read()
    starts = [i for i in range(len(d) - 12)
              if d[i:i + 4] == b"FORM" and d[i + 8:i + 12] == b"XDIR"]
    songs = []
    for k, off in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(d)
        seg = d[off:end]
        seq_count = 1
        p = seg.find(b"INFO")
        if p >= 0 and p + 10 <= len(seg):
            seq_count = struct.unpack_from("<H", seg, p + 8)[0] or 1
        songs.append((off, end - off, seq_count))
    return d, songs


def wav_peak_and_seconds(path):
    """Return (peak_abs_sample, duration_seconds) for a 16-bit PCM WAV."""
    w = wave.open(path)
    try:
        frames = w.getnframes()
        secs = frames / float(w.getframerate())
        raw = w.readframes(frames)
    finally:
        w.close()
    peak = 0
    for (s,) in struct.iter_unpack("<h", raw[: (len(raw) // 2) * 2]):
        a = -s if s < 0 else s
        if a > peak:
            peak = a
    return peak, secs


# ---------------------------------------------------------------------------
# HQ path: General MIDI via xmidi2smf -> FluidSynth -> ffmpeg normalise.
# ---------------------------------------------------------------------------
def find_soundfont(explicit):
    """First existing of: --soundfont arg, $PRIVATEER_SOUNDFONT, macOS DLS."""
    for cand in (explicit, os.environ.get("PRIVATEER_SOUNDFONT"), MAC_SYSTEM_DLS):
        if cand and os.path.exists(cand):
            return cand
    return None


def render_fluid(fluidsynth, soundfont, smf_path, wav_path):
    """Render one SMF to WAV with FluidSynth (no shell, no interactive mode)."""
    subprocess.run([fluidsynth, "-ni", "-F", wav_path, "-r", str(RATE),
                    "-g", "1.0", soundfont, smf_path],
                   stdout=DEVNULL, stderr=DEVNULL, check=True)
    return os.path.exists(wav_path)


def normalize_wav(ffmpeg, src, dst, target_db=NORM_TARGET_DB):
    """Peak-normalise `src` to `target_db` dBFS, force 44.1 kHz/stereo/s16le,
    write to `dst`. Two passes: detect peak, then apply the makeup gain."""
    probe = subprocess.run([ffmpeg, "-i", src, "-af", "volumedetect",
                            "-f", "null", "-"],
                           capture_output=True, text=True).stderr
    m = re.search(r"max_volume:\s*(-?\d+(?:\.\d+)?) dB", probe)
    gain = (target_db - float(m.group(1))) if m else 0.0
    subprocess.run([ffmpeg, "-y", "-i", src, "-af", "volume=%.2fdB" % gain,
                    "-ar", str(RATE), "-ac", "2", "-c:a", "pcm_s16le", dst],
                   stdout=DEVNULL, stderr=DEVNULL, check=True)


def render_one_hq(fluidsynth, soundfont, ffmpeg, seg, wav_path, tmp_dir):
    """One XMIDI container -> normalised GM WAV. Returns True on success."""
    smf = os.path.join(tmp_dir, "_hq.mid")
    raw = os.path.join(tmp_dir, "_hq_raw.wav")
    open(smf, "wb").write(xmidi2smf.convert_container(seg))
    if not render_fluid(fluidsynth, soundfont, smf, raw):
        return False
    normalize_wav(ffmpeg, raw, wav_path)
    for f in (smf, raw):
        if os.path.exists(f):
            os.remove(f)
    return True


# ---------------------------------------------------------------------------
# ADLIB fallback path: libADLMIDI + the game's TIMBRES.AD FM bank.
# ---------------------------------------------------------------------------
def build_wopl_bank(sound_dir, out_dir):
    """TIMBRES.AD -> privateer.wopl (authentic FM bank); return its path."""
    wopl = os.path.join(out_dir, "privateer.wopl")
    timbres = os.path.join(sound_dir, "TIMBRES.AD")
    melodic, perc = ail2wopl.parse_ail(open(timbres, "rb").read())
    open(wopl, "wb").write(ail2wopl.build_wopl(melodic, perc))
    print("bank: %d melodic + %d perc -> %s" % (len(melodic), len(perc), wopl))
    return wopl


def render_one_adlib(player, wopl, seg, wav_path, tmp_dir):
    """One XMIDI container -> OPL WAV via adlmidiplay. Returns True on success."""
    xmi = os.path.join(tmp_dir, "_adlib.xmi")
    open(xmi, "wb").write(seg)
    subprocess.run([player, xmi, "-w", "-nl", wopl],
                   stdout=DEVNULL, stderr=DEVNULL, check=True)
    produced = xmi + ".wav"
    os.remove(xmi)
    if os.path.exists(produced):
        os.replace(produced, wav_path)
        return True
    return False


# ---------------------------------------------------------------------------
# Driver.
# ---------------------------------------------------------------------------
def parse_args(argv):
    """Tiny hand-rolled arg parse (keep deps at zero). Returns a dict."""
    pos, opt = [], {}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a.startswith("--"):
            opt[a[2:]] = argv[i + 1]
            i += 2
        else:
            pos.append(a)
            i += 1
    return pos, opt


def main():
    pos, opt = parse_args(sys.argv[1:])
    if len(pos) < 2:
        print("usage: render_music.py <SOUND_DIR> <OUT_DIR> [ASSET_DIR]\n"
              "                       [--synth hq|adlib]\n"
              "                       [--soundfont PATH] [--fluidsynth PATH]\n"
              "                       [--ffmpeg PATH] [--adlmidiplay PATH]",
              file=sys.stderr)
        return 1
    sound_dir = pos[0]
    out_dir = pos[1]
    asset_dir = pos[2] if len(pos) > 2 else None

    fluidsynth = opt.get("fluidsynth") or shutil.which("fluidsynth")
    ffmpeg = opt.get("ffmpeg") or shutil.which("ffmpeg")
    soundfont = find_soundfont(opt.get("soundfont"))
    adlmidiplay = opt.get("adlmidiplay") or shutil.which("adlmidiplay")

    # Choose the synth: default HQ when the GM toolchain is complete; else fall
    # back to AdLib so a bare checkout still renders.
    synth = opt.get("synth")
    if not synth:
        synth = "hq" if (fluidsynth and ffmpeg and soundfont) else "adlib"
    if synth == "hq" and not (fluidsynth and ffmpeg and soundfont):
        print("HQ path needs fluidsynth + ffmpeg + a SoundFont; missing one. "
              "fluidsynth=%s ffmpeg=%s soundfont=%s"
              % (fluidsynth, ffmpeg, soundfont), file=sys.stderr)
        return 1
    if synth == "adlib" and not adlmidiplay:
        print("AdLib path needs adlmidiplay (libADLMIDI) on PATH or via "
              "--adlmidiplay.", file=sys.stderr)
        return 1

    ext = "GEN" if synth == "hq" else "ADL"
    print("synth: %s   source: *.%s" % (synth.upper(), ext))
    if synth == "hq":
        print("soundfont: %s" % soundfont)

    os.makedirs(out_dir, exist_ok=True)
    if asset_dir:
        os.makedirs(asset_dir, exist_ok=True)

    wopl = build_wopl_bank(sound_dir, out_dir) if synth == "adlib" else None

    grand_total = 0
    for name, stem in TRACKS.items():
        src = os.path.join(sound_dir, "%s.%s" % (name, ext))
        if not os.path.exists(src):
            print("skip (missing): %s.%s" % (name, ext))
            continue

        # Drop any stale single-tune aggregate from older "largest only" runs.
        stale = os.path.join(out_dir, stem + ".wav")
        if os.path.exists(stale):
            os.remove(stale)

        d, songs = split_subsongs(src)
        print("file:  %-13s sub-songs=%d" % (os.path.basename(src), len(songs)))

        for idx, (off, length, _seq) in enumerate(songs):
            seg = d[off:off + length]
            wav = os.path.join(out_dir, "%s_%02d.wav" % (stem, idx))
            if synth == "hq":
                ok = render_one_hq(fluidsynth, soundfont, ffmpeg, seg, wav,
                                   out_dir)
            else:
                ok = render_one_adlib(adlmidiplay, wopl, seg, wav, out_dir)
            if not ok:
                print("  [%s_%02d] FAILED to render" % (stem, idx))
                continue
            peak, dur = wav_peak_and_seconds(wav)
            flag = "  <-- SILENT/empty (stinger?)" if peak < SILENCE_PEAK else ""
            stem_nn = "%s_%02d" % (stem, idx)
            # Mirror the DIRECTOR's beds/stings into asset_dir under the SAME
            # per-sub-song filename the runtime loads (music.cpp's k_track_file).
            mirrored = ""
            if asset_dir and stem_nn in DIRECTOR_STEMS:
                shutil.copy2(wav, os.path.join(asset_dir, stem_nn + ".wav"))
                mirrored = "  -> asset"
            print("  track: %-16s %7.1fs  peak=%-6d%s%s"
                  % (os.path.basename(wav), dur, peak, flag, mirrored))
            grand_total += 1

    print("rendered %d sub-song WAV(s) into %s" % (grand_total, out_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

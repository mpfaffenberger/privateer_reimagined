#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# extract_speech_pak.py -- extract every VOC from Privateer's SPEECH.PAK.
#
# Two output modes (mutually exclusive):
#   (default)   write raw .voc files into OUTDIR (one per PAK entry)
#   --convert-wav OUTDIR
#               write 44.1kHz / mono / s16 WAVs into OUTDIR (suitable for the
#               engine's audio::load, which is WAV-only). Re-encodes via
#               ffmpeg per file. Use this before running the F9 speech labeler.
#
# SPEECH.PAK format (reverse-engineered; cross-checked against wctools/unpak):
#   header: 4 bytes size, 24-bit file count, 24-bit data_begin
#   index table: 4 bytes per entry (3-byte abs offset + 1-byte flag)
#   each data entry: 3-byte length, 1-byte bit_depth, 3-byte voc_data_offset,
#                    1-byte flag, then the VOC payload itself
#
# We just scan for the "Creative Voice File\x1a" magic and slice between
# consecutive magics; the magic itself is always the first byte of a valid VOC,
# so the resulting blob IS the VOC, no trimming needed.
#
# Usage:
#   python3 tools/extract_speech_pak.py SPEECH.PAK OUTDIR              # raw VOC
#   python3 tools/extract_speech_pak.py SPEECH.PAK --convert-wav OUTDIR # WAVs
# -----------------------------------------------------------------------------
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

VOC_MAGIC = b"Creative Voice File\x1a"
FFMPEG = shutil.which("ffmpeg") or "ffmpeg"


def extract_vocs(pak_path: Path):
    """Yield (idx, voc_bytes) for every VOC in the PAK, in PAK order."""
    data = pak_path.read_bytes()
    magics = []
    pos = 0
    while True:
        j = data.find(VOC_MAGIC, pos)
        if j < 0:
            break
        magics.append(j)
        pos = j + 1
    for i, start in enumerate(magics):
        end = magics[i + 1] if i + 1 < len(magics) else len(data)
        yield i, data[start:end]


def write_vocs(pak_path: Path, out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    n = 0
    for idx, blob in extract_vocs(pak_path):
        (out_dir / f"speech_{idx:04d}.voc").write_bytes(blob)
        n += 1
    print(f"[extract] wrote {n} VOCs -> {out_dir}")
    return n


def convert_to_wav(voc_dir: Path, wav_dir: Path):
    wav_dir.mkdir(parents=True, exist_ok=True)
    n_ok = n_skip = 0
    for voc in sorted(voc_dir.glob("speech_*.voc")) + sorted(voc_dir.glob("speech_*.VOC")):
        wav = wav_dir / (voc.stem + ".wav")
        # Skip if the WAV is already newer than the VOC (idempotent re-runs).
        if wav.exists() and wav.stat().st_mtime >= voc.stat().st_mtime:
            n_skip += 1
            continue
        # ffmpeg's Creative VOC demuxer reads the magic + body; -ar/-ac/-c:a
        # set the engine's canonical format (44.1kHz mono PCM s16).
        r = subprocess.run(
            [FFMPEG, "-y", "-loglevel", "error",
             "-i", str(voc),
             "-ar", "44100", "-ac", "1", "-c:a", "pcm_s16le",
             str(wav)],
        )
        if r.returncode == 0:
            n_ok += 1
        else:
            print(f"[convert] ffmpeg failed for {voc.name} (rc={r.returncode})",
                  file=sys.stderr)
    print(f"[convert] wrote {n_ok} WAVs, skipped {n_skip} already-current -> {wav_dir}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pak", type=Path, help="SPEECH.PAK to extract from")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--convert-wav", dest="wav_out", type=Path, default=None,
                      help="Convert each VOC to WAV via ffmpeg into this dir "
                           "(exclusive with the default raw-VOC output)")
    ap.add_argument("raw_out", type=Path, nargs="?",
                    help="Directory to write raw VOCs into (default mode)")
    args = ap.parse_args()

    if args.wav_out:
        # Convert mode: extract VOCs into a private staging dir first, then
        # feed them through ffmpeg. Use a sibling tmp dir to keep the two
        # output trees independent.
        tmp = args.wav_out.parent / ("_voc_stage_" + args.wav_out.name)
        write_vocs(args.pak, tmp)
        convert_to_wav(tmp, args.wav_out)
        # Commented: leave the stage dir for debugging. Re-runs overwrite it.
        # shutil.rmtree(tmp)
    else:
        if not args.raw_out:
            ap.error("OUTDIR required (or pass --convert-wav OUTDIR)")
        write_vocs(args.pak, args.raw_out)


if __name__ == "__main__":
    sys.exit(main() or 0)

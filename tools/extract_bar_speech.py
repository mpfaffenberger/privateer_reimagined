#!/usr/bin/env python3
"""Extract bar/fixer conversations from Privateer's CONV/ directory.

Each bar/fixer conversation consists of:
  <NAME>.PFC - dialog text (variable speaker IDs)
  <NAME>.VPK - audio data (custom Origin format, sliced into per-line entries)

We parse the PFC to extract individual dialog lines with their voice ID,
slice the corresponding VPK into per-line audio entries by index, and write
each as a labelled WAV in OUTDIR.

VPK format (reverse-engineered):
  dword[0]  total file size (LE uint32, == filesize)
  dword[1+]: index table, each entry 4 bytes (3-byte abs offset + 1-byte
             flag 0x20). Reads until first non-0x20 entry or EOF.
  Each audio entry starts at the indexed offset; the first 4 bytes is a
             length field (LE 32-bit) and the remainder is the audio payload
             (raw 8-bit unsigned mono PCM at the configured sample rate).

PFC format (each dialog line is):
  NUL + 'rand_npc\0' (NPC class)
     + 'normal\0\0\0' (state)
     + <voice_id>\0 (e.g. 'shpdlr_1', 'randcu_3', ...)
     + 2 bytes: \x9f\x00 (constant)
     + 1 byte: voice variant (0x11=shipdealer, 0x09=rumor, etc.)
     + NUL
     + ASCII text (dialog)
     + NUL + 0xe0 + NUL (terminator)

Usage:
  python3 tools/extract_bar_speech.py CONV_DIR OUTDIR [--rate 11025]
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path


def parse_vpk(vpk_path: Path):
    """Yield (index, audio_bytes) for each entry in a VPK file."""
    data = vpk_path.read_bytes()
    if len(data) < 8:
        return

    size = struct.unpack_from('<I', data, 0)[0]
    if size != len(data):
        return  # bad file

    # Walk the index: 4-byte entries (3-byte offset + 1-byte flag), reading
    # while flags == 0x20.
    i = 4
    entries = []
    while i + 4 <= len(data):
        e = struct.unpack_from('<I', data, i)[0]
        off = e & 0x00FFFFFF
        flag = (e >> 24) & 0xFF
        if flag != 0x20:
            break
        entries.append(off)
        i += 4

    for j, off in enumerate(entries):
        end = entries[j + 1] if j + 1 < len(entries) else len(data)
        yield j, data[off:end]


def parse_pfc(pfc_path: Path):
    """Yield (index, text) for each dialog entry in a PFC file."""
    data = pfc_path.read_bytes()
    if not data:
        return

    out_idx = 0
    i = 0
    while i < len(data):
        # Each dialog block ends at the next NUL + 0xe0 + NUL terminator.
        j = data.find(b'\x00\xe0\x00', i)
        if j < 0:
            break
        chunk = data[i:j + 3]

        # Extract text: starts after the voice ID marker.
        # The \x9f\x00 marker is in a fixed position before the text.
        voice_marker = chunk.rfind(b'\x9f\x00')
        if voice_marker >= 0:
            text_start = voice_marker + 3
        else:
            text_start = 30
        text_end = chunk.rfind(b'\x00', text_start)
        if text_end <= text_start:
            text = ""
        else:
            text = chunk[text_start:text_end].decode('ascii', 'replace')

        yield out_idx, text
        out_idx += 1
        i = j + 3


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("conv_dir", type=Path, help="CONV/ directory with .VPK/.PFC pairs")
    ap.add_argument("out_dir",   type=Path, help="output directory for WAVs")
    ap.add_argument("--rate", type=int, default=11025,
                    help="sample rate for the raw PCM (default 11025)")
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)

    pkgs = sorted(args.conv_dir.glob("*.VPK"))
    total = 0
    for vpk in pkgs:
        stem = vpk.stem  # e.g., "BUYFIGHT"
        pfc = vpk.with_suffix(".PFC")
        lines = list(parse_pfc(pfc)) if pfc.exists() else []
        for idx, audio in parse_vpk(vpk):
            label = ""
            if idx < len(lines):
                _, label = lines[idx]
            # Sanitize label for filename.
            safe = "".join(c if c.isalnum() else "_" for c in label[:50]).strip("_")
            out_name = f"{stem}_{idx:03d}_{safe}.wav" if safe else f"{stem}_{idx:03d}.wav"
            wav_path = args.out_dir / out_name
            r = subprocess.run(
                ["ffmpeg", "-y", "-loglevel", "error",
                 "-f", "u8", "-ar", str(args.rate), "-ac", "1",
                 "-i", "pipe:0", str(wav_path)],
                input=audio,
                capture_output=True,
            )
            if r.returncode != 0:
                print(f"[extract] {vpk.name} entry {idx}: ffmpeg failed "
                      f"({r.returncode}): {r.stderr.decode()[:200]}",
                      file=sys.stderr)
                continue
            total += 1

    print(f"[extract] wrote {total} WAVs -> {args.out_dir}")


if __name__ == "__main__":
    sys.exit(main() or 0)

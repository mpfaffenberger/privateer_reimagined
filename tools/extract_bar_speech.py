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
  Each audio entry starts at the indexed offset and is an **LZW-compressed
  Creative VOC file** (per the WC Encyclopedia + confirmed by decode):
    bytes [0:2]   decompressed length, LE u16
    bytes [2:4]   zero (high word of the length / reserved)
    bytes [4:]    LZW bitstream -> a standard `Creative Voice File` (.VOC)
  LZW variant: LSB-first, 9-bit start growing to 12-bit max, clear code 256,
  end code 257, first dict code 258, NO early width change. The decompressed
  VOC is then handed to ffmpeg, which decodes it to PCM (8-bit u-PCM ~11kHz).
  See docs/bar_speech_vpk_format.md for the full reverse-engineering story.

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


def _lzw_decode_voc(entry: bytes) -> bytes | None:
    """Decode one VPK entry (LZW-compressed VOC) -> raw VOC bytes, or None.

    LZW variant (brute-forced against the 'Creative Voice File' magic +
    the 2-byte decompressed-length oracle): LSB-first bit packing, 9-bit
    codes growing to a 12-bit max, clear=256, end=257, first free code 258,
    increment width when next_code == (1<<width) (no early change).
    """
    if len(entry) < 8:
        return None
    out_len = struct.unpack_from('<H', entry, 0)[0]
    data = entry[4:]
    START_W, MAX_W, CLEAR, END, FIRST = 9, 12, 256, 257, 258

    bitbuf = bitcnt = pos = 0

    def read(n):
        nonlocal bitbuf, bitcnt, pos
        while bitcnt < n:
            if pos >= len(data):
                return None
            bitbuf |= data[pos] << bitcnt
            pos += 1
            bitcnt += 8
        v = bitbuf & ((1 << n) - 1)
        bitbuf >>= n
        bitcnt -= n
        return v

    width = START_W
    table = {i: bytes([i]) for i in range(256)}
    next_code = FIRST
    out = bytearray()
    prev = None
    while len(out) < out_len:
        code = read(width)
        if code is None:
            break
        if code == CLEAR:
            table = {i: bytes([i]) for i in range(256)}
            next_code = FIRST
            width = START_W
            prev = None
            continue
        if code == END:
            break
        if code in table:
            piece = table[code]
        elif code == next_code and prev is not None:
            piece = prev + prev[:1]
        else:
            break  # corrupt stream
        out += piece
        if prev is not None:
            table[next_code] = prev + piece[:1]
            next_code += 1
            if next_code >= (1 << width) and width < MAX_W:
                width += 1
        prev = piece

    if out[:19] != b"Creative Voice File":
        return None
    return bytes(out)


def parse_vpk(vpk_path: Path):
    """Yield (index, entry_bytes) for each entry in a VPK file."""
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
        for idx, entry in parse_vpk(vpk):
            voc = _lzw_decode_voc(entry)
            if voc is None:
                # Empty/terminator entry or undecodable -- skip quietly.
                continue
            label = ""
            if idx < len(lines):
                _, label = lines[idx]
            # Sanitize label for filename.
            safe = "".join(c if c.isalnum() else "_" for c in label[:50]).strip("_")
            out_name = f"{stem}_{idx:03d}_{safe}.wav" if safe else f"{stem}_{idx:03d}.wav"
            wav_path = args.out_dir / out_name
            # ffmpeg auto-detects the Creative VOC container; decode to mono
            # 16-bit PCM at the requested rate.
            r = subprocess.run(
                ["ffmpeg", "-y", "-loglevel", "error",
                 "-i", "pipe:0", "-ar", str(args.rate), "-ac", "1",
                 "-c:a", "pcm_s16le", str(wav_path)],
                input=voc,
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

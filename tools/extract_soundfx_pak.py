#!/usr/bin/env python3
# -----------------------------------------------------------------------------
# extract_soundfx_pak.py — pull every VOC out of Privateer's SOUNDFX.PAK.
#
# unpak (third_party/wctools) only knows SPEECH.PAK's "one VOC" layout, so we
# need our own reader for SOUNDFX.PAK's indexed container. Format (reverse-
# engineered from the GOG file + cross-checked against the VOC magic):
#
#   dword[0]        : total file size (validation; LE uint32, == filesize)
#   dword[1]        : 0xe0000008 — flag byte 0xe0 + 8 (per-file header size)
#   dword[2]        : filesize - 8 (end-of-data sentinel, unflagged)
#   dword[3 ..]     : offset table — each entry 0xe0?????? whose low 24 bits is
#                     the ABSOLUTE offset of a file's 8-byte header; the VOC's
#                     "Creative Voice File" magic sits at offset+8. Table runs
#                     up to the first file (where the data region begins).
#
# The 0xe0 top byte is an Origin flag (mask it off for the 24-bit offset). A few
# index slots hold two concatenated VOCs, so the VOC magic is the authoritative
# record boundary: we scan for every magic and slice file i = [magic_i, magic_i+1).
# ffmpeg reads to the VOC terminator block and ignores the trailing header bytes.
#
# Local-use only: writes into gog_extracted/ (gitignored). Never commit output.
# -----------------------------------------------------------------------------

import struct
import sys
from pathlib import Path

VOC_MAGIC = b"Creative Voice File\x1a"
OFFSET_MASK = 0x00FFFFFF  # strip the 0xe0 flag byte -> 24-bit absolute offset


def find_magics(data):
    out, pos = [], 0
    while True:
        j = data.find(VOC_MAGIC, pos)
        if j < 0:
            break
        out.append(j)
        pos = j + 1
    return out


def parse_index(data):
    """Best-effort index-table parse, for reporting/validation only."""
    filesize = len(data)
    dwords = struct.unpack_from("<%dI" % (len(data) // 4), data, 0)
    if dwords[0] != filesize:
        return None  # not the header we expect; magic scan still works
    first_magic = data.find(VOC_MAGIC)
    table_end = first_magic - 8
    return [dwords[i] & OFFSET_MASK for i in range(3, table_end // 4)]


def main():
    if len(sys.argv) < 3:
        print("usage: extract_soundfx_pak.py SOUNDFX.PAK OUTDIR [--list]")
        return 2
    pak = Path(sys.argv[1])
    outdir = Path(sys.argv[2])
    list_only = "--list" in sys.argv[2:]

    data = pak.read_bytes()
    magics = find_magics(data)
    index = parse_index(data)

    print("[pak] file=%s size=%d" % (pak.name, len(data)))
    print("[pak] VOC magics (files): %d   index-table entries: %s"
          % (len(magics), len(index) if index is not None else "n/a"))

    if not list_only:
        outdir.mkdir(parents=True, exist_ok=True)

    print("[pak] %-4s %-10s %-9s" % ("idx", "voc_off", "voc_len"))
    for i, m in enumerate(magics):
        end = magics[i + 1] if i + 1 < len(magics) else len(data)
        # Trim the next file's 8-byte header when this slot is index-aligned.
        if i + 1 < len(magics):
            end -= 8
        blob = data[m:end]
        print("[pak] %-4d %-10d %-9d" % (i, m, len(blob)))
        if not list_only:
            (outdir / ("sfx_%02d.voc" % i)).write_bytes(blob)

    if not list_only:
        print("[pak] wrote %d VOCs -> %s" % (len(magics), outdir))
    return 0


if __name__ == "__main__":
    sys.exit(main())

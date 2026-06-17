#!/usr/bin/env python3
# =============================================================================
# carve_overlay.py -- carve + disassemble a window of PRCD.EXE's Borland overlay
# pool, for reverse-engineering the MNVR maneuver bytecode (Track 2, np-d2w).
#
# CLEAN-ROOM / ASSET-TIME tool only. It reads the user's OWN legal GOG copy of
# `PRCD.EXE` and writes ONLY local, gitignored artifacts under `re/`. It does
# NOT embed, vendor or commit any game bytes -- the disassembly it prints is a
# derivative of copyrighted code and must stay local (see docs/ai_model.md s7).
#
# Why this exists: PRCD.EXE is a 16-bit real-mode MZ with VROOMM overlays.
# Ghidra's MZ loader maps only the *resident* image, so the AI code (which lives
# in the overlay pool, file offset >= 524128) is invisible by default. This
# carves a flat window and disassembles it with capstone in CS_MODE_16; intra-
# overlay near branches are segment-base-independent, so they resolve correctly.
#
#   python3 tools/ghidra/carve_overlay.py <exe> <file_off_hex> <len> [--save out.bin]
#
# Reproduces the addresses cited in docs/ai_model.md s7 (e.g. the MNVR loader at
# 0xb38f6, the maneuver runtime around 0x834xx).
# =============================================================================
import argparse


def main() -> None:
    ap = argparse.ArgumentParser(description="carve/disassemble a PRCD.EXE overlay window")
    ap.add_argument("exe", help="path to the user's own PRCD.EXE")
    ap.add_argument("offset", help="file offset to start at (hex ok, e.g. 0xb38f6)")
    ap.add_argument("length", help="number of bytes (hex ok)")
    ap.add_argument("--save", help="also write the raw carve to this path (local only)")
    args = ap.parse_args()

    off = int(args.offset, 0)
    length = int(args.length, 0)
    data = open(args.exe, "rb").read()
    window = data[off:off + length]

    if args.save:
        with open(args.save, "wb") as fh:
            fh.write(window)
        print(f"; wrote {len(window)} bytes -> {args.save}")

    try:
        import capstone
    except ImportError:
        raise SystemExit("capstone not installed: pip install capstone")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
    for ins in md.disasm(window, off):
        print(f"{ins.address:#08x}: {ins.bytes.hex():<16} {ins.mnemonic} {ins.op_str}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# =============================================================================
# ail2wopl.py -- convert a Miles AIL ".AD"/".OPL" timbre bank (as used by
#                Wing Commander: Privateer's DATA/SOUND/TIMBRES.AD) into a
#                libADLMIDI WOPL3 bank file.
#
# This is an ASSET-TIME tool only -- it is NOT a runtime dependency of the game.
# It exists so we can render the original AdLib/OPL2 music (.ADL XMIDI files)
# to WAV using the game's own authentic FM instrument definitions.
#
# Byte layout authority: libADLMIDI
#   - AIL bank parsing .............. utils/gen_adldata/file_formats/load_ail.h
#   - WOPL operator slot ordering ... src/adlmidi_cvt.hpp (cvt_generic_to_FMIns)
#   - WOPL file serialization ....... src/wopl/wopl_file.c (WOPL_SaveBankToMem)
#
# AIL instrument (11 OPL register bytes after the 3-byte len/transpose header):
#   [0]=mod 0x20  [1]=mod 0x40  [2]=mod 0x60  [3]=mod 0x80  [4]=mod 0xE0
#   [5]=feedback/connection (0xC0)
#   [6]=car 0x20  [7]=car 0x40  [8]=car 0x60  [9]=car 0x80  [10]=car 0xE0
#
# WOPL operators[]: index 0 == CARRIER, index 1 == MODULATOR (2-op).
# =============================================================================
import struct
import sys

WOPL_MAGIC = b"WOPL3-BANK\x00"
WOPL_VERSION = 3
WOPL_VM_AIL = 7            # volume model enum (WOPL_VolumeModel)
WOPL_INS_4OP = 0x01
INST_SIZE_V3 = 66         # bytes per instrument record (with sounding delays)


def parse_ail(data):
    """Return (melodic, percussion) dicts: patch -> WOPL instrument bytes."""
    melodic = {}      # patch -> 66-byte record
    percussion = {}   # patch -> 66-byte record
    pos = 0
    while pos + 6 <= len(data):
        patch = data[pos]
        bank = data[pos + 1]
        offset = struct.unpack_from("<I", data, pos + 2)[0]
        if patch == 0xFF or bank == 0xFF:
            break
        if patch > 127:
            raise ValueError("patch id > 127 -- not an AIL bank?")
        pos += 6

        length = data[offset] + data[offset + 1] * 256
        notenum = struct.unpack_from("<b", data, offset + 2)[0]
        inscount = (length - 3) // 11
        is_perc = (bank == 0x7F)

        # fb/connection byte is taken from the first 11-byte op block (byte 5).
        fb_c = data[offset + 3 + 5]

        ops = [bytearray(5) for _ in range(4)]   # WOPL: [car1, mod1, car2, mod2]
        flags = 0
        for i in range(min(inscount, 2)):
            o = offset + 3 + i * 11
            blk = data[o:o + 11]
            blk = blk + bytes(11 - len(blk))     # pad if truncated
            car = bytes((blk[6], blk[7], blk[8], blk[9], blk[10]))   # 20,40,60,80,E0
            mod = bytes((blk[0], blk[1], blk[2], blk[3], blk[4]))
            ops[i * 2 + 0][:] = car   # carrier slot
            ops[i * 2 + 1][:] = mod   # modulator slot
            if i == 1:
                flags |= WOPL_INS_4OP

        fb1 = fb_c & 0x0F
        fb2 = (fb_c & 0x0E) | (fb_c >> 7)

        note_off1 = 0 if is_perc else notenum
        perc_key = notenum if is_perc else 0

        rec = bytearray()
        rec += b"\x00" * 32                       # inst_name
        rec += struct.pack(">h", note_off1)       # note_offset1 (BE)
        rec += struct.pack(">h", 0)               # note_offset2
        rec += struct.pack("b", 0)                # midi_velocity_offset
        rec += struct.pack("b", 0)                # second_voice_detune
        rec += struct.pack("B", perc_key & 0xFF)  # percussion_key_number
        rec += struct.pack("B", flags)            # inst_flags
        rec += struct.pack("B", fb1)              # fb_conn1_C0
        rec += struct.pack("B", fb2)              # fb_conn2_C0
        for op in ops:
            rec += bytes(op)                      # 4 * 5 operator bytes
        rec += struct.pack(">H", 0)               # delay_on_ms
        rec += struct.pack(">H", 0)               # delay_off_ms
        assert len(rec) == INST_SIZE_V3, len(rec)

        (percussion if is_perc else melodic)[patch] = bytes(rec)
    return melodic, percussion


def blank_instrument():
    rec = bytearray(b"\x00" * 32)
    rec += struct.pack(">h", 0) + struct.pack(">h", 0)
    rec += struct.pack("b", 0) + struct.pack("b", 0)
    rec += struct.pack("B", 0)
    rec += struct.pack("B", 0x04)   # WOPL_Ins_IsBlank
    rec += struct.pack("B", 0) + struct.pack("B", 0)
    rec += b"\x00" * 20
    rec += struct.pack(">H", 0) + struct.pack(">H", 0)
    return bytes(rec)


def build_wopl(melodic, percussion):
    blank = blank_instrument()
    out = bytearray()
    out += WOPL_MAGIC
    out += struct.pack("<H", WOPL_VERSION)        # version (LE)
    out += struct.pack(">H", 1)                   # melodic bank count (BE)
    out += struct.pack(">H", 1 if percussion else 0)  # percussion bank count
    out += struct.pack("B", 0)                    # opl_flags
    out += struct.pack("B", WOPL_VM_AIL)          # volume_model

    # Bank metadata (version >= 2): name[32] + lsb + msb.
    out += b"TIMBRES".ljust(32, b"\x00") + bytes((0, 0))
    if percussion:
        out += b"TIMBRES-P".ljust(32, b"\x00") + bytes((0, 0))

    # Melodic instrument data (128 slots).
    for patch in range(128):
        out += melodic.get(patch, blank)
    # Percussion instrument data (128 slots).
    if percussion:
        for patch in range(128):
            out += percussion.get(patch, blank)
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        print("usage: ail2wopl.py <TIMBRES.AD> <out.wopl>", file=sys.stderr)
        return 1
    data = open(sys.argv[1], "rb").read()
    melodic, percussion = parse_ail(data)
    wopl = build_wopl(melodic, percussion)
    open(sys.argv[2], "wb").write(wopl)
    print("ail2wopl: %d melodic + %d percussion instruments -> %s (%d bytes)"
          % (len(melodic), len(percussion), sys.argv[2], len(wopl)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

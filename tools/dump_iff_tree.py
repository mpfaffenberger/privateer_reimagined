#!/usr/bin/env python3
"""Dump the IFF chunk tree of a Privateer appearance file. Exploration tool."""
import sys, struct

def walk(b, pos, end, depth=0):
    while pos + 8 <= end:
        tag = b[pos:pos+4].decode("latin1", "replace")
        size = struct.unpack(">I", b[pos+4:pos+8])[0]
        data_start = pos + 8
        pad = "  " * depth
        # FORM containers recurse with a 4-byte type tag first
        if tag == "FORM":
            inner_type = b[data_start:data_start+4].decode("latin1", "replace")
            print(f"{pad}FORM ({size:#x}) {inner_type}  @0x{pos:x}")
            walk(b, data_start + 4, data_start + size, depth + 1)
            pos = data_start + size + (size & 1)
        else:
            preview = b[data_start:data_start+min(16, size)].hex()
            print(f"{pad}{tag} ({size:#x}) @0x{pos:x}  data: {preview}")
            pos = data_start + size + (size & 1)
        if pos <= data_start:  # safety
            break

if __name__ == "__main__":
    b = open(sys.argv[1], "rb").read()
    print(f"=== {sys.argv[1]} ({len(b)} bytes) ===")
    walk(b, 0, len(b))

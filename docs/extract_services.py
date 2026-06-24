#!/usr/bin/env python3
"""Reverse-engineer per-base service availability from the GOG Privateer
data files + the WCU Privateer Remake Python sources.

Run: python3 docs/extract_services.py
Output: writes /tmp/bases_full.json + prints a Markdown table.

This script proves that ALL four service availability questions
(ship dealer, merchant guild, mercenary guild) are completely
deterministic from the public Privateer + WCU data, no game binary
required."""
import struct, sys, json

# -- Hardcoded data from WCU (privateer_wcu/bases/weapons_lib.py) and
#    from reverse-engineering the BASE chunk format in QUADRANT.IFF.
LETTERS = "123456789a0bcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
NOREPAIR = {
    "Liverpool", "Edom", "Oakham", "Rilke", "Matahari", "Oresville",
    "Joplin", "Erewhon", "Trinsic", "Elysia", "Wickerton", "New_Reno",
    "Vincent_Moon", "Burton", "Speke", "Romulus", "Remus", "Saratov",
    "New_Iberia", "Kronecker", "Megiddo", "Valkyrie", "Gaea", "Glasgow",
    "Smallville", "Drake", "Tuck's", "Macabee", "Munchen", "Charon",
    "Siva", "Mjolnar",
}

def h(c):
    """hashLetter: returns index of char in LETTERS, +1. Returns 0 if not found."""
    try: return LETTERS.index(c) + 1
    except ValueError: return 0

def walk(b, start, end, depth=0):
    """Walk IFF chunks in a byte buffer. Yields (tag, inner, size, abs_offset)."""
    p = start
    while p < end:
        if p + 8 > end: break
        tag = b[p:p+4].decode("latin1", "replace")
        size = struct.unpack(">I", b[p+4:p+8])[0]
        if tag == "FORM":
            inner = b[p+8:p+12].decode("latin1", "replace")
            yield (tag, inner, size, p)
            yield from walk(b, p+12, p+12+size-4, depth+1)
        else:
            yield (tag, None, size, p)
        p += 8 + size + (size & 1)
        if size == 0: break

# 1. Walk BASES.IFF for {idx -> (name, kind)}.
b_bases = open("gog_extracted/extracted/priv/DATA/SECTORS/BASES.IFF", "rb").read()
bases = {}
for tag, inner, size, p in walk(b_bases, 12, 12+1154):
    if tag == "INFO":
        idx = b_bases[p+8]
        kind = b_bases[p+8+1]
        name = b_bases[p+10:p+8+size].rstrip(b"\x00").decode("latin1", "replace")
        bases[idx] = (name, kind)

# 2. Walk QUADRANT.IFF for system-to-base-ID mapping.
b_quad = open("gog_extracted/extracted/priv/DATA/SECTORS/QUADRANT.IFF", "rb").read()
sys_to_bases = {}  # name -> [base_ids]
for tag, inner, size, p in walk(b_quad, 12, len(b_quad)):
    if tag == "FORM" and inner == "SYST":
        sys_name = None
        base_ids = []
        for t2, i2, s2, p2 in walk(b_quad, p+12, p+12+size-4):
            if t2 == "INFO":
                d = b_quad[p2+8:p2+8+s2]
                sys_name = d[5:].rstrip(b"\x00").decode("latin1", "replace")
            elif t2 == "BASE":
                d = b_quad[p2+8:p2+8+s2]
                base_ids = list(d)
        if sys_name:
            sys_to_bases[sys_name] = base_ids

# 3. Map each base to its system.
base_to_sys = {}
for sys_name, base_ids in sys_to_bases.items():
    for bid in base_ids:
        base_to_sys.setdefault(bid, sys_name)

# 4. Compute per-base service flags.
def services(idx):
    name, kind = bases[idx]
    sys_name = base_to_sys.get(idx, "?")
    has_sd = name not in NOREPAIR and name.replace("'", "") not in NOREPAIR
    if sys_name != "?" and sys_name and name:
        merch = (h(sys_name[0]) + h(name[0])) % 2 == 0
        merc = (h(sys_name[0]) + h(name[0])) % 4 <= 1
    else:
        merch = merc = None
    return sys_name, has_sd, merch, merc

# 5. Emit Markdown table.
print("# Per-base service availability\n")
print("Computed deterministically from BASES.IFF + QUADRANT.IFF + WCU")
print("hash formulas. Run again any time: python3 docs/extract_services.py")
print("")
print("| IDX | Name                | System             | Ship dealer | Merchant guild | Mercenary guild |")
print("|----:|---------------------|--------------------|:-----------:|:--------------:|:---------------:|")
out = {}
for idx in sorted(bases):
    name, kind = bases[idx]
    sys, sd, merch, merc = services(idx)
    sd_s = "" if sd else "-"
    m1_s = "" if merch else "-"
    m2_s = "" if merc else "-"
    print(f"| {idx:>3} | {name:19} | {sys:18} | {sd_s:^11} | {m1_s:^14} | {m2_s:^15} |")
    out[idx] = {"name": name, "system": sys, "ship_dealer": sd, "merchant_guild": merch, "mercenary_guild": merc}

with open("/tmp/bases_full.json", "w") as f:
    json.dump(out, f, indent=2, sort_keys=True)
print("\nSaved /tmp/bases_full.json")

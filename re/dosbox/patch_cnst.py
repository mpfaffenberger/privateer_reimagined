# ---------------------------------------------------------------------------
# patch_cnst.py -- "Plan B" binary patcher for Privateer's combat-AI CNST
# fields, used by the patch-and-relaunch playground (no debugger).
#
# This is a TOOL (pure code, no shipped game data), so it IS committable --
# the .gitignore carves it out of the otherwise-ignored re/ tree. The data it
# reads/writes (PRIV.TRE.master, cd/PRIV.TRE) stays gitignored: it's derived
# from the user's OWN legal GOG copy of Wing Commander: Privateer.
#
# WHAT IT DOES
#   Every NPC pilot's combat brain is a 16-byte CNST vector (8 x LE u16) baked
#   into PRIV.TRE (docs/ai_model.md s2.2). Rather than poke RAM in the crash-
#   prone DOSBox-X debugger, we BINARY-PATCH those bytes on a writable copy of
#   the CD and relaunch the game to watch the AI behave.
#
#   For a chosen faction we locate every occurrence of that faction's canonical
#   CNST signature in the PRISTINE master, overwrite ONLY the requested fields
#   (unspecified fields keep their canonical values -> zero drift), and write
#   the result to the writable CD's PRIV.TRE. We ALWAYS start from the master,
#   so re-running with new values can never accumulate drift.
#
# USAGE
#   python3 re/dosbox/patch_cnst.py --faction pirate --f0 5000 --f1 1500 \
#                                   --f3 75 --f6 76
#   python3 re/dosbox/patch_cnst.py --reset        # restore clean master
#
# CNST layout (LE u16):  f0@+0 f1@+2 f2@+4 f3@+6 f4@+8 f5@+10 f6@+12 f7@+14
# ---------------------------------------------------------------------------
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# This script lives at re/dosbox/patch_cnst.py; data sits beside it.
HERE = Path(__file__).resolve().parent
MASTER = HERE / "PRIV.TRE.master"            # pristine, never written
OUT = HERE / "cd" / "PRIV.TRE"               # the writable CD's copy

U16_MAX = 0xFFFF
NUM_FIELDS = 8                                # f0..f7

# ---------------------------------------------------------------------------
# Faction canonical CNST vectors (docs/ai_model.md s2.2, s7.x).
#
# Each vector IS both the search signature (16 LE bytes) AND the baseline we
# patch on top of -- so an unspecified field always falls back to the shipped
# value, never to zero. f3 / f6 are the discriminators between factions:
#   pirate  f3=75 f6=76      militia f3=40 f6=76      default f3=60 f6=76
# (see ai_debug_playground.md s7A for the full signature table.)
# ---------------------------------------------------------------------------
FACTIONS: dict[str, list[int]] = {
    "pirate":  [600, 1500, 45, 75, 2, 2, 76, 0],
    "militia": [600, 1500, 45, 40, 2, 2, 76, 0],
    "default": [600, 1500, 45, 60, 2, 2, 76, 0],
}

FIELD_LABELS = {
    0: "f0 break-off?",
    1: "f1 attack-range?",
    2: "f2",
    3: "f3 aggression",
    4: "f4",
    5: "f5",
    6: "f6 morale/flee",
    7: "f7",
}


# ---------------------------------------------------------------------------
# byte helpers
# ---------------------------------------------------------------------------
def pack_cnst(vec: list[int]) -> bytes:
    """8 x LE u16 -> 16 raw bytes."""
    return struct.pack("<8H", *vec)


def find_all(haystack: bytes, needle: bytes) -> list[int]:
    """Every (overlapping-safe, but these never overlap) offset of needle."""
    offs: list[int] = []
    i = haystack.find(needle)
    while i != -1:
        offs.append(i)
        i = haystack.find(needle, i + 1)
    return offs


# ---------------------------------------------------------------------------
# the patch
# ---------------------------------------------------------------------------
def load_master() -> bytearray:
    if not MASTER.exists():
        sys.exit(f"error: master not found at {MASTER}\n"
                 f"  re-create it: cp re/dosbox/cd/PRIV.TRE {MASTER.name} "
                 f"(from a freshly extracted GAME.GOG).")
    return bytearray(MASTER.read_bytes())


def write_out(data: bytes) -> None:
    if not OUT.parent.exists():
        sys.exit(f"error: writable CD dir missing at {OUT.parent}\n"
                 f"  extract GAME.GOG into it first (see docs).")
    OUT.write_bytes(data)


def reset() -> int:
    """Restore the writable CD's PRIV.TRE to the pristine master, unchanged."""
    data = load_master()
    write_out(bytes(data))
    print(f"reset: wrote pristine master ({len(data):,} bytes) -> {OUT}")
    return 0


def patch(faction: str, overrides: dict[int, int]) -> int:
    canonical = FACTIONS[faction]
    sig = pack_cnst(canonical)

    data = load_master()
    offsets = find_all(bytes(data), sig)

    # Guard: a zero-match patch means the wrong signature/faction -- shout.
    if not offsets:
        sys.exit(
            f"error: 0 CNST blocks matched the '{faction}' signature\n"
            f"  signature: {sig.hex(' ')}\n"
            f"  (wrong --faction, or the master isn't pristine PRIV.TRE?)")

    # Build the patched vector: canonical baseline + only requested overrides.
    new_vec = list(canonical)
    for idx, val in overrides.items():
        new_vec[idx] = val
    new_block = pack_cnst(new_vec)

    # Apply at every matched offset (one per faction combat profile).
    for off in offsets:
        data[off:off + 16] = new_block

    write_out(bytes(data))

    # ---- report -----------------------------------------------------------
    changed = [i for i in range(NUM_FIELDS) if new_vec[i] != canonical[i]]
    print(f"faction       : {faction}")
    print(f"signature     : {sig.hex(' ')}")
    print(f"blocks patched: {len(offsets)}")
    print(f"offsets       : {', '.join(hex(o) for o in offsets)}")
    print("fields (canonical -> patched):")
    for i in range(NUM_FIELDS):
        mark = "  <== changed" if i in changed else ""
        print(f"  {FIELD_LABELS[i]:<18} "
              f"{canonical[i]:>6} -> {new_vec[i]:>6}{mark}")
    if not changed:
        print("note: no fields changed (all values equal canonical) -- "
              "wrote a clean copy.")
    print(f"wrote         : {OUT}")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def parse_field(raw: str, name: str) -> int:
    try:
        val = int(raw, 0)            # 0x.. and decimal both accepted
    except ValueError:
        sys.exit(f"error: --{name} must be an integer, got {raw!r}")
    if not 0 <= val <= U16_MAX:
        sys.exit(f"error: --{name}={val} out of u16 range [0, {U16_MAX}]")
    return val


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--faction", choices=sorted(FACTIONS), default="pirate",
                   help="which CNST profile to target (default: pirate)")
    for i in range(NUM_FIELDS):
        p.add_argument(f"--f{i}", metavar="N",
                       help=f"override {FIELD_LABELS[i]} (u16)")
    p.add_argument("--reset", action="store_true",
                   help="restore the writable CD's PRIV.TRE from the master")
    args = p.parse_args(argv)

    if args.reset:
        return reset()

    overrides: dict[int, int] = {}
    for i in range(NUM_FIELDS):
        raw = getattr(args, f"f{i}")
        if raw is not None:
            overrides[i] = parse_field(raw, f"f{i}")

    return patch(args.faction, overrides)


if __name__ == "__main__":
    raise SystemExit(main())

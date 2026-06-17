"""Decode Privateer ship-AI definitions from DATA/AIDS/*.IFF → JSON + analysis.

Recon confirmed the original game's combat brain is *data-driven*: every NPC
pilot (faction mooks + named aces) is a small IFF blob of tuning numbers, not
hardcoded C. Three flavours live in DATA/AIDS/:

    FORM AIDS  — a combat pilot.  Children:
        INFO  <7 bytes>     personality / affiliation / refs (layout below)
        CNST  <16 bytes>    8 × LE u16 skill-tuning vector (THE payload)
        MNVR  <N bytes>     maneuver bytecode — UNDECODED, preserved verbatim
                            (opcode semantics need a PRCD.EXE disasm; Track 2)

    FORM ATTD  — ATTITUDE.IFF, the galaxy reaction tables.  Children:
        AROW  <9 bytes> × 9 reaction matrix rows (9 factions × 9 cols)
        DISP  <27 bytes>    disposition table
        FLNG  <8 bytes>     4 × LE i16 signed "feeling" deltas
        THRD  <6 bytes>     3 × LE u16 thresholds
        CNST  <7 bytes>     reaction tuning bytes

    FORM COND  — MANEUVER.IFF, the morale-gated default scripts.  Children:
        CNST  <16 bytes>    default skill vector
        FORM MORL × 3       one per morale tier, each:
            INFO <1 byte>   morale tier index (0,1,2)
            MNVR <N bytes>  that tier's maneuver bytecode (verbatim)

A handful of files in DATA/AIDS/ are NOT combat AI at all — they're comm /
mission scripts (FORM BUSR base chatter, FORM PUSR special-pilot dialogue) and
the MFD face atlas. We record their form type and skip them; they belong to a
different importer.

Legal: this TOOL and the derived docs/ai_model.md are committable. The full
JSON byte-dump it emits under gog_extracted/ai_dump/ is a DERIVATIVE of the
shipped game data and is gitignored (same policy as our audio/sprite extracts).

Re-run:  python3 tools/import_privateer_db/aids.py --summary
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from collections import Counter
from pathlib import Path

# Allow `python3 tools/import_privateer_db/aids.py` to work standalone.
sys.path.insert(0, str(Path(__file__).parent))
import wc_iff  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_IN = REPO_ROOT / "gog_extracted/extracted/priv/DATA/AIDS"
DEFAULT_OUT = REPO_ROOT / "gog_extracted/ai_dump/privateer_ai.json"

# The 8 factions our engine knows (src/faction.h enum ordering). ATTITUDE.IFF
# carries NINE — the extra column is flagged in the analysis, not silently
# squashed. We do NOT assume this order matches Origin's internal AROW order;
# that mapping is INFERRED in ai_model.md and needs EXE confirmation.
ENGINE_FACTIONS = [
    "civilian", "merchant", "confed", "militia",
    "hunter", "pirate", "retro", "kilrathi",
]


# ─────────────────────────────────────────────────────────────────────────────
# low-level field helpers
# ─────────────────────────────────────────────────────────────────────────────


def _u16_vector(body: bytes) -> list[int]:
    """Decode a chunk body as a tight array of LE u16. Trailing odd byte (if
    any) is dropped — every AI chunk we've seen is u16-aligned."""
    n = len(body) // 2
    return list(struct.unpack_from(f"<{n}H", body))


def _i16_vector(body: bytes) -> list[int]:
    n = len(body) // 2
    return list(struct.unpack_from(f"<{n}h", body))


def _mnvr_record(body: bytes) -> dict:
    """Preserve a MNVR script VERBATIM plus cheap, non-interpretive stats.

    We deliberately do NOT tokenise into instructions — instruction lengths
    are unknown until PRCD.EXE is disassembled (Track 2). The byte histogram +
    lead byte are an honest head-start, clearly labelled as un-decoded.
    """
    return {
        "length": len(body),
        "lead_byte": body[0] if body else None,   # likely first opcode
        "hex": body.hex(),
        "sha1": hashlib.sha1(body).hexdigest(),    # de-dupe shared scripts
    }


# ─────────────────────────────────────────────────────────────────────────────
# per-form parsers
# ─────────────────────────────────────────────────────────────────────────────


def _parse_aids(root: wc_iff.Chunk) -> dict:
    """FORM AIDS — one combat pilot."""
    info = root.child("INFO")
    cnst = root.child("CNST")
    mnvr = root.child("MNVR")

    rec: dict = {"form": "AIDS"}

    if info is not None and info.body is not None:
        b = info.body
        # 7-byte INFO. Layout is INFERRED (see ai_model.md): the first four
        # bytes vary per pilot, the last three are ~always zero. We keep the
        # raw bytes so a later disasm pass can re-label without re-extraction.
        rec["info_raw"] = list(b)
        rec["info_hex"] = b.hex()

    if cnst is not None and cnst.body is not None:
        rec["cnst"] = _u16_vector(cnst.body)        # the skill vector
        rec["cnst_hex"] = cnst.body.hex()

    if mnvr is not None and mnvr.body is not None:
        rec["mnvr"] = _mnvr_record(mnvr.body)

    return rec


def _parse_attd(root: wc_iff.Chunk) -> dict:
    """FORM ATTD — the galaxy reaction tables (ATTITUDE.IFF)."""
    rec: dict = {"form": "ATTD"}

    rows = [list(c.body) for c in root.children_by_id("AROW") if c.body]
    rec["arow_matrix"] = rows                       # 9 × 9 reaction codes
    rec["arow_dim"] = [len(rows), len(rows[0]) if rows else 0]

    disp = root.child("DISP")
    if disp is not None and disp.body is not None:
        rec["disp"] = list(disp.body)

    flng = root.child("FLNG")
    if flng is not None and flng.body is not None:
        rec["flng_i16"] = _i16_vector(flng.body)    # signed feeling deltas

    thrd = root.child("THRD")
    if thrd is not None and thrd.body is not None:
        rec["thrd_u16"] = _u16_vector(thrd.body)    # thresholds

    cnst = root.child("CNST")
    if cnst is not None and cnst.body is not None:
        rec["cnst"] = list(cnst.body)               # reaction tuning bytes

    return rec


def _parse_cond(root: wc_iff.Chunk) -> dict:
    """FORM COND — morale-gated default maneuvers (MANEUVER.IFF)."""
    rec: dict = {"form": "COND"}

    cnst = root.child("CNST")
    if cnst is not None and cnst.body is not None:
        rec["cnst"] = _u16_vector(cnst.body)
        rec["cnst_hex"] = cnst.body.hex()

    tiers = []
    for morl in root.children_by_id("MORL"):
        info = morl.child("INFO")
        mnvr = morl.child("MNVR")
        tier = {
            "morale_tier": info.body[0] if (info and info.body) else None,
            "mnvr": _mnvr_record(mnvr.body) if (mnvr and mnvr.body) else None,
        }
        tiers.append(tier)
    rec["morale_tiers"] = tiers
    return rec


# Dispatch table: root form type → parser. Anything not here is a non-AI
# asset (comm/mission script, face atlas) and gets recorded but not decoded.
_PARSERS = {
    "AIDS": _parse_aids,
    "ATTD": _parse_attd,
    "COND": _parse_cond,
}


def parse_aids_dir(in_dir: Path) -> dict:
    """Parse EVERY *.IFF in DATA/AIDS/. Returns a JSON-ready document."""
    pilots: dict[str, dict] = {}
    attitude: dict | None = None
    maneuver: dict | None = None
    other: dict[str, str] = {}

    for path in sorted(in_dir.glob("*.IFF")):
        data = path.read_bytes()
        root = wc_iff.parse_iff(data)
        parser = _PARSERS.get(root.form_type)
        if parser is None:
            # Non-combat-AI asset (BUSR/PUSR comms, MFD faces). Note & skip.
            other[path.stem] = f"{root.id}/{root.form_type}"
            continue
        rec = parser(root)
        rec["_source"] = path.name
        if root.form_type == "AIDS":
            pilots[path.stem] = rec
        elif root.form_type == "ATTD":
            attitude = rec
        elif root.form_type == "COND":
            maneuver = rec

    return {
        "_meta": {
            "source": "PRIV.TRE/DATA/AIDS/*.IFF",
            "pilot_count": len(pilots),
            "non_ai_files": other,
        },
        "pilots": pilots,
        "attitude": attitude,
        "maneuver": maneuver,
    }


# ─────────────────────────────────────────────────────────────────────────────
# analysis — the whole point: which CNST fields VARY (= the skill knobs)
# ─────────────────────────────────────────────────────────────────────────────


def analyse_cnst(doc: dict) -> dict:
    """Tabulate every pilot's CNST vector and find constant vs varying fields.

    The variance IS the labelling key: a field identical across all 30 pilots
    is a global engine limit; a field that climbs from faction-mook to named
    ace is a per-pilot skill knob (aggression / accuracy / morale / …).
    """
    pilots = doc["pilots"]
    if not pilots:
        return {}

    width = max(len(p.get("cnst", [])) for p in pilots.values())
    columns: list[list[int]] = [[] for _ in range(width)]
    for p in pilots.values():
        cnst = p.get("cnst", [])
        for i in range(width):
            if i < len(cnst):
                columns[i].append(cnst[i])

    fields = []
    for i, col in enumerate(columns):
        distinct = sorted(set(col))
        fields.append({
            "index": i,
            "constant": len(distinct) == 1,
            "min": min(col),
            "max": max(col),
            "distinct_values": distinct,
            "distinct_count": len(distinct),
        })

    # Pull the maneuver default vector in for side-by-side context.
    default = doc.get("maneuver", {}).get("cnst") if doc.get("maneuver") else None

    # Pilots with INFO+MNVR but NO CNST chunk fall back to the MANEUVER.IFF
    # default vector at runtime — a real engine behaviour, not a parse miss.
    inherits = sorted(n for n, p in pilots.items() if not p.get("cnst"))

    return {
        "field_width": width,
        "fields": fields,
        "constant_fields": [f["index"] for f in fields if f["constant"]],
        "varying_fields": [f["index"] for f in fields if not f["constant"]],
        "maneuver_default_cnst": default,
        "pilots_inheriting_default_cnst": inherits,
    }


def analyse_mnvr(doc: dict) -> dict:
    """UN-decoded MNVR head-start: byte histogram + unique-script dedupe.

    NOT an instruction decode. Provides Track-2 (Ghidra) with: how many
    distinct scripts exist, which lead bytes (probable entry opcodes) appear,
    and a global byte-value frequency table.
    """
    histogram: Counter = Counter()
    lead_bytes: Counter = Counter()
    scripts: dict[str, list[str]] = {}   # sha1 → [pilots sharing it]

    def _ingest(mnvr: dict | None, owner: str) -> None:
        if not mnvr:
            return
        raw = bytes.fromhex(mnvr["hex"])
        histogram.update(raw)
        if mnvr.get("lead_byte") is not None:
            lead_bytes[mnvr["lead_byte"]] += 1
        scripts.setdefault(mnvr["sha1"], []).append(owner)

    for name, p in doc["pilots"].items():
        _ingest(p.get("mnvr"), name)
    if doc.get("maneuver"):
        for t in doc["maneuver"].get("morale_tiers", []):
            _ingest(t.get("mnvr"), f"MANEUVER.morale{t.get('morale_tier')}")

    return {
        "unique_script_count": len(scripts),
        "scripts_by_sha1": {k: sorted(v) for k, v in scripts.items()},
        "lead_byte_frequency": {f"0x{b:02x}": n
                                for b, n in lead_bytes.most_common()},
        "byte_value_frequency": {f"0x{b:02x}": n
                                 for b, n in histogram.most_common()},
    }


# ─────────────────────────────────────────────────────────────────────────────
# pretty reporting (stdout)
# ─────────────────────────────────────────────────────────────────────────────


def _print_cnst_table(doc: dict, cnst_an: dict) -> None:
    pilots = doc["pilots"]
    width = cnst_an["field_width"]
    print("\n  CNST skill-vector cross-table (LE u16 fields):")
    header = "    " + f"{'pilot':<12}" + "".join(f"f{i:<6}" for i in range(width))
    print(header)
    print("    " + "-" * (12 + 7 * width))
    for name in sorted(pilots):
        cnst = pilots[name].get("cnst", [])
        if not cnst:
            print(f"    {name:<12}(no CNST → inherits MANEUVER.IFF default)")
            continue
        cells = "".join(f"{v:<7}" for v in cnst)
        print(f"    {name:<12}{cells}")
    print("\n  field variance:")
    for f in cnst_an["fields"]:
        kind = "CONST" if f["constant"] else "VARY "
        print(f"    f{f['index']}: {kind}  "
              f"min={f['min']:<6} max={f['max']:<6} "
              f"distinct={f['distinct_values']}")
    print(f"\n  → constant (global limits): {cnst_an['constant_fields']}")
    print(f"  → varying  (skill knobs)  : {cnst_an['varying_fields']}")


def _print_attitude(doc: dict) -> None:
    att = doc.get("attitude")
    if not att:
        return
    print("\n  ATTITUDE reaction matrix (AROW, 8=ally 7=neutral 6=hostile):")
    rows = att["arow_matrix"]
    print("       " + " ".join(f"{i:>2}" for i in range(len(rows[0]))))
    for i, row in enumerate(rows):
        print(f"    {i:>2}: " + " ".join(f"{v:>2}" for v in row))
    print(f"  FLNG (signed feeling deltas): {att.get('flng_i16')}")
    print(f"  THRD (thresholds)          : {att.get('thrd_u16')}")
    print(f"  CNST (reaction tuning)     : {att.get('cnst')}")


def _print_mnvr(mnvr_an: dict) -> None:
    print("\n  MNVR bytecode — UNDECODED (needs PRCD.EXE disasm):")
    print(f"    unique scripts: {mnvr_an['unique_script_count']}")
    print(f"    lead bytes (probable entry opcodes): "
          f"{mnvr_an['lead_byte_frequency']}")
    top = list(mnvr_an["byte_value_frequency"].items())[:12]
    print(f"    top byte values: {dict(top)}")


# ─────────────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--in", dest="in_path", type=Path, default=DEFAULT_IN,
                   help=f"input DATA/AIDS dir (default: "
                        f"{DEFAULT_IN.relative_to(REPO_ROOT)})")
    p.add_argument("--out", dest="out_path", type=Path, default=DEFAULT_OUT,
                   help=f"output JSON dump  (default: "
                        f"{DEFAULT_OUT.relative_to(REPO_ROOT)})")
    p.add_argument("--dump-iff", metavar="FILE",
                   help="print the IFF tree of one AIDS file and exit")
    p.add_argument("--summary", action="store_true",
                   help="print the CNST/ATTITUDE/MNVR analysis to stdout")
    args = p.parse_args(argv)

    if args.dump_iff:
        fp = Path(args.dump_iff)
        if not fp.is_absolute():
            fp = args.in_path / fp
        print(wc_iff.parse_iff(fp.read_bytes()).dump())
        return 0

    if not args.in_path.exists():
        print(f"error: {args.in_path} not found.\n"
              f"  run tools/extract_privateer.sh first.", file=sys.stderr)
        return 1

    doc = parse_aids_dir(args.in_path)
    doc["_analysis"] = {
        "cnst": analyse_cnst(doc),
        "mnvr": analyse_mnvr(doc),
    }

    args.out_path.parent.mkdir(parents=True, exist_ok=True)
    args.out_path.write_text(json.dumps(doc, indent=2))

    out_rel = (args.out_path.relative_to(REPO_ROOT)
               if args.out_path.is_absolute() else args.out_path)
    meta = doc["_meta"]
    print(f"  wrote {out_rel}  —  {meta['pilot_count']} combat pilots")
    print(f"  non-AI files skipped: {len(meta['non_ai_files'])} "
          f"({', '.join(sorted(meta['non_ai_files']))})")

    if args.summary:
        _print_cnst_table(doc, doc["_analysis"]["cnst"])
        _print_attitude(doc)
        _print_mnvr(doc["_analysis"]["mnvr"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

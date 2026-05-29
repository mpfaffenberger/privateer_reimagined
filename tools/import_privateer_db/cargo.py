"""Decode Privateer commodity definitions from CARGO.IFF → cargo.toml.

Origin's CARGO.IFF layout (reverse-engineered by inspection, informed by
dpjudas/WCPrivateer/Sources/FileFormat/WCGameData.h):

    FORM SYST                       — outer wrapper
      FORM DAMG                     — damage placeholder (ignored)
        DAMG <4 bytes>
      TABL <N × LE u32>             — file offsets, one per commodity
      <repeating, N times>:         — Origin extension: size-prefixed FORMs
        u32 LE   size               — total bytes of the FORM that follows
        FORM CRGO                   — standard IFF FORM from here
          INFO <body>:
            u8       category_index   (0,1,2… within category)
            cstring  label            ("Grain", "Iron", …)
            char[8]  "CDBRTYPE"       — literal type-name reference
            cstring  category         ("FOOD", "RAWMAT", "TECH", …)
            ...      trailing bytes   (unknown; preserved as `_trailing_hex`,
                                       likely base price + availability)

The TABL+size-prefix combo is *not* standard IFF — it's an Origin/WC
extension so the runtime can jump to commodity N in O(1) without parsing.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# Allow `python3 tools/import_privateer_db/cargo.py` to work standalone.
sys.path.insert(0, str(Path(__file__).parent))
import wc_iff  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_IN = REPO_ROOT / "gog_extracted/extracted/priv/DATA/TYPES/CARGO.IFF"
DEFAULT_OUT = REPO_ROOT / "docs/privateer_db/cargo.toml"


# ─────────────────────────────────────────────────────────────────────────────
# parsing
# ─────────────────────────────────────────────────────────────────────────────


def _parse_info_body(body: bytes) -> dict:
    """Decode one INFO chunk body into a commodity record."""
    o = 0
    category_index = body[o]
    o += 1
    label, o = wc_iff.read_cstring(body, o)

    type_tag = body[o:o + 8].decode("ascii", errors="replace")
    o += 8
    anomaly = None if type_tag == "CDBRTYPE" else type_tag

    category, o = wc_iff.read_cstring(body, o)
    trailing = body[o:]

    record = {
        "label": label,
        "category": category,
        "category_index": category_index,
        "_trailing_hex": trailing.hex(),
    }
    if anomaly is not None:
        record["_type_tag_anomaly"] = anomaly
    return record


def parse_cargo_file(path: Path) -> dict:
    """Top-level: read CARGO.IFF and return a dict suitable for TOML emission."""
    data = path.read_bytes()
    root = wc_iff.parse_iff(data)
    if not (root.id == "FORM" and root.form_type == "SYST"):
        raise ValueError(f"unexpected root: {root.id}/{root.form_type}")

    tabl = root.child("TABL")
    if tabl is None or tabl.body is None:
        raise ValueError("CARGO.IFF: missing TABL chunk")

    # TABL body is N little-endian u32 file offsets.
    n = len(tabl.body) // 4
    offsets = list(struct.unpack_from(f"<{n}I", tabl.body))

    commodities: list[dict] = []
    for i, file_off in enumerate(offsets):
        # Origin's extension: each TABL entry points to a 4-byte size prefix,
        # *then* a standard IFF FORM at file_off+4.
        size = struct.unpack_from("<I", data, file_off)[0]
        form, _ = wc_iff.parse_chunk_at(data, file_off + 4)
        if not (form.id == "FORM" and form.form_type == "CRGO"):
            print(f"  warning: cargo[{i}] @{file_off:#x}: "
                  f"expected FORM CRGO, got {form.id}/{form.form_type}",
                  file=sys.stderr)
            continue
        info = form.child("INFO")
        if info is None or info.body is None:
            print(f"  warning: cargo[{i}]: no INFO chunk", file=sys.stderr)
            continue
        record = _parse_info_body(info.body)
        record["_index"] = i
        record["_file_offset"] = file_off
        record["_form_size"] = size
        commodities.append(record)

    return {
        "_meta": {
            "source": "PRIV.TRE/DATA/TYPES/CARGO.IFF",
            "count": len(commodities),
            "categories": sorted({c["category"] for c in commodities}),
        },
        "commodities": commodities,
    }


# ─────────────────────────────────────────────────────────────────────────────
# minimal TOML writer (avoids tomli_w dep)
# ─────────────────────────────────────────────────────────────────────────────


def _toml_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _toml_value(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, str):
        return f'"{_toml_escape(v)}"'
    if isinstance(v, list):
        return "[" + ", ".join(_toml_value(x) for x in v) + "]"
    raise TypeError(f"unsupported TOML value type: {type(v).__name__}")


def _write_toml(out: Path, doc: dict) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []

    lines.append("# Auto-generated by tools/import_privateer_db/cargo.py")
    lines.append("# Source: Origin's PRIV.TRE/DATA/TYPES/CARGO.IFF (clean-room extraction)")
    lines.append("# Re-run with: python3 tools/import_privateer_db/cargo.py")
    lines.append("")

    meta = doc.get("_meta", {})
    if meta:
        lines.append("[_meta]")
        for k, v in meta.items():
            lines.append(f"{k} = {_toml_value(v)}")
        lines.append("")

    for c in doc.get("commodities", []):
        lines.append("[[commodities]]")
        # stable field order: human-readable first, metadata last
        order = ["label", "category", "category_index",
                 "_index", "_file_offset", "_form_size",
                 "_trailing_hex", "_type_tag_anomaly"]
        for k in order:
            if k in c:
                lines.append(f"{k} = {_toml_value(c[k])}")
        # any unexpected extras (forward-compat)
        for k, v in c.items():
            if k not in order:
                lines.append(f"{k} = {_toml_value(v)}")
        lines.append("")

    out.write_text("\n".join(lines))


# ─────────────────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--in", dest="in_path", type=Path, default=DEFAULT_IN,
                   help=f"input CARGO.IFF  (default: {DEFAULT_IN.relative_to(REPO_ROOT)})")
    p.add_argument("--out", dest="out_path", type=Path, default=DEFAULT_OUT,
                   help=f"output TOML       (default: {DEFAULT_OUT.relative_to(REPO_ROOT)})")
    p.add_argument("--dump-iff", action="store_true",
                   help="print IFF tree (top-level FORM only) and exit")
    p.add_argument("--summary", action="store_true",
                   help="print a one-line-per-commodity summary to stdout")
    args = p.parse_args(argv)

    if not args.in_path.exists():
        print(f"error: {args.in_path} not found.\n"
              f"  run tools/extract_privateer.sh first.", file=sys.stderr)
        return 1

    if args.dump_iff:
        print(wc_iff.parse_iff(args.in_path.read_bytes()).dump())
        return 0

    doc = parse_cargo_file(args.in_path)
    _write_toml(args.out_path, doc)

    n = doc["_meta"]["count"]
    cats = doc["_meta"]["categories"]
    out_rel = args.out_path.relative_to(REPO_ROOT) if args.out_path.is_absolute() else args.out_path
    print(f"  wrote {out_rel}  —  {n} commodities across {len(cats)} categories")
    print(f"  categories: {', '.join(cats)}")

    if args.summary:
        print()
        print(f"  {'idx':>3}  {'label':<22}  {'category':<10}  {'sub':>3}  trailing")
        print(f"  {'---':>3}  {'-----':<22}  {'--------':<10}  {'---':>3}  --------")
        for c in doc["commodities"]:
            print(f"  {c['_index']:>3}  {c['label']:<22}  {c['category']:<10}  "
                  f"{c['category_index']:>3}  {c['_trailing_hex']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

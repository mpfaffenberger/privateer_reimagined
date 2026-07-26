"""Export the fixer conversations as a readable screenplay.

Reviewing dialogue inside JSON is miserable, and reviewing it in-game means
replaying the campaign. This dumps every scene in narrative order with speaker
names, prop shots, gating flags, and both branches, so the writing can be read
and judged as writing.

Usage::

    python -m tools.cinematics.export_script            # -> docs/fixer_script.md
    python -m tools.cinematics.export_script --stdout
    python -m tools.cinematics.export_script --scene sandoval
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Narrative order: the campaign as the player experiences it, which is NOT the
# order entries happen to sit in fixers.json.
ARCS: list[tuple[str, list[str]]] = [
    ("ACT I - THE COURIER (Sandoval)", [
        "sandoval_offer",
        "tayla_artifact_handoff",
    ]),
    ("ACT I - THE SMUGGLER (Tayla)", [
        "tayla_m02_offer",
        "tayla_m03_offer", "tayla_m03_debrief",
        "tayla_m04_offer", "tayla_m04_debrief",
        "tayla_m05_offer", "tayla_m05_debrief",
    ]),
    ("ACT II - THE MADE MAN (Lynch)", [
        "lynch_m06_offer", "lynch_m06_debrief",
        "lynch_m07_offer",
        "lynch_m08_offer",
        "lynch_m09_offer",
    ]),
    ("ACT II - THE ARCHIVE (Masterson)", [
        "masterson_m10_offer",
        "masterson_m11_offer",
        "masterson_m12_offer",
        "masterson_m13_offer",
        "oxford_library_scene",
    ]),
    ("ACT III - THE BLOCKADE (Murphy)", [
        "murphy_m14_offer", "murphy_m14_debrief",
        "murphy_m15_offer", "murphy_m15_debrief",
        "murphy_m16_offer",
    ]),
    ("ACT III - THE SCHOLAR (Monkhouse)", [
        "monkhouse_m17_offer", "monkhouse_m17_debrief",
    ]),
    ("ACT IV - THE FRONTIER (Cross)", [
        "cross_m18_offer", "cross_m18_debrief",
        "cross_m19_offer", "cross_m19_debrief",
        "cross_m20_offer", "cross_m20_debrief",
        "cross_m21_offer",
    ]),
    ("ACT IV - THE NAVY (Goodin / Terrell)", [
        "goodin_offer",
        "terrell_offer", "terrell_debrief", "terrell_epilogue",
    ]),
]


def _root() -> Path:
    return Path(__file__).resolve().parents[2]


def render(entry: dict, out: list[str]) -> None:
    name = entry.get("name", entry["id"])
    out.append(f"### {name} — `{entry['id']}`")
    out.append("")

    where = entry.get("base") or ", ".join(entry.get("archetypes", [])) or "?"
    gate = []
    if entry.get("requires_flags"):
        gate.append("requires " + ", ".join(f"`{f}`" for f in entry["requires_flags"]))
    if entry.get("forbids_flags"):
        gate.append("blocked by " + ", ".join(f"`{f}`" for f in entry["forbids_flags"]))
    out.append(f"*Location:* {where}  ")
    out.append(f"*Appears when:* {'; '.join(gate) if gate else 'always'}")
    out.append("")

    def lines(dlg_key: str, spk_key: str, prop_key: str | None = None) -> None:
        dlg = entry.get(dlg_key, [])
        spk = entry.get(spk_key, [])
        props = entry.get(prop_key, []) if prop_key else []
        cast = entry.get("cast", {})
        for i, line in enumerate(dlg):
            key = spk[i] if i < len(spk) else ""
            who = ("GRAYSON" if key == "pc" else
                   cast[key]["name"].upper() if key in cast else name.upper())
            shot = ""
            if i < len(props) and props[i]:
                shot = f"  *[{Path(props[i]).stem}]*"
            if line.strip().strip(". ") == "":
                out.append(f"> **{who}:** *(silence)*{shot}")
            else:
                out.append(f"> **{who}:** {line}{shot}")
        out.append("")

    lines("dialogue", "speaker", "prop")

    if entry.get("offer"):
        out.append(f"**> OFFER:** {entry['offer']}")
        out.append("")

    if entry.get("accept_dialogue"):
        out.append("**— ACCEPT —**")
        out.append("")
        lines("accept_dialogue", "accept_speaker", "accept_prop")

    if entry.get("refuse_dialogue"):
        out.append("**— REFUSE —**")
        out.append("")
        lines("refuse_dialogue", "refuse_speaker")

    acts = (entry.get("accept_actions") or []) + (entry.get("done_actions") or [])
    if acts:
        out.append(f"*Outcome:* {', '.join(f'`{a}`' for a in acts)}")
        out.append("")
    out.append("---")
    out.append("")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--stdout", action="store_true")
    ap.add_argument("--scene", help="only scenes whose id contains this")
    args = ap.parse_args(argv)

    root = _root()
    doc = json.loads((root / "assets" / "data" / "fixers.json").read_text())
    by_id = {e["id"]: e for e in doc["fixers"]}

    out: list[str] = [
        "# Privateer — Bar Fixer Script",
        "",
        "Auto-generated from `assets/data/fixers.json` by",
        "`tools/cinematics/export_script.py`. Do not edit by hand — edit the",
        "authoring scripts and re-run.",
        "",
    ]

    total = pc = 0
    for arc, ids in ARCS:
        shown = [i for i in ids if not args.scene or args.scene in i]
        if not shown:
            continue
        out.append(f"## {arc}")
        out.append("")
        for fid in shown:
            entry = by_id.get(fid)
            if not entry:
                out.append(f"*(missing scene: {fid})*")
                continue
            total += len(entry.get("dialogue", []))
            pc += sum(1 for s in entry.get("speaker", []) if s == "pc")
            render(entry, out)

    # Anything not covered by an arc, so nothing is silently dropped.
    listed = {i for _, ids in ARCS for i in ids}
    orphans = [e for e in doc["fixers"] if e["id"] not in listed]
    if orphans and not args.scene:
        out.append("## UNSORTED")
        out.append("")
        for entry in orphans:
            render(entry, out)

    text = "\n".join(out)
    if args.stdout:
        print(text)
    else:
        dest = root / "docs" / "fixer_script.md"
        dest.write_text(text)
        print(f"[export] wrote {dest}  ({total} lines, {pc} Grayson)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

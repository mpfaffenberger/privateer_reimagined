"""Structural validation for assets/data/fixers.json.

The parallel-array design (dialogue / speaker / voice / prop, plus the
accept_ and refuse_ phases) is easy to author and easy to get subtly wrong:
one inserted line shifts every index after it, and nothing complains until a
character speaks in someone else's voice. This checks the invariants the
engine relies on, so a bad edit fails here rather than in the bar.

Checks:
  * speaker/voice/prop arrays are index-aligned with their dialogue
  * every referenced portrait, prop, and voice clip exists on disk
  * silent beats ("...") carry no audio, and voiced lines are not blank
  * every entry showing a REFUSE button has refuse dialogue to play
  * accept/refuse epilogues are internally consistent

Exit code is non-zero when any ERROR is found (warnings do not fail).

Usage::

    python -m tools.cinematics.validate_fixers
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def is_beat(text: str) -> bool:
    return text.strip().strip(". ") == ""


def main() -> int:
    root = _repo_root()
    cine = root / "assets" / "cinematics"
    doc = json.loads((root / "assets" / "data" / "fixers.json").read_text())

    errors: list[str] = []
    warnings: list[str] = []

    for entry in doc["fixers"]:
        fid = entry["id"]

        for prefix in ("", "accept_", "refuse_"):
            dlg = entry.get(f"{prefix}dialogue" if prefix else "dialogue", [])
            if not dlg:
                continue
            spk = entry.get(f"{prefix}speaker" if prefix else "speaker", [])
            vox = entry.get(f"{prefix}voice" if prefix else "voice", [])
            label = prefix.rstrip("_") or "main"

            if spk and len(spk) != len(dlg):
                errors.append(
                    f"{fid}[{label}]: speaker has {len(spk)} entries, "
                    f"dialogue has {len(dlg)}"
                )
            if vox and len(vox) != len(dlg):
                errors.append(
                    f"{fid}[{label}]: voice has {len(vox)} entries, "
                    f"dialogue has {len(dlg)}"
                )

            for i, clip in enumerate(vox):
                line = dlg[i] if i < len(dlg) else ""
                if clip and not (cine / clip).is_file():
                    errors.append(f"{fid}[{label}][{i}]: missing clip {clip}")
                if clip and is_beat(line):
                    errors.append(
                        f"{fid}[{label}][{i}]: silent beat has audio ({clip})"
                    )
                if not clip and line and not is_beat(line):
                    warnings.append(
                        f"{fid}[{label}][{i}]: voiced line has no clip "
                        f"({line[:40]!r})"
                    )

        props = entry.get("prop", [])
        if props:
            dlg = entry.get("dialogue", [])
            if len(props) != len(dlg):
                errors.append(
                    f"{fid}: prop has {len(props)} entries, dialogue has {len(dlg)}"
                )
            for i, png in enumerate(props):
                if png and not (cine / png).is_file():
                    errors.append(f"{fid}[prop][{i}]: missing art {png}")

        for field in ("portrait", "portrait_pc"):
            png = entry.get(field)
            if png and not (cine / png).is_file():
                errors.append(f"{fid}: missing {field} {png}")

        # A REFUSE button with nothing behind it just blanks the panel.
        if entry.get("offer") and not entry.get("refuse_dialogue"):
            warnings.append(f"{fid}: has an offer but no refuse dialogue")
        if entry.get("offer") and not entry.get("accept_dialogue"):
            warnings.append(f"{fid}: has an offer but no accept dialogue")

    for w in warnings:
        print(f"  WARN  {w}")
    for e in errors:
        print(f"  ERROR {e}")

    print(f"\n[validate-fixers] {len(doc['fixers'])} entries: "
          f"{len(errors)} error(s), {len(warnings)} warning(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

"""publish.py: write a cinematic and its trigger together, or not at all (#368).

Authoring scripts used to ``Cinematic.save()`` first and then patch
``triggers.json``. If the trigger step failed (bad JSON, a duplicate), the
saved cinematic was orphaned, and the scripts' "refuse to reuse an existing
id" guard then blocked every rerun.

``publish()`` fixes the ordering:

1. Run every check that can fail before touching the real files: the id is
   free, triggers.json is well-formed, no duplicate trigger (same cinematic,
   or an identical ``when`` block), and the staged cinematic passes the
   validator.
2. Only then swap the staged files into place with ``os.replace`` (atomic per
   file). If the triggers swap fails, the new cinematic is removed again, so
   the two outputs never disagree.

Staged files live next to their targets so ``os.replace`` stays on one
filesystem, and so the validator resolves assets exactly as the game will.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Callable, List, Optional

from . import validate as _validate

Validator = Callable[[Path], List[dict]]


class PublishError(RuntimeError):
    """Nothing was written: the cinematic or trigger failed a check."""


def _dump(doc: dict) -> str:
    # ensure_ascii=False matches Cinematic.save(): literal UTF-8, not \u escapes.
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def _merged_triggers(triggers_path: Path, cid: str, trigger: dict) -> dict:
    document = json.loads(triggers_path.read_text(encoding="utf-8"))
    triggers = document.get("triggers")
    if not isinstance(triggers, list):
        raise PublishError(f"{triggers_path} has no triggers array; refusing to clobber it")
    if trigger.get("cinematic") != cid:
        raise PublishError(f"trigger names {trigger.get('cinematic')!r}, expected {cid!r}")
    for existing in triggers:
        if existing.get("cinematic") == cid:
            raise PublishError(f"a trigger for {cid} already exists")
        if existing.get("when") == trigger.get("when"):
            raise PublishError(
                f"{cid}'s trigger conditions duplicate {existing.get('cinematic')!r}'s")
    triggers.append(trigger)
    return document


def publish(cinematic, trigger: Optional[dict] = None, *,
            cin_dir: Optional[Path] = None,
            validator: Optional[Validator] = None) -> Path:
    """Validate, then write ``<cin_dir>/<id>.json`` and merge ``trigger``.

    ``cinematic`` is a builder ``Cinematic`` (anything with ``.id`` and
    ``.to_dict()``). ``trigger`` is one triggers.json entry, or None for a
    cinematic that is started some other way. Raises PublishError, leaving
    both files untouched, if any check fails.
    """
    cin_dir = Path(cin_dir) if cin_dir else _validate.cinematics_dir()
    validator = validator or _validate.validate
    cid = cinematic.id
    out = cin_dir / f"{cid}.json"
    triggers_path = cin_dir / "triggers.json"
    staged_cin = cin_dir / f".{cid}.staged.json"
    staged_trig = cin_dir / ".triggers.staged.json"

    if out.exists():
        raise PublishError(f"refusing to reuse existing cinematic id: {cid}")
    try:
        merged = _merged_triggers(triggers_path, cid, trigger) if trigger else None

        staged_cin.write_text(_dump(cinematic.to_dict()), encoding="utf-8")
        issues = validator(staged_cin)
        if _validate.has_errors(issues):
            raise PublishError(f"{cid} failed validation:\n{_validate.format_issues(issues)}")
        if merged is not None:
            staged_trig.write_text(_dump(merged), encoding="utf-8")

        os.replace(staged_cin, out)
        if merged is not None:
            try:
                os.replace(staged_trig, triggers_path)
            except OSError:
                out.unlink(missing_ok=True)   # roll back: no orphan cinematic
                raise
        return out
    finally:
        staged_cin.unlink(missing_ok=True)
        staged_trig.unlink(missing_ok=True)

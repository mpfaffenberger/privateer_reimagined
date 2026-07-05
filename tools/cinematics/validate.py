"""Cinematic validator (Phase 4.1).

Load an ``assets/cinematics/<id>.json`` and check it against the JSON Schema
(``assets/cinematics/schema.json``) PLUS semantic rules the schema can't
express:

* every referenced portrait / audio file actually exists on disk;
* any ``look_at: "ship:<actor>"`` and ``actor_path.actor`` refers to an actor
  ``spawn``-ed earlier in the timeline;
* cue times are non-negative and the ``end`` cue (if any) is the last cue;
* no two ``camera_path`` cues overlap in time;
* every ``line`` cue has a ``camera_path`` covering its full duration
  (convention: close-up on the speaker's ship during dialogue).

Returns a clean list of ``{severity, cue_index, message}`` dicts. ``error``
severity means the cinematic is broken; ``warning`` means a degradable issue
(the engine no-ops missing assets, but the author probably wants to know).

    python -m tools.cinematics.validate intro_troy
"""
from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import List, Optional, Union

try:
    import jsonschema  # type: ignore
    _HAVE_JSONSCHEMA = True
except ImportError:  # optional dep; we still run every semantic rule
    _HAVE_JSONSCHEMA = False


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def cinematics_dir() -> Path:
    return repo_root() / "assets" / "cinematics"


def schema_path() -> Path:
    return cinematics_dir() / "schema.json"


Issue = dict  # {"severity": str, "cue_index": Optional[int], "message": str}


def _issue(severity: str, cue_index, message: str) -> Issue:
    return {"severity": severity, "cue_index": cue_index, "message": message}


def resolve_path(id_or_path: Union[str, Path]) -> Path:
    """Accept a bare id ('intro_troy'), a stem, or a path to the JSON."""
    p = Path(id_or_path)
    if p.suffix == ".json" and p.exists():
        return p
    cand = cinematics_dir() / f"{Path(id_or_path).stem}.json"
    return cand


# ---------------------------------------------------------------------------
# Schema validation (structural)
# ---------------------------------------------------------------------------
def _schema_issues(data: dict) -> List[Issue]:
    if not _HAVE_JSONSCHEMA:
        return [_issue("warning", None,
                       "jsonschema not installed \u2014 skipped structural schema "
                       "check (pip install jsonschema); semantic rules still ran")]
    schema = json.loads(schema_path().read_text())
    validator = jsonschema.Draft7Validator(schema)
    issues: List[Issue] = []
    for err in sorted(validator.iter_errors(data), key=lambda e: list(e.path)):
        # Try to attribute the error to a timeline cue index.
        idx = None
        path = list(err.path)
        if len(path) >= 2 and path[0] == "timeline":
            idx = path[1]
        loc = "/".join(str(x) for x in path) or "<root>"
        issues.append(_issue("error", idx, f"schema: {err.message} (at {loc})"))
    return issues


# ---------------------------------------------------------------------------
# Semantic validation
# ---------------------------------------------------------------------------
def _asset_exists(rel: str) -> bool:
    return (cinematics_dir() / rel).is_file()


def _semantic_issues(data: dict) -> List[Issue]:
    issues: List[Issue] = []
    timeline = data.get("timeline", [])
    if not isinstance(timeline, list):
        return [_issue("error", None, "timeline is not a list")]

    spawned_by_t: List[tuple] = []   # (t, actor) for spawns seen so far
    camera_spans: List[tuple] = []   # (start, end, cue_index)
    end_indices: List[int] = []

    for i, cue in enumerate(timeline):
        if not isinstance(cue, dict):
            issues.append(_issue("error", i, "cue is not an object"))
            continue
        t = cue.get("t")
        cmd = cue.get("cmd")

        # -- times ------------------------------------------------------------
        if not isinstance(t, (int, float)):
            issues.append(_issue("error", i, "cue missing numeric 't'"))
            t = None
        elif t < 0:
            issues.append(_issue("error", i, f"negative cue time t={t}"))

        # -- record spawns (for later reference checks) -----------------------
        if cmd == "spawn":
            actor = cue.get("actor")
            if actor:
                spawned_by_t.append((t if t is not None else 0.0, actor))

        # -- actor references must be spawned earlier -------------------------
        if cmd == "actor_path":
            _check_actor_ref(issues, i, cue.get("actor"), t, spawned_by_t,
                             "actor_path.actor")
        if cmd == "camera_path":
            dur = cue.get("dur", 0.0) or 0.0
            if t is not None:
                camera_spans.append((t, t + dur, i))
            for k in cue.get("keys", []):
                la = k.get("look_at") if isinstance(k, dict) else None
                if isinstance(la, str) and la.startswith("ship:"):
                    _check_actor_ref(issues, i, la[len("ship:"):], t,
                                     spawned_by_t, "camera_path look_at")

        # -- asset existence --------------------------------------------------
        if cmd in ("music", "sfx"):
            f = cue.get("file")
            if f and not _asset_exists(f):
                issues.append(_issue("warning", i,
                              f"{cmd} audio file not found on disk: {f}"))
        if cmd == "line":
            portrait = cue.get("portrait")
            if portrait and not _asset_exists(portrait):
                issues.append(_issue("warning", i,
                              f"portrait not found on disk: {portrait}"))
            vf = cue.get("voice_file")
            if vf and not _asset_exists(vf):
                issues.append(_issue("warning", i,
                              f"voice_file not found on disk: {vf}"))

        if cmd == "end":
            end_indices.append(i)

    # -- end cue must be last ------------------------------------------------
    if len(end_indices) > 1:
        issues.append(_issue("error", end_indices[-1],
                      f"multiple 'end' cues ({len(end_indices)}); expected \u22641"))
    if end_indices:
        # By time: the end cue must have the greatest t of any cue.
        end_i = end_indices[0]
        end_t = timeline[end_i].get("t", 0.0)
        latest_t = max((c.get("t", 0.0) for c in timeline
                        if isinstance(c, dict)), default=0.0)
        if end_t < latest_t:
            issues.append(_issue("error", end_i,
                          f"'end' cue at t={end_t} is not last "
                          f"(a later cue exists at t={latest_t})"))

    # -- overlapping camera paths --------------------------------------------
    camera_spans.sort()
    for a, b in zip(camera_spans, camera_spans[1:]):
        if b[0] < a[1] - 1e-6:   # next starts before current ends
            issues.append(_issue("error", b[2],
                          f"camera_path at t={b[0]} overlaps the previous "
                          f"camera_path (ends t={a[1]:.2f})"))

    # -- dialogue lines should have camera close-up coverage ------------------
    # Convention: during every `line` cue a camera_path must be active and
    # tracking the speaker's ship (look_at: "ship:<actor>").  We warn (not
    # error) since the engine still plays the line without it.
    for i, cue in enumerate(timeline):
        if not isinstance(cue, dict) or cue.get("cmd") != "line":
            continue
        lt = cue.get("t")
        ld = cue.get("dur", 0.0) or 0.0
        if lt is None:
            continue
        covered = False
        for (cs, ce, _ci) in camera_spans:
            if cs <= lt + 1e-6 and ce >= lt + ld - 1e-6:
                covered = True
                break
        if not covered:
            issues.append(_issue("warning", i,
                          f"line at t={lt} (dur {ld}) has no camera_path "
                          f"covering its full duration \u2014 convention is a "
                          f"close-up on the speaker's ship"))

    return issues


# ---------------------------------------------------------------------------
# Encoding validation (engine JSON-parser compatibility)
# ---------------------------------------------------------------------------
# The engine's hand-rolled parser (src/json.cpp) accepts these backslash
# escapes and nothing else. It ALSO reads raw UTF-8 bytes verbatim, and (as of
# the np-cinematic fix) decodes \uXXXX. Historically it did NOT, so an em-dash
# written as \u2014 by an ensure_ascii=True encoder passed Python's validate
# but was REJECTED at runtime ("bad escape sequence"). This scan makes that
# whole class of bug visible in the author->validate step.
_ENGINE_ESCAPES = set('"\\/ntrbf')


def _encoding_issues(path: Path) -> List[Issue]:
    issues: List[Issue] = []
    try:
        raw = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as e:  # pragma: no cover
        return [_issue("error", None, f"cannot read file as UTF-8: {e}")]

    saw_u = False
    i, n = 0, len(raw)
    while i < n:
        if raw[i] == "\\" and i + 1 < n:
            nxt = raw[i + 1]
            if nxt == "u":
                saw_u = True
                i += 6            # skip \uXXXX
                continue
            if nxt not in _ENGINE_ESCAPES:
                issues.append(_issue("error", None,
                    f"escape sequence '\\{nxt}' is NOT supported by the engine "
                    f"JSON parser (src/json.cpp) and will be rejected at "
                    f"runtime"))
            i += 2
            continue
        i += 1

    if saw_u:
        issues.append(_issue("warning", None,
            "file contains \\uXXXX unicode escapes. The engine now decodes "
            "them, but the canonical form is literal UTF-8 \u2014 builder.save() "
            "writes ensure_ascii=False. A \\uXXXX here means it was produced by "
            "an ensure_ascii=True encoder; re-save via the builder to normalise "
            "and avoid depending on the escape-decoding path."))
    return issues


# ---------------------------------------------------------------------------
# Location validation (entry-point teleport)
# ---------------------------------------------------------------------------
# Warnings only — the engine degrades gracefully (unknown system refuses the
# play with a clear message; unknown nav plays in place), but the author
# almost certainly wants to know before Mike does.
def _location_issues(data: dict) -> List[Issue]:
    loc = data.get("location")
    if loc is None:
        return []
    if not isinstance(loc, dict):
        return [_issue("warning", None,
                       "location is not an object \u2014 the engine ignores it "
                       "and plays in place")]
    issues: List[Issue] = []
    system = loc.get("system") or ""
    nav = loc.get("nav") or ""
    system_json = repo_root() / "assets" / "systems" / f"{system}.json"
    if not system:
        issues.append(_issue("warning", None,
                      "location.system is empty \u2014 play will not travel; "
                      "the scene may run against the wrong backdrop"))
    elif not system_json.is_file():
        issues.append(_issue("warning", None,
                      f"location.system '{system}' has no "
                      f"assets/systems/{system}.json \u2014 the engine will "
                      f"refuse the play"))
    if not nav:
        issues.append(_issue("warning", None,
                      "location.nav is empty \u2014 the player won't be snapped "
                      "to the authored geometry"))
    elif system and system_json.is_file():
        try:
            sysdoc = json.loads(system_json.read_text(encoding="utf-8"))
            names = [n.get("name") for n in sysdoc.get("nav_points", [])
                     if isinstance(n, dict)]
            if nav not in names:
                issues.append(_issue("warning", None,
                              f"location.nav '{nav}' is not a nav point in "
                              f"'{system}' (has: {', '.join(filter(None, names))})"))
        except (OSError, json.JSONDecodeError):
            pass  # unreadable system file -- already warned via is_file above
    return issues


def _check_actor_ref(issues, cue_index, actor, t, spawned_by_t, label) -> None:
    if not actor:
        return
    earlier = [a for (st, a) in spawned_by_t
               if a == actor and (t is None or st <= t + 1e-6)]
    if not earlier:
        issues.append(_issue("error", cue_index,
                      f"{label} '{actor}' referenced but never spawned "
                      f"earlier in the timeline"))


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------
def validate(id_or_path: Union[str, Path]) -> List[Issue]:
    """Validate a cinematic. Returns a list of {severity, cue_index, message}."""
    path = resolve_path(id_or_path)
    if not path.is_file():
        return [_issue("error", None, f"cinematic file not found: {path}")]
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        return [_issue("error", None, f"invalid JSON: {e}")]

    return (_schema_issues(data) + _semantic_issues(data)
            + _location_issues(data) + _encoding_issues(path))


def has_errors(issues: List[Issue]) -> bool:
    return any(i["severity"] == "error" for i in issues)


def format_issues(issues: List[Issue]) -> str:
    if not issues:
        return "  (no issues)"
    lines = []
    for i in issues:
        where = f"cue[{i['cue_index']}]" if i["cue_index"] is not None else "file"
        lines.append(f"  [{i['severity'].upper():7}] {where:9} {i['message']}")
    return "\n".join(lines)


def main(argv: Optional[List[str]] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        print("usage: python -m tools.cinematics.validate <id|path>")
        return 2
    target = argv[0]
    issues = validate(target)
    errors = [i for i in issues if i["severity"] == "error"]
    warnings = [i for i in issues if i["severity"] == "warning"]

    print(f"Validating cinematic: {resolve_path(target)}")
    print(format_issues(issues))
    print(f"\n  {len(errors)} error(s), {len(warnings)} warning(s) \u2014 "
          f"{'FAIL' if errors else 'PASS'}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())

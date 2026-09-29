"""Cinematic Studio bridge daemon (Phase C).

The out-of-game half of the Studio (docs/cinematic_studio.md §3). The in-game
ImGui panel (Ctrl+K) writes ``assets/cinematics/studio/requests/<id>.json``;
this daemon polls that directory and answers each request with
``responses/<id>.json`` — ``{"id", "status": "working"|"done"|"error",
"message", "cinematic_id", "log": [...]}``.

Two request kinds:

* ``refine`` — DETERMINISTIC, no LLM. Re-runs ``portraits.gen_line`` /
  ``voices.gen_line_voice`` with the request's per-line overrides (emotion,
  prompt extra, seed image, voice_id, speed), regenerating to the SAME asset
  paths so the cinematic JSON needs no edit; text changes (and newly created
  voice files) are wired in via ``builder.apply_overrides``.
* ``author`` — hands the English brief/trigger/outcome to the
  cinematic-director agent via a headless ``code-puppy --agent
  cinematic-director -p "..."`` subprocess. If the CLI is unavailable (or
  ``STUDIO_BRIDGE_NO_AGENT=1``), the response degrades to ``status:"error"``
  whose message is the exact command to paste manually.

Contract notes (Phase B handoff):
* the bridge CREATES ``responses/`` itself and writes ``status:"working"``
  BEFORE processing so the panel shows progress immediately;
* absent request keys mean "keep" — never treated as empty overrides;
* ``regen`` accepts index lists AND the ``"all"`` shorthand;
* requests are NEVER deleted — the panel owns their lifecycle ([Delete]);
* key softening (documented deviation from the original spec): a missing
  OPENAI/GEMINI key degrades portrait regen to the placeholder backend, and a
  missing MINIMAX_API_KEY skips voice regens with a log note — neither fails
  the whole request. Only structural problems (bad JSON, unknown cinematic,
  out-of-range index) produce ``status:"error"``.

Usage (from the repo root):

    python -m tools.cinematics.studio_bridge              # daemon, 2s poll
    python -m tools.cinematics.studio_bridge --once       # drain queue, exit
    python -m tools.cinematics.studio_bridge --interval 5
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path, PurePosixPath
from typing import List, Optional

try:
    from . import portraits as portraits_mod
    from . import voices as voices_mod
    from .builder import apply_overrides
except ImportError:  # loose-script execution
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import portraits as portraits_mod  # type: ignore
    import voices as voices_mod  # type: ignore
    from builder import apply_overrides  # type: ignore

AGENT_NAME = "cinematic-director"
AGENT_TIMEOUT_S = 2400  # authoring invents characters + paints refs & line
                        # portraits (~45s each) + voices; a real run measured
                        # >10 min, so 600s killed a healthy agent mid-flight.
AGENT_LOG_TAIL = 100   # keep responses readable; agents are chatty


def repo_root() -> Path:
    # tools/cinematics/studio_bridge.py -> repo root is two parents up.
    return Path(__file__).resolve().parents[2]


def studio_dir() -> Path:
    return repo_root() / "assets" / "cinematics" / "studio"


def requests_dir() -> Path:
    return studio_dir() / "requests"


def responses_dir() -> Path:
    return studio_dir() / "responses"


def _log(msg: str) -> None:
    print(f"[bridge] {msg}")


class BridgeError(RuntimeError):
    """A request-level failure: becomes status:'error' with this message."""


# ---------------------------------------------------------------------------
# Response protocol
# ---------------------------------------------------------------------------
def write_response(rid: str, status: str, message: str = "",
                   cinematic_id: str = "", log: Optional[List[str]] = None) -> None:
    responses_dir().mkdir(parents=True, exist_ok=True)
    doc = {"id": rid, "status": status, "message": message,
           "cinematic_id": cinematic_id, "log": list(log or [])}
    out = responses_dir() / f"{rid}.json"
    out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                   encoding="utf-8")


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------
def _line_cues(doc: dict) -> List[dict]:
    """Line cues in timeline order (index 0 = first 'line'), aliasing doc."""
    tl = sorted(doc.get("timeline", []), key=lambda c: c.get("t", 0.0))
    return [c for c in tl if c.get("cmd") == "line"]


def _regen_indices(spec, n_cues: int) -> List[int]:
    """Normalize a regen spec (index list or the 'all' shorthand)."""
    if spec == "all":
        return list(range(n_cues))
    out = []
    for i in spec or []:
        i = int(i)
        if not 0 <= i < n_cues:
            raise BridgeError(
                f"regen index {i} out of range — cinematic has {n_cues} line cue(s)")
        out.append(i)
    return out


def _char_scene_seq(portrait_rel: str):
    """Parse 'portraits/<char>/<scene>_<seq>.png' -> (char, scene, seq).

    Returns None for paths the pipeline didn't produce (no guessing games —
    the caller logs and skips instead of scribbling on the wrong file).
    """
    p = PurePosixPath(portrait_rel)
    if len(p.parts) != 3 or p.parts[0] != "portraits" or p.suffix != ".png":
        return None
    stem = p.stem
    if stem.startswith("_") or "_" not in stem:
        return None  # e.g. '_ref.png' — never regen the identity anchor here
    scene, seq = stem.rsplit("_", 1)
    return p.parts[1], scene, seq


def _load_bible() -> Optional[dict]:
    try:
        return portraits_mod.load_bible()
    except Exception:
        return None


# ---------------------------------------------------------------------------
# kind: "refine" — deterministic regeneration, no LLM
# ---------------------------------------------------------------------------
def process_refine(req: dict, log: List[str]) -> str:
    cid = req.get("cinematic_id") or ""
    if not cid:
        raise BridgeError("refine request has no cinematic_id")
    cine_path = repo_root() / "assets" / "cinematics" / f"{cid}.json"
    if not cine_path.is_file():
        raise BridgeError(f"cinematic not found: assets/cinematics/{cid}.json")

    doc = json.loads(cine_path.read_text(encoding="utf-8"))
    cues = _line_cues(doc)
    overrides = {int(o["index"]): o
                 for o in req.get("line_overrides") or [] if "index" in o}
    for i in overrides:
        if not 0 <= i < len(cues):
            raise BridgeError(
                f"override index {i} out of range — cinematic has {len(cues)} line cue(s)")

    regen = req.get("regen") or {}
    image = req.get("image") or {}

    def text_for(i: int) -> str:
        return overrides.get(i, {}).get("text", cues[i].get("text", ""))

    # pending JSON edits, applied once at the end via apply_overrides.
    pending: dict = {}
    for i, ov in overrides.items():
        if "text" in ov and ov["text"] != cues[i].get("text"):
            pending.setdefault(i, {})["text"] = ov["text"]

    n_portraits = 0
    for i in _regen_indices(regen.get("portraits"), len(cues)):
        n_portraits += _regen_portrait(i, cues[i], overrides.get(i, {}), image,
                                       text_for(i), log)

    n_voices = 0
    for i in _regen_indices(regen.get("voices"), len(cues)):
        new_vf = _regen_voice(i, cues[i], overrides.get(i, {}), text_for(i), log)
        if new_vf:
            n_voices += 1
            if new_vf != cues[i].get("voice_file"):
                pending.setdefault(i, {})["voice_file"] = new_vf

    if pending:
        log.extend(apply_overrides(cine_path, pending))
        log.append(f"updated assets/cinematics/{cid}.json")

    return (f"refined {cid}: {len(pending)} line edit(s), "
            f"{n_portraits} portrait regen(s), {n_voices} voice regen(s)")


def _regen_portrait(i: int, cue: dict, ov: dict, image: dict,
                    text: str, log: List[str]) -> bool:
    rel = cue.get("portrait")
    if not rel:
        log.append(f"portrait {i}: line has no portrait path; skipped")
        return False
    parsed = _char_scene_seq(rel)
    if parsed is None:
        log.append(f"portrait {i}: unrecognized portrait path {rel!r}; skipped")
        return False
    char, scene, seq = parsed

    # style_extra (request-wide) + portrait_prompt_extra (per line) both ride
    # in the prompt, so they land in the content-hash cache key.
    extras = [image.get("style_extra", ""), ov.get("portrait_prompt_extra", "")]
    prompt_extra = " ".join(x.strip() for x in extras if x and x.strip())

    ref_override = None
    seed = ov.get("seed_image")
    if seed:
        p = Path(seed)
        ref_override = p if p.is_absolute() else repo_root() / p
        if not ref_override.is_file():
            log.append(f"portrait {i}: seed_image not found ({seed}); "
                       f"falling back to _ref.png")
            ref_override = None

    # image.quality maps onto the OpenAI backend's OPENAI_IMAGE_QUALITY knob
    # (the placeholder/Gemini backends simply ignore it).
    old_quality = os.environ.get("OPENAI_IMAGE_QUALITY")
    if image.get("quality"):
        os.environ["OPENAI_IMAGE_QUALITY"] = image["quality"]
    try:
        if ref_override is None and not portraits_mod.ref_path(char).is_file():
            portraits_mod.gen_ref(char)  # placeholder-safe identity anchor
        # backend=None -> auto: keyed real backend, else placeholder. That IS
        # the no-key softening: portraits never fail a request for a missing key.
        portraits_mod.gen_line(
            character=char, scene=scene, seq=seq, text=text,
            emotion=ov.get("emotion", "neutral"),
            ref_override=ref_override, prompt_extra=prompt_extra,
        )
        log.append(f"portrait {i}: regenerated -> {rel}")
        return True
    finally:
        if image.get("quality"):
            if old_quality is None:
                os.environ.pop("OPENAI_IMAGE_QUALITY", None)
            else:
                os.environ["OPENAI_IMAGE_QUALITY"] = old_quality


def _regen_voice(i: int, cue: dict, ov: dict, text: str,
                 log: List[str]) -> Optional[str]:
    if not voices_mod.available():
        log.append(f"voice {i}: MINIMAX_API_KEY not set; skipped "
                   f"(kept existing audio)")
        return None
    if not text.strip():
        log.append(f"voice {i}: line has no text; skipped")
        return None

    parsed = _char_scene_seq(cue.get("portrait") or "")
    character = parsed[0] if parsed else f"line{i}"
    # Regenerate to the SAME voice_file path when the line has one; otherwise
    # let voices.py pick its stable hash name and wire it in afterwards.
    out_rel = voices_mod.gen_line_voice(
        character, text, out_rel=cue.get("voice_file"),
        voice_id=ov.get("voice_id"), speed=float(ov.get("speed", 1.0)),
        bible=_load_bible(), force=True,
    )
    if out_rel:
        log.append(f"voice {i}: regenerated -> {out_rel}")
    else:
        log.append(f"voice {i}: synthesis failed; kept existing audio")
    return out_rel


# ---------------------------------------------------------------------------
# kind: "author" — hand the English to the cinematic-director agent
# ---------------------------------------------------------------------------
def author_cinematic_id(req: dict) -> str:
    explicit = str(req.get("cinematic_id") or "").strip()
    if explicit:
        return explicit
    request_id = re.sub(r"[^a-z0-9_]+", "_",
                        str(req.get("id") or "untitled").lower()).strip("_")
    return f"studio_{request_id or 'untitled'}"


def build_author_prompt(req: dict) -> str:
    cid = author_cinematic_id(req)
    parts = [
        f"Author a new in-game cinematic with id '{cid}' for new_privateer.",
        "First read docs/cinematic_studio.md and docs/cinematic_format.md — "
        "they are the contract. Work from the repo root.",
        f"BRIEF: {req.get('brief', '(none given — improvise something short)')}",
    ]
    if req.get("triggers_text"):
        parts.append(
            f"TRIGGER (English): {req['triggers_text']}\n"
            "Compile this into an assets/cinematics/triggers.json entry for "
            "this cinematic. MERGE into the existing 'triggers' array — never "
            "clobber or drop existing entries.")
    if req.get("outcome_text"):
        parts.append(
            f"OUTCOME (English): {req['outcome_text']}\n"
            "Compile this into the cinematic's top-level 'outcome' block "
            "(player_at_nav / spawns) via Cinematic.outcome(...).")
    parts.append(
        "LOCATION (mandatory): ALWAYS call Cinematic.location(system, nav), "
        "deriving system + nav from the trigger description (the trigger's "
        "'system' and 'near_nav.nav'). The engine teleports the player there "
        "before playback — cross-system if needed — so the scene runs on its "
        "authored backdrop. The system must be a real assets/systems/"
        "<system>.json and the nav must be a nav point named in that file; "
        "the outcome's player_at_nav must ALSO be a nav that exists in that "
        "same system (verify both against the system JSON before saving).")
    parts.append(
        "AUDIO (mandatory): music/sfx cue paths resolve relative to "
        "assets/cinematics/, and referencing a file that does not exist "
        "plays SILENCE with a log error. NEVER invent audio filenames. "
        "Music: pick a real track from assets/music/original/ and reference "
        "it as '../music/original/<file>.wav' (combat_NN for tension/battle, "
        "basetune_NN for calm). Sfx: pick real files from assets/sfx/ as "
        "'../sfx/<file>.wav' (laser_fire, explosion_big/small, impact_armor, "
        "lock_seeking, cruise_windup, engine_hum...). ls both dirs and "
        "verify every referenced file exists before saving. Voice lines are "
        "exempt — auto_voices generates those.")
    parts.append(
        "PLAYER SHIP (mandatory): when the scene shows the player's own ship, "
        "spawn that actor with class '$player' — the engine substitutes "
        "whatever the player is actually flying at play time. NEVER hardcode "
        "the player's ship class. Also keep cameras that frame the player "
        "actor within ~2km of its path keys (distant framing reads as a "
        "speck).")
    parts.append(
        "CAMERA (mandatory): during every line of dialogue the camera MUST "
        "be in a CLOSE-UP on the speaker's ship — use follow mode: set "
        "\"follow\": \"<actor>\" on the camera_path cue with keys[0].pos as "
        "the OFFSET from the ship (NOT a world position). The camera "
        "matches the ship's velocity every frame. Use ~300m offset for "
        "fighters (talon, centurion, $player) and ~1500m for capital ships "
        "(drayman, galaxy). look_at should be \"ship:<actor>\". Emit a "
        "camera_path cue that starts at or before each line cue and holds "
        "for the line's full duration. Wide establishing shots are ONLY for "
        "non-dialogue moments. No two camera_path cues may overlap in time. "
        "For formation shots, comma-separated actors: "
        "\"follow\": \"talon1,talon2,...\" targets the centroid.")
    parts.append(
        "CHARACTERS (mandatory): check assets/data/characters.json for the "
        "cast. Use EXISTING characters when appropriate (grayson, etc). For "
        "NEW NPCs, invent a name, NOT a reuse of an existing character — "
        "add them to characters.json with appearance/personality/wardrobe, "
        "then use that character id for portrait + voice generation. NEVER "
        "reuse a named character (like 'Captain Vance' or 'Reesa Kort') for "
        "a different NPC — create a new one.")
    parts.append(
        "VOICES (mandatory): Cinematic(auto_voices=True) generates voices "
        "via the MiniMax T2A API. If you see 'MINIMAX_API_KEY not set' in the "
        "output, voices will be SILENT — this is a failure, not graceful "
        "degradation. STOP and report the error instead of saving a "
        "voiceless cinematic. Every line cue MUST have a voice_file field "
        "that resolves to a real .mp3 on disk.\n"
        "VOICE DIRECTION (MiniMax TTS tags): The MiniMax speech API "
        "(https://platform.minimax.io/docs/api-reference/speech-t2a-http) "
        "supports special tags INSIDE the text string that control "
        "delivery. USE THESE for natural-sounding dialogue:\n"
        "  - Pauses: <#N.N#> inserts a pause of N.N seconds. Example: "
        "'Pender's Star Jump.<#0.5#>Rough corridor.' — the TTS pauses "
        "0.5s after the name, but the on-screen subtitle strips the tag "
        "automatically. Use <#0.3#> for a beat, <#0.5#> for a noticeable "
        "pause, <#1.0#> for a dramatic beat.\n"
        "  - Emotion: the voice_setting supports an 'emotion' field with "
        "values like 'happy', 'sad', 'angry', 'fearful', 'disgusted', "
        "'surprised', 'neutral'. Pass --emotion when using voices.py CLI, "
        "or set it via the builder API. Match the emotion to the scene.\n"
        "  - Interjections: MiniMax supports inline interjection markers "
        "like [laughter], [sigh], [cough], [breath], [gasps] in the text. "
        "Use sparingly for character flavor — a [sigh] from a weary "
        "merchant, a [laughter] from a cocky pirate. These are spoken as "
        "sounds, not read as words. The subtitle renderer does NOT strip "
        "these, so either keep them in the subtitle text (they read fine) "
        "or put them only in the voice text and strip from the subtitle.\n"
        "  - Speed: --speed 0.9 for slower, calmer delivery; --speed 1.1 "
        "for urgent, fast speech. Default is 1.0.\n"
        "  - For nav point tours or enumerated items, ALWAYS use <#0.5#> "
        "after the name/title before the description.\n"
        "  - For dramatic reveals, use <#1.0#> before the key line.\n"
        "The text stored in the cinematic JSON 'text' field should be the "
        "CLEAN readable version (no tags) — the tags go only in the voice "
        "generation text. If you use builder.py with auto_voices=True, pass "
        "the tagged text as the 'voice_text' parameter and the clean text "
        "as 'text'. If 'voice_text' is not supported, put tags in text — "
        "the engine's subtitle renderer strips <#...#> tags automatically.")
    parts.append(
        "PORTRAITS (mandatory): Cinematic(auto_portraits=True) generates "
        "portraits for each line. Verify the portrait PNGs exist on disk "
        "after saving. If portrait generation fails, STOP and report — do "
        "not save a cinematic with missing portraits.")
    parts.append(
        "DO NOT use author_studio_cinematic.py or any stale mirror scripts. "
        "Author FRESH via tools/cinematics/builder.py Cinematic(...) — it is "
        "the canonical authoring API.")
    parts.append(
        f"CINEMATIC ID UNIQUENESS (mandatory): The cinematic id '{cid}' "
        "writes to assets/cinematics/<id>.json. NEVER reuse an existing id "
        "for a new scene — even if the topic overlaps (e.g. do not write a "
        "new 'penders_haulers' that contradicts an existing one). Before "
        "authoring, check `ls assets/cinematics/`; if <id>.json already "
        "exists with a DIFFERENT scene (different cast, different trigger, "
        "different tone), use a NEW id (e.g. 'm22_vera_crusader' vs "
        "'penders_haulers'). One id = one scene, full stop. The same rule "
        "applies to asset filenames: audio/<id>_<speaker>_<seq>.mp3 and "
        "portraits/<speaker>/<id>_<seq>.png are namespaced by cinematic id.")
    style = (req.get("image") or {}).get("style_extra")
    if style:
        parts.append(f"Portrait style direction for every line: {style}")
    parts.append(
        "Steps: 1) author the cinematic with tools/cinematics/builder.py "
        "Cinematic(auto_portraits=True, auto_voices=True) and set the outcome "
        "as above; 2) write it with tools.cinematics.publish.publish(c, "
        "trigger) instead of c.save() plus a hand-edited triggers.json: it "
        "validates first and writes both files or neither; 3) fix any errors "
        "it raises; 4) verify all voice files and portrait PNGs exist on "
        "disk; 5) finish by printing the cinematic id on its own line.")
    return "\n\n".join(parts)


def author_command(req: dict) -> List[str]:
    return ["code-puppy", "--agent", AGENT_NAME, "-p", build_author_prompt(req)]


def process_author(req: dict, log: List[str]) -> str:
    cmd = author_command(req)
    display = " ".join(shlex.quote(c) for c in cmd)

    # Degraded path: no CLI on PATH, or CI/test says "never spawn LLMs".
    if os.environ.get("STUDIO_BRIDGE_NO_AGENT") or shutil.which("code-puppy") is None:
        raise BridgeError(f"run: {display}")

    log.append(f"invoking {AGENT_NAME} headlessly (timeout {AGENT_TIMEOUT_S}s)")
    env = dict(os.environ)
    # Pre-flight: warn about missing API keys that will cause silent failures.
    for key in ("MINIMAX_API_KEY", "OPENAI_API_KEY"):
        if not env.get(key):
            log.append(f"WARNING: {key} not set — voice/portrait generation will fail")
    quality = (req.get("image") or {}).get("quality")
    if quality:
        env["OPENAI_IMAGE_QUALITY"] = quality
    try:
        proc = subprocess.run(cmd, cwd=repo_root(), env=env,
                              capture_output=True, text=True,
                              timeout=AGENT_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        raise BridgeError(
            f"agent timed out after {AGENT_TIMEOUT_S}s; run manually: {display}")

    for line in (proc.stdout or "").splitlines()[-AGENT_LOG_TAIL:]:
        if line.strip():
            log.append(line.rstrip())
    if proc.returncode != 0:
        tail = "\n".join((proc.stderr or "").strip().splitlines()[-5:])
        raise BridgeError(
            f"agent exited {proc.returncode}"
            + (f": {tail}" if tail else "")
            + f" — run manually: {display}")

    cid = author_cinematic_id(req)
    output = repo_root() / "assets" / "cinematics" / f"{cid}.json"
    if not output.is_file():
        raise BridgeError(
            f"agent exited 0 but did not create assets/cinematics/{cid}.json; "
            "inspect the agent log for the aborted generation step")
    return (f"agent finished; check assets/cinematics/{cid}.json, "
            f"then Reload + Play from the Studio panel")


# ---------------------------------------------------------------------------
# Poll loop
# ---------------------------------------------------------------------------
def process_request(req_path: Path, rid: str) -> None:
    try:
        req = json.loads(req_path.read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001 — a broken request must still answer
        write_response(rid, "error", f"unreadable request JSON: {e}")
        _log(f"{rid}: error — unreadable request JSON: {e}")
        return

    kind = req.get("kind") or ""
    cid = author_cinematic_id(req) if kind == "author" else str(req.get("cinematic_id") or "")
    if kind == "author":
        req["cinematic_id"] = cid
    log: List[str] = []
    _log(f"processing {rid} (kind={kind or '?'}, cinematic={cid or '-'})")
    write_response(rid, "working", "processing", cid, log)

    try:
        if kind == "refine":
            message = process_refine(req, log)
        elif kind == "author":
            message = process_author(req, log)
        else:
            raise BridgeError(
                f"unknown kind {kind!r} (expected 'author' or 'refine')")
        write_response(rid, "done", message, cid, log)
        _log(f"{rid}: done — {message}")
    except BridgeError as e:
        write_response(rid, "error", str(e), cid, log)
        _log(f"{rid}: error — {e}")
    except Exception as e:  # noqa: BLE001 — the daemon must outlive any request
        write_response(rid, "error",
                       f"internal: {e.__class__.__name__}: {e}", cid, log)
        _log(f"{rid}: error — internal: {e}")
    # NEVER delete req_path — the Studio panel owns request lifecycle.


def scan_and_process(in_flight: set) -> int:
    """One poll pass. Returns the number of requests processed."""
    requests_dir().mkdir(parents=True, exist_ok=True)
    responses_dir().mkdir(parents=True, exist_ok=True)

    processed = 0
    for req_path in sorted(requests_dir().glob("*.json")):
        rid = req_path.stem  # filename IS the id (Phase-B protocol)
        if rid in in_flight:
            continue
        resp_path = responses_dir() / f"{rid}.json"
        if resp_path.is_file():
            try:
                status = json.loads(resp_path.read_text(encoding="utf-8")).get("status")
            except Exception:
                status = None
            if status in ("done", "error"):
                continue  # terminal — the panel decides what happens next
            # A leftover "working" response means a previous bridge crashed
            # mid-request (single-bridge assumption): reprocess it.
        in_flight.add(rid)
        try:
            process_request(req_path, rid)
        finally:
            in_flight.discard(rid)
        processed += 1
    return processed


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="tools.cinematics.studio_bridge",
        description="Cinematic Studio bridge: answers the in-game panel's "
                    "requests (refine deterministically, author via the "
                    "cinematic-director agent).")
    ap.add_argument("--once", action="store_true",
                    help="process everything pending, then exit")
    ap.add_argument("--interval", type=float, default=2.0,
                    help="poll interval in seconds (default 2)")
    args = ap.parse_args(argv)

    _log(f"watching {requests_dir()} (interval {args.interval}s"
         + (", --once" if args.once else "") + ")")
    in_flight: set = set()
    heartbeat = requests_dir().parent / "bridge.alive"
    while True:
        # Heartbeat: the in-game panel checks this file's mtime to show
        # "bridge online/OFFLINE" (a silent dead bridge cost us 11 minutes once).
        try:
            heartbeat.write_text(str(int(time.time())))
        except OSError:
            pass
        n = scan_and_process(in_flight)
        if args.once:
            _log(f"--once: processed {n} request(s), exiting")
            return 0
        time.sleep(args.interval)


if __name__ == "__main__":
    raise SystemExit(main())

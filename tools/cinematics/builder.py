"""Fluent cinematic authoring API (Phase 4.1).

Emit a **valid** ``assets/cinematics/<id>.json`` from Python instead of
hand-editing JSON. The API mirrors the DSL 1:1 (see ``docs/cinematic_format.md``
and ``assets/cinematics/schema.json``) so every method maps to exactly one cue
command.

    from tools.cinematics.builder import Cinematic

    c = Cinematic("intro_troy", letterbox=True, skippable=True,
                  auto_portraits=True, portrait_backend="placeholder")
    c.at(0.0).fade_in(2.0)
    c.at(0.0).music("audio/intro_theme.wav")
    c.at(0.5).camera_path(keys=[[0, 200, 3000], [1500, 0, 1500]],
                          look_at="ship:hero", ease="smooth", dur=6.0)
    c.at(1.0).spawn("hero", cls="tarsus", pos=[500, 0, 800])
    c.at(3.0).line("grayson", "I didn't sign up for this.",
                   emotion="bitter", side="left", dur=3.5)   # auto-gens art
    c.at(11.5).end(actions=["set_flag:intro_seen"])
    path = c.save()   # writes assets/cinematics/intro_troy.json

**The killer feature:** when ``auto_portraits=True`` (or a per-line
``generate=True``), ``.line()`` invokes ``portraits.gen_line(...)`` to render
the speaker's portrait and wires the returned PNG path straight into the cue's
``portrait`` field. Authoring a line generates its art in one step. A ``qc_fn``
passthrough lets the director agent plug in its own vision judge.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Callable, List, Optional, Sequence, Union

try:
    from . import portraits as portraits_mod
    from . import voices as voices_mod
except ImportError:  # loose-script execution
    import portraits as portraits_mod  # type: ignore
    import voices as voices_mod  # type: ignore

Vec3 = Sequence[float]
# A camera keyframe is either a bare position or a {"pos": ..., "look_at": ...}.
CameraKey = Union[Vec3, dict]
QCFn = Callable[[Path, Path], dict]


def repo_root() -> Path:
    # tools/cinematics/builder.py -> repo root is two parents up.
    return Path(__file__).resolve().parents[2]


def cinematics_dir() -> Path:
    return repo_root() / "assets" / "cinematics"


def _as_pos(v: Vec3) -> List[float]:
    if len(v) != 3:
        raise ValueError(f"position must be [x, y, z], got {v!r}")
    return [float(x) for x in v]


def _rel_to_cinematics(p: Path) -> str:
    """Turn an absolute asset path into the DSL's cinematics-relative form.

    The engine loads ``line.portrait`` / audio paths relative to
    ``assets/cinematics/`` (e.g. ``"portraits/grayson/intro_01.png"``).
    """
    p = Path(p).resolve()
    try:
        return p.relative_to(cinematics_dir().resolve()).as_posix()
    except ValueError:
        # Outside assets/cinematics/ — hand back a posix path and let validate
        # flag it. Never silently corrupt the timeline.
        return p.as_posix()


# ---------------------------------------------------------------------------
# Library helper: surgical edits to an EXISTING cinematic JSON (Studio refine).
# ---------------------------------------------------------------------------
# Fields a refine may touch on a line cue. Everything else (timing, camera,
# spawns) belongs to authoring, not refining.
_OVERRIDABLE_LINE_FIELDS = frozenset({"text", "voice_file", "portrait"})


def apply_overrides(cinematic_json_path: Union[str, Path],
                    line_overrides: dict) -> List[str]:
    """Apply per-line-cue field changes to an existing cinematic JSON.

    ``line_overrides`` maps LINE-CUE INDEX -> {field: value}, where index 0 is
    the first ``line`` cue in timeline order (the Studio request convention,
    docs/cinematic_studio.md §3) and field is one of ``text`` / ``voice_file``
    / ``portrait``. Cues are edited in place and the file is rewritten as raw
    UTF-8 (``ensure_ascii=False``) — untouched cues are never regenerated or
    reordered. Returns human-readable change lines for the bridge's log[].
    """
    path = Path(cinematic_json_path)
    doc = json.loads(path.read_text(encoding="utf-8"))
    # Stable sort by t mirrors the engine's (and save()'s) cue ordering, and
    # the returned dicts alias the originals, so edits land in the document.
    cues = [c for c in sorted(doc.get("timeline", []), key=lambda c: c.get("t", 0.0))
            if c.get("cmd") == "line"]

    changes: List[str] = []
    for index, fields in sorted(line_overrides.items()):
        i = int(index)
        if not 0 <= i < len(cues):
            raise IndexError(
                f"line override index {i} out of range — "
                f"cinematic has {len(cues)} line cue(s)")
        bad = set(fields) - _OVERRIDABLE_LINE_FIELDS
        if bad:
            raise ValueError(
                f"line override {i}: unknown field(s) {sorted(bad)}; "
                f"allowed: {sorted(_OVERRIDABLE_LINE_FIELDS)}")
        for field, value in fields.items():
            if cues[i].get(field) == value:
                continue  # no-op; keep the log honest
            cues[i][field] = value
            changes.append(f"line {i}: {field} -> {value!r}")

    if changes:
        path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
    return changes


class _At:
    """Time-bound cue binder returned by :meth:`Cinematic.at`.

    Every method appends exactly one cue at the bound time and returns ``self``
    so beats at the same instant can be chained: ``c.at(0).fade_in().music(...)``.
    """

    def __init__(self, parent: "Cinematic", t: float):
        if t < 0:
            raise ValueError(f"cue time must be non-negative, got {t}")
        self._p = parent
        self._t = float(t)

    def _add(self, cue: dict) -> "_At":
        cue["t"] = self._t
        self._p._timeline.append(cue)
        return self

    # -- fades ----------------------------------------------------------------
    def fade_in(self, dur: float = 1.0) -> "_At":
        return self._add({"cmd": "fade_in", "dur": float(dur)})

    def fade_out(self, dur: float = 1.0) -> "_At":
        return self._add({"cmd": "fade_out", "dur": float(dur)})

    # -- audio ----------------------------------------------------------------
    def music(self, file: str) -> "_At":
        return self._add({"cmd": "music", "file": file})

    def sfx(self, file: str, pos: Optional[Vec3] = None) -> "_At":
        cue = {"cmd": "sfx", "file": file}
        if pos is not None:
            cue["pos"] = _as_pos(pos)
        return self._add(cue)

    # -- camera / actors ------------------------------------------------------
    def camera_path(self, keys: Sequence[CameraKey], dur: float,
                    look_at: Optional[Union[Vec3, str]] = None,
                    ease: str = "smooth") -> "_At":
        if ease not in ("smooth", "linear"):
            raise ValueError(f"ease must be 'smooth' or 'linear', got {ease!r}")
        norm = [self._camera_key(k) for k in keys]
        # A convenience top-level look_at applies to any key without its own —
        # the common "fly the camera while tracking one target" case (DRY).
        if look_at is not None:
            la = look_at if isinstance(look_at, str) else _as_pos(look_at)
            for k in norm:
                k.setdefault("look_at", la)
        return self._add({"cmd": "camera_path", "dur": float(dur),
                          "ease": ease, "keys": norm})

    @staticmethod
    def _camera_key(k: CameraKey) -> dict:
        if isinstance(k, dict):
            out = {"pos": _as_pos(k["pos"])}
            if "look_at" in k and k["look_at"] is not None:
                la = k["look_at"]
                out["look_at"] = la if isinstance(la, str) else _as_pos(la)
            return out
        return {"pos": _as_pos(k)}

    def spawn(self, actor: str, cls: str, pos: Vec3,
              faction: str = "civilian") -> "_At":
        self._p._spawned.add(actor)
        return self._add({"cmd": "spawn", "actor": actor, "class": cls,
                          "faction": faction, "pos": _as_pos(pos)})

    def actor_path(self, actor: str, keys: Sequence[Vec3], dur: float) -> "_At":
        return self._add({"cmd": "actor_path", "actor": actor, "dur": float(dur),
                          "keys": [{"pos": _as_pos(k)} for k in keys]})

    # -- dialogue -------------------------------------------------------------
    def line(self, character: str, text: str, dur: float = 3.5,
             emotion: str = "neutral", side: str = "left",
             voice_file: Optional[str] = None,
             portrait: Optional[str] = None,
             speaker: Optional[str] = None,
             generate: Optional[bool] = None,
             voice_text: Optional[str] = None,
             voice_emotion: str = "",
             voice_speed: Optional[float] = None) -> "_At":
        """A composite spoken beat: portrait panel + subtitle + optional voice.

        ``character`` is the character-bible id (used for portrait generation
        and to resolve the nameplate). Pass ``speaker=`` to override the
        nameplate text. If portrait auto-generation is on (cinematic-level
        ``auto_portraits`` or per-line ``generate=True``) and no explicit
        ``portrait`` is given, this renders the art via ``portraits.gen_line``
        and wires the resulting PNG path into the cue. ``voice_text`` may carry
        MiniMax pause/interjection tags while ``text`` stays clean for subtitles;
        ``voice_emotion`` and ``voice_speed`` direct delivery without becoming
        runtime fields.
        """
        if side not in ("left", "right"):
            raise ValueError(f"side must be 'left' or 'right', got {side!r}")

        nameplate = speaker or self._p._display_name(character)
        do_gen = self._p._auto_portraits if generate is None else generate

        if portrait is None and do_gen:
            portrait = self._p._generate_portrait(character, text, emotion)

        # Voice auto-gen (MiniMax TTS). Only when no explicit clip was given, so
        # an author can still hand-pick a specific file. Degrades to no voice
        # when MINIMAX_API_KEY is unset (line stays silent w/ subtitle+portrait).
        spoken_text = voice_text if voice_text is not None else text
        if voice_file is None and self._p._auto_voices and spoken_text.strip():
            voice_file = self._p._generate_voice(
                character, spoken_text, emotion=voice_emotion,
                speed=voice_speed)

        cue = {"cmd": "line", "speaker": nameplate, "text": text,
               "side": side, "dur": float(dur)}
        if portrait is not None:
            cue["portrait"] = portrait
        if voice_file is not None:
            cue["voice_file"] = voice_file
        return self._add(cue)

    def subtitle(self, text: str, dur: float = 3.0) -> "_At":
        return self._add({"cmd": "subtitle", "text": text, "dur": float(dur)})

    # -- flow -----------------------------------------------------------------
    def end(self, actions: Optional[Sequence[str]] = None) -> "_At":
        cue = {"cmd": "end"}
        if actions:
            cue["actions"] = list(actions)
        return self._add(cue)


class Cinematic:
    """A cinematic timeline under construction. Call :meth:`save` to emit JSON."""

    def __init__(self, id: str, letterbox: bool = True, skippable: bool = True,
                 auto_portraits: bool = False,
                 portrait_backend: Optional[str] = None,
                 qc_fn: Optional[QCFn] = None,
                 auto_voices: bool = False,
                 voice_speed: float = 1.0):
        self.id = id
        self.letterbox = letterbox
        self.skippable = skippable
        self._timeline: List[dict] = []
        self._spawned: set[str] = set()

        # Portrait auto-generation config.
        self._auto_portraits = auto_portraits
        self._portrait_backend = portrait_backend
        self._qc_fn = qc_fn
        # Voice auto-generation config (MiniMax TTS via voices.py).
        self._auto_voices = auto_voices
        self._voice_speed = voice_speed
        # Per-character sequence counters so each line gets a unique PNG / mp3.
        self._seq: dict[str, int] = {}
        self._vseq: dict[str, int] = {}
        self._bible: Optional[dict] = None
        self._outcome: Optional[dict] = None
        self._location: Optional[dict] = None

    # -- authoring ------------------------------------------------------------
    def at(self, t: float) -> _At:
        """Bind subsequent cue calls to time ``t`` (seconds)."""
        return _At(self, t)

    def location(self, system: str, nav: str) -> "Cinematic":
        """Declare the entry-point location (pre-play teleport).

        Emits the top-level ``location`` block. Before playback the engine
        teleports the player to ``nav`` in ``system`` — queueing a system
        switch first when the player is elsewhere — so the scene always runs
        against its authored backdrop and the outcome's ``player_at_nav``
        can't silently no-op in the wrong system. ``system`` is the galaxy id
        (``assets/systems/<system>.json``); ``nav`` must name a nav point in
        that system. ALWAYS call this when authoring: derive it from the
        trigger's system/near_nav.
        """
        self._location = {"system": system, "nav": nav}
        return self

    def outcome(self, player_at_nav: Optional[str] = None,
                spawns: Optional[Sequence[dict]] = None,
                player_pos: Optional[Sequence[float]] = None) -> "Cinematic":
        """Declare the post-cinematic world state (Studio Phase A2).

        Emits the top-level ``outcome`` block the engine applies when the
        cinematic ends naturally or is skipped (docs/cinematic_studio.md §2):
        teleport the player to ``player_at_nav`` (current system) — or to the
        exact world-coord ``player_pos`` [x,y,z], which takes precedence (for
        finales away from any nav) — and spawn each ``{"class", "faction",
        "count", "hostile"?}`` group nearby.
        """
        block: dict = {}
        if player_at_nav:
            block["player_at_nav"] = player_at_nav
        if player_pos is not None:
            if len(player_pos) != 3:
                raise ValueError("player_pos must be [x, y, z]")
            block["player_pos"] = [float(v) for v in player_pos]
        if spawns:
            block["spawns"] = [dict(s) for s in spawns]
        self._outcome = block
        return self

    # -- portrait helpers -----------------------------------------------------
    def _load_bible(self) -> dict:
        if self._bible is None:
            self._bible = portraits_mod.load_bible()
        return self._bible

    def _display_name(self, character: str) -> str:
        bible = self._load_bible()
        entry = bible.get(character)
        return entry["display_name"] if entry else character

    def _generate_portrait(self, character: str, text: str, emotion: str) -> str:
        """Render this line's portrait and return its cinematics-relative path.

        Ensures the character's canonical ``_ref.png`` exists (gen-ref on
        demand) so ``gen_line``'s reference-conditioning always has an anchor —
        crucial for the offline placeholder demo.
        """
        if not portraits_mod.ref_path(character).is_file():
            portraits_mod.gen_ref(character, backend=self._portrait_backend)

        n = self._seq.get(character, 0) + 1
        self._seq[character] = n
        seq = f"{n:02d}"
        out = portraits_mod.gen_line(
            character=character, scene=self.id, seq=seq,
            text=text, emotion=emotion,
            backend=self._portrait_backend, qc_fn=self._qc_fn,
        )
        return _rel_to_cinematics(out)

    def _generate_voice(self, character: str, text: str, *,
                        emotion: str = "",
                        speed: Optional[float] = None) -> Optional[str]:
        """Synthesize this line via MiniMax and return its cinematics-relative
        path ("audio/<id>_<char>_<NN>.mp3"), or None when unavailable."""
        n = self._vseq.get(character, 0) + 1
        self._vseq[character] = n
        out_rel = f"audio/{self.id}_{character}_{n:02d}.mp3"
        return voices_mod.gen_line_voice(
            character=character, text=text, out_rel=out_rel,
            speed=self._voice_speed if speed is None else speed,
            emotion=emotion, bible=self._load_bible(),
        )

    # -- serialisation --------------------------------------------------------
    def to_dict(self) -> dict:
        # Sort by time for a readable, deterministic file. The director sorts
        # by t anyway, and Python's sort is stable so same-t cue order is kept.
        timeline = sorted(self._timeline, key=lambda c: c["t"])
        doc = {
            "id": self.id,
            "letterbox": self.letterbox,
            "skippable": self.skippable,
        }
        if self._location is not None:
            doc["location"] = self._location
        if self._outcome is not None:
            doc["outcome"] = self._outcome
        doc["timeline"] = timeline
        return doc

    def save(self, path: Optional[Union[str, Path]] = None) -> Path:
        """Write the timeline JSON. Defaults to ``assets/cinematics/<id>.json``."""
        out = Path(path) if path else cinematics_dir() / f"{self.id}.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        # ensure_ascii=False: emit non-ASCII (em-dash, curly quotes, accents)
        # as literal UTF-8 bytes rather than \uXXXX escapes. The engine's
        # hand-rolled JSON parser (src/json.cpp) reads raw UTF-8 fine; it now
        # also understands \u escapes, but writing real UTF-8 keeps the files
        # human-readable and dodges the whole escape-decoding path.
        out.write_text(
            json.dumps(self.to_dict(), indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        return out

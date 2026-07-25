"""Cinematic portrait generation CLI (Phase 3).

Pipeline:  bible  ->  gen-ref (human approve)  ->  gen-line (per line, cached)  ->  qc

    python -m tools.cinematics.portraits gen-ref grayson
    python -m tools.cinematics.portraits gen-line grayson \
        --scene demo --seq 01 --emotion "cold warning, jaw set" \
        --text "Cut your engines and drop the cargo. Last warning."
    python -m tools.cinematics.portraits qc \
        assets/cinematics/portraits/grayson/demo_01.png grayson

The engine only ever loads the resulting 512x640 PNGs by path — all AI lives
here. With no API key set, a labelled placeholder card is drawn instead (zero
spend) so the whole cinematic flow can be demoed offline.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path
from typing import Optional

# Support both `python -m tools.cinematics.portraits` and direct execution.
try:
    from .backends import (PORTRAIT_SIZE, finalize_portrait, get_backend)
    from .cache import PortraitCache
    from . import qc as qc_mod
except ImportError:  # run as a loose script
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from backends import (PORTRAIT_SIZE, finalize_portrait, get_backend)  # type: ignore
    from cache import PortraitCache  # type: ignore
    import qc as qc_mod  # type: ignore


# --- The shared style prefix. KEEP IN SYNC WITH style_bible.md. --------------
STYLE_PREFIX = (
    "Modern painted sci-fi character portrait, clean high-fidelity digital "
    "painting (concept-art quality, not photoreal, not anime, not pixel art). "
    "Chest-up 3/4 view, subject turned about 30 degrees off-camera with face "
    "toward the viewer, head in the upper third. Cool key light from upper "
    "left (cockpit-instrument glow) with a warm rim light lower right; dark "
    "moody cockpit/ship-interior ambience but the face is clearly lit. Simple "
    "dark softly-vignetted background, no scenery. Desaturated industrial "
    "palette (gunmetal, charcoal, worn brown) with a single character accent "
    "color. 4:5 portrait composition. "
)
NEGATIVE = (
    " Do not include any text, letters, captions, watermark, logo, border, or "
    "UI frame. Single subject only, anatomically correct, one head, two eyes."
)


def repo_root() -> Path:
    # tools/cinematics/portraits.py -> repo root is two parents up.
    return Path(__file__).resolve().parents[2]


def load_bible() -> dict:
    path = repo_root() / "assets" / "data" / "characters.json"
    data = json.loads(path.read_text())
    return data["characters"]


def _character(bible: dict, char_id: str) -> dict:
    if char_id not in bible:
        raise SystemExit(
            f"Unknown character '{char_id}'. Known: {', '.join(sorted(bible))}"
        )
    return bible[char_id]


def _portraits_dir(char_id: str) -> Path:
    return repo_root() / "assets" / "cinematics" / "portraits" / char_id


def build_ref_prompt(char: dict) -> str:
    return (
        f"{STYLE_PREFIX}Subject: {char['display_name']}. "
        f"Appearance: {char['appearance']} "
        f"Wardrobe: {char['wardrobe']} "
        f"Neutral, purposeful expression (canonical reference headshot)."
        f"{NEGATIVE}"
        # placeholder hints (stripped by real backends' prompts naturally):
        f" ||NAME||{char['display_name']}|| ||LINE||reference||"
    )


def build_line_prompt(char: dict, text: str, emotion: str,
                      prompt_extra: str = "") -> str:
    extra = f" Additional direction: {prompt_extra.strip()}." \
        if prompt_extra and prompt_extra.strip() else ""
    return (
        f"{STYLE_PREFIX}Subject: {char['display_name']} (keep the SAME face, "
        f"hair, and identity as the provided reference image). "
        f"Appearance: {char['appearance']} Wardrobe: {char['wardrobe']} "
        f"Expression/direction: {emotion}. "
        f"This is the moment they say: \"{text}\"."
        f"{extra}"
        f"{NEGATIVE}"
        f" ||NAME||{char['display_name']}|| ||LINE||{text or emotion}||"
    )


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------
def cmd_gen_ref(args) -> int:
    bible = load_bible()
    char = _character(bible, args.character)
    backend = get_backend(args.backend)
    out = _portraits_dir(args.character) / "_ref.png"
    out.parent.mkdir(parents=True, exist_ok=True)

    prompt = build_ref_prompt(char)
    print(f"[gen-ref] {args.character} via {backend.name}")
    raw = backend.txt2img(prompt, timeout=args.timeout)
    png = finalize_portrait(raw)
    out.write_bytes(png)
    print(f"[gen-ref] wrote {out}  ({PORTRAIT_SIZE[0]}x{PORTRAIT_SIZE[1]} RGBA)")
    print("[gen-ref] HUMAN APPROVAL STEP: eyeball this _ref.png before running "
          "gen-line — everything downstream conditions on it.")
    return 0


def cmd_gen_line(args) -> int:
    bible = load_bible()
    char = _character(bible, args.character)
    backend = get_backend(args.backend)
    cache = PortraitCache(Path(__file__).resolve().parent)

    # Studio seed_image support: condition on an arbitrary reference instead
    # of the character's canonical _ref.png (docs/cinematic_studio.md §5).
    ref_override = getattr(args, "ref", None)
    if ref_override:
        ref = Path(ref_override)
        if not ref.is_file():
            raise SystemExit(f"--ref image not found: {ref}")
    else:
        ref = _portraits_dir(args.character) / "_ref.png"
        if not ref.is_file():
            raise SystemExit(
                f"No reference for '{args.character}'. Run: gen-ref {args.character}"
            )
    ref_bytes = ref.read_bytes()

    out = _portraits_dir(args.character) / f"{args.scene}_{args.seq}.png"
    out.parent.mkdir(parents=True, exist_ok=True)

    # prompt_extra rides inside the prompt, so it lands in the cache key too —
    # a changed direction regenerates, an unchanged one stays a free retake.
    prompt = build_line_prompt(char, args.text, args.emotion,
                               prompt_extra=getattr(args, "prompt_extra", "") or "")
    key = cache.key(prompt=prompt, backend=backend.name, ref_bytes=ref_bytes,
                    size=PORTRAIT_SIZE)

    # ---- cache hit: free retake ----------------------------------------
    cached = None if args.no_cache else cache.get(key)
    if cached is not None:
        out.write_bytes(cached)
        print(f"[gen-line] CACHE HIT ({key[:12]}) -> {out}  (no API spend)")
        return 0

    # ---- budget guard ---------------------------------------------------
    if args.max_images is not None and args.max_images <= 0:
        raise SystemExit("[gen-line] budget guard: --max-images is 0, refusing.")

    qc_fn = qc_mod.check  # external agents pass their own via the library API
    attempts = args.retries + 1
    last_reason = ""
    for i in range(attempts):
        print(f"[gen-line] {args.character} {args.scene}_{args.seq} "
              f"via {backend.name} (attempt {i+1}/{attempts})")
        raw = backend.img2img(prompt, ref, timeout=args.timeout)
        png = finalize_portrait(raw)
        out.write_bytes(png)
        result = qc_fn(out, ref)
        print(f"[gen-line] qc: pass={result['pass']} :: {result['reason']}")
        if result["pass"]:
            cache.put(key, png, meta={
                "character": args.character,
                "scene": args.scene, "seq": args.seq,
                "backend": backend.name, "path": str(out),
            })
            print(f"[gen-line] wrote {out}  (cached {key[:12]})")
            return 0
        last_reason = str(result["reason"])

    # ---- all retries failed: fall back to the neutral reference --------
    print(f"[gen-line] QC failed after {attempts} attempts ({last_reason}); "
          f"falling back to _ref.png as neutral portrait.")
    shutil.copyfile(ref, out)
    print(f"[gen-line] wrote {out}  (fallback = _ref.png)")
    return 0


def cmd_qc(args) -> int:
    result = qc_mod.check(args.portrait, args.ref)
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


def cmd_list(args) -> int:
    bible = load_bible()
    for cid, c in bible.items():
        ref = _portraits_dir(cid) / "_ref.png"
        mark = "OK" if ref.is_file() else "-- "
        print(f"  [{mark}] {cid:<14} {c['display_name']}")
    return 0


# ---------------------------------------------------------------------------
# Library entry points (for the cinematic-director agent to import directly)
# ---------------------------------------------------------------------------
def gen_ref(character: str, backend: Optional[str] = None,
            timeout: float = 180.0) -> Path:
    """Programmatic gen-ref for the authoring SDK / director agent.

    Generates (or regenerates) a character's canonical ``_ref.png`` — the
    identity anchor every line portrait conditions on. Returns the ref path.
    In placeholder mode this is a zero-spend labelled card, so the whole
    authoring flow works offline.
    """
    ns = argparse.Namespace(character=character, backend=backend, timeout=timeout)
    cmd_gen_ref(ns)
    return _portraits_dir(character) / "_ref.png"


def ref_path(character: str) -> Path:
    """Where a character's canonical reference headshot lives."""
    return _portraits_dir(character) / "_ref.png"


def gen_line(character: str, scene: str, seq: str, text: str, emotion: str,
             backend: Optional[str] = None, retries: int = 2,
             qc_fn=None, no_cache: bool = False,
             ref_override: Optional[Path] = None,
             prompt_extra: str = "") -> Path:
    """Programmatic gen-line for the cinematic-director agent / Studio bridge.

    ``qc_fn`` is any callable ``(portrait_path, ref_path) -> {pass, reason}``;
    pass the agent's own vision-judge here to replace the built-in aHash check.
    ``ref_override`` conditions the generation on THAT image instead of the
    character's ``_ref.png`` (the Studio's per-line ``seed_image``);
    ``prompt_extra`` is appended to the prompt after the emotion/direction.
    Both flow into the content-hash cache key, so changed seeds/directions
    regenerate while unchanged ones stay free retakes.
    Returns the output path (a real generation, a cache hit, or the reference
    fallback).
    """
    ns = argparse.Namespace(
        character=character, scene=scene, seq=seq, text=text, emotion=emotion,
        backend=backend, retries=retries, no_cache=no_cache,
        max_images=None, timeout=180.0,
        ref=str(ref_override) if ref_override else None,
        prompt_extra=prompt_extra,
    )
    if qc_fn is not None:
        qc_mod.check = qc_fn  # swap in the agent's vision judge for this run
    try:
        cmd_gen_line(ns)
    finally:
        if qc_fn is not None:
            qc_mod.check = _builtin_qc_check  # restore default
    return _portraits_dir(character) / f"{scene}_{seq}.png"


# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="tools.cinematics.portraits",
        description="Cinematic portrait generation pipeline (Phase 3).",
    )
    p.add_argument("--backend",
                   choices=["codex", "openai", "gemini", "placeholder"],
                   default=None, help="Force a backend. Default: auto — codex "
                   "(ChatGPT OAuth, no metered spend) if logged in, else a "
                   "keyed real backend, else placeholder. 'codex' supports "
                   "both txt2img and reference-conditioned img2img.")
    p.add_argument("--timeout", type=float, default=180.0)
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("gen-ref", help="Generate a character's canonical _ref.png")
    r.add_argument("character")
    r.set_defaults(func=cmd_gen_ref)

    g = sub.add_parser("gen-line", help="Generate a per-line reference-conditioned portrait")
    g.add_argument("character")
    g.add_argument("--scene", required=True, help="scene id, e.g. 'demo'")
    g.add_argument("--seq", required=True, help="sequence id, e.g. '01'")
    g.add_argument("--text", default="", help="the spoken line")
    g.add_argument("--emotion", default="neutral", help="expression/direction")
    g.add_argument("--ref", default=None, metavar="IMAGE",
                   help="condition on THIS image instead of the character's "
                        "_ref.png (the Studio's seed_image)")
    g.add_argument("--prompt-extra", default="", dest="prompt_extra",
                   help="extra direction appended to the generation prompt "
                        "(after emotion); part of the cache key")
    g.add_argument("--retries", type=int, default=2, help="QC retry budget")
    g.add_argument("--max-images", type=int, default=None,
                   help="per-run cost/budget guard (max API images)")
    g.add_argument("--no-cache", action="store_true")
    g.set_defaults(func=cmd_gen_line)

    q = sub.add_parser("qc", help="Check a portrait against a character's _ref.png")
    q.add_argument("portrait")
    q.add_argument("ref", help="character id OR path to a _ref.png")
    q.set_defaults(func=cmd_qc)

    ls = sub.add_parser("list", help="List bible characters + which have a _ref.png")
    ls.set_defaults(func=cmd_list)
    return p


# Remember the built-in so a programmatic qc_fn override can be restored.
_builtin_qc_check = qc_mod.check


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    # qc: allow a character id in place of a ref path for convenience.
    if args.cmd == "qc":
        ref = Path(args.ref)
        if not ref.suffix:  # looks like a character id
            ref = _portraits_dir(args.ref) / "_ref.png"
        args.ref = str(ref)

    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())

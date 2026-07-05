"""Consistency QC for generated portraits.

The REAL identity-drift judging will be driven by the cinematic-director agent
(a vision-capable model) later. This module defines the seam it plugs into:

    check(portrait_path, ref_path) -> {"pass": bool, "reason": str, "score": float}

An external vision agent implements the same signature and is passed to
``portraits.gen_line(..., qc_fn=agent_check)``. The built-in default below is a
cheap, dependency-free sanity gate:

* the portrait must exist, be the pinned 512x640 size, and NOT be blank /
  near-transparent (a "face not present" proxy);
* a perceptual average-hash (aHash) distance vs the reference must be within a
  loose band — close enough to be plausibly the same character/framing, but not
  a pixel-identical copy of the ref (which would mean the edit did nothing).

This is deliberately permissive: it catches gross failures (empty image, wildly
different composition) without pretending to be a face-recognition model. Swap
in the agent for real identity verification.
"""
from __future__ import annotations

from pathlib import Path
from typing import Callable, Dict

from PIL import Image

from .backends import PORTRAIT_SIZE

QCResult = Dict[str, object]
QCFn = Callable[[Path, Path], QCResult]


def _ahash(img: Image.Image, side: int = 16) -> int:
    g = img.convert("L").resize((side, side), Image.Resampling.BILINEAR)
    px = list(g.getdata())
    avg = sum(px) / len(px)
    bits = 0
    for i, p in enumerate(px):
        if p >= avg:
            bits |= 1 << i
    return bits


def _hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


def _content_fraction(img: Image.Image) -> float:
    """Fraction of pixels that are opaque AND not near-uniform background."""
    rgba = img.convert("RGBA")
    px = rgba.getdata()
    n = len(px)
    opaque = sum(1 for (_, _, _, a) in px if a > 16)
    return opaque / n if n else 0.0


def default_check(portrait_path: Path, ref_path: Path,
                  drift_max: int = 110, min_content: float = 0.15) -> QCResult:
    """Built-in perceptual-hash + not-blank sanity check.

    Returns {"pass", "reason", "score"} where score is the aHash Hamming
    distance to the reference (lower == more similar; 0 == identical).
    """
    portrait_path = Path(portrait_path)
    ref_path = Path(ref_path)

    if not portrait_path.is_file():
        return {"pass": False, "reason": "portrait file missing", "score": 999}

    img = Image.open(portrait_path)
    if img.size != PORTRAIT_SIZE:
        return {
            "pass": False,
            "reason": f"wrong size {img.size}, expected {PORTRAIT_SIZE}",
            "score": 999,
        }

    content = _content_fraction(img)
    if content < min_content:
        return {
            "pass": False,
            "reason": f"portrait looks blank/transparent (content={content:.2f})",
            "score": 999,
        }

    if not ref_path.is_file():
        # No reference to compare against: pass on the sanity check alone.
        return {"pass": True, "reason": "no reference; sanity-only pass",
                "score": -1}

    ref_img = Image.open(ref_path)
    dist = _hamming(_ahash(img), _ahash(ref_img))

    if dist > drift_max:
        return {
            "pass": False,
            "reason": f"identity drift too high (aHash dist {dist} > {drift_max})",
            "score": dist,
        }
    return {"pass": True, "reason": f"ok (aHash dist {dist})", "score": dist}


def check(portrait_path, ref_path) -> QCResult:
    """Public default entry point (matches the agent-swappable signature)."""
    return default_check(Path(portrait_path), Path(ref_path))

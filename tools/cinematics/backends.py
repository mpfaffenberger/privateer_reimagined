"""Image-generation backend abstraction for the cinematic portrait pipeline.

The C++ engine never touches this code — it only loads pre-generated PNGs by
path. All AI/network lives here, behind a small swappable interface so we can
drop in local SD/ComfyUI later without touching the CLI.

Backends
--------
* ``OpenAIBackend``   — gpt-image-2 (/v1/images/generations + /v1/images/edits),
  the SAME model + endpoints the ship-sprite / concourse generators use
  (tools/pixelart/generate_sprite.py, tools/gen_concourse_ai.py).
* ``GeminiBackend``   — Google Gemini image generation (REST, no SDK required)
* ``PlaceholderBackend`` — always available; draws an obvious solid-color
  512x640 card with the character name + line text via Pillow. This is the
  spend-free fallback used whenever no API key is present, so the end-to-end
  cinematic flow can be demoed offline.

Contract (``ImageBackend``)
---------------------------
* ``available() -> bool`` — is this backend usable right now (key present)?
* ``txt2img(prompt) -> bytes`` — fresh text-to-image (used by ``gen-ref``).
* ``img2img(prompt, ref_path) -> bytes`` — reference-conditioned edit (used by
  ``gen-line``; the reference holds identity far better than fresh t2i).

Every returned blob is normalized to exactly 512x640 RGBA PNG bytes by
``finalize_portrait`` before it is written to disk.
"""
from __future__ import annotations

import base64
import hashlib
import io
import mimetypes
import os
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Optional

from PIL import Image, ImageDraw, ImageFont

# --- PINNED by Phase 1: portraits are 512x640 RGBA, 4:5 portrait. ------------
PORTRAIT_W = 512
PORTRAIT_H = 640
PORTRAIT_SIZE = (PORTRAIT_W, PORTRAIT_H)

# Native generation size closest to 4:5 for the OpenAI image API (portrait).
_OPENAI_GEN_SIZE = "1024x1536"

OPENAI_GEN_URL = "https://api.openai.com/v1/images/generations"
OPENAI_EDIT_URL = "https://api.openai.com/v1/images/edits"
# Gemini image-capable model (REST generateContent).
GEMINI_MODEL = "gemini-2.5-flash-image-preview"


# ---------------------------------------------------------------------------
# Post-processing: force any image to exactly 512x640 RGBA (cover-crop).
# ---------------------------------------------------------------------------
def finalize_portrait(png_bytes: bytes) -> bytes:
    """Normalize arbitrary image bytes to 512x640 RGBA PNG (center cover-crop)."""
    img = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    src_w, src_h = img.size
    target_ratio = PORTRAIT_W / PORTRAIT_H
    src_ratio = src_w / src_h
    if src_ratio > target_ratio:
        # too wide -> crop width
        new_w = round(src_h * target_ratio)
        left = (src_w - new_w) // 2
        img = img.crop((left, 0, left + new_w, src_h))
    elif src_ratio < target_ratio:
        # too tall -> crop height (bias toward the top so faces survive)
        new_h = round(src_w / target_ratio)
        top = (src_h - new_h) // 4  # keep upper portion (chest-up framing)
        img = img.crop((0, top, src_w, top + new_h))
    img = img.resize(PORTRAIT_SIZE, Image.Resampling.LANCZOS)
    out = io.BytesIO()
    img.save(out, "PNG")
    return out.getvalue()


# ---------------------------------------------------------------------------
# Interface
# ---------------------------------------------------------------------------
class ImageBackend(ABC):
    name: str = "abstract"

    @abstractmethod
    def available(self) -> bool: ...

    @abstractmethod
    def txt2img(self, prompt: str, timeout: float = 180.0) -> bytes:
        """Return PNG bytes for a fresh text-to-image generation."""

    @abstractmethod
    def img2img(self, prompt: str, ref_path: Path, timeout: float = 180.0) -> bytes:
        """Return PNG bytes for a reference-conditioned edit of ``ref_path``."""


# ---------------------------------------------------------------------------
# OpenAI gpt-image-1
# ---------------------------------------------------------------------------
class OpenAIBackend(ImageBackend):
    name = "openai"

    # gpt-image-2 supports low|medium|high|auto. Portraits are hero art, so we
    # default to high; override with OPENAI_IMAGE_QUALITY.
    def __init__(self, api_key: Optional[str] = None, quality: Optional[str] = None):
        self.api_key = api_key or os.environ.get("OPENAI_API_KEY")
        self.quality = quality or os.environ.get("OPENAI_IMAGE_QUALITY", "high")

    def available(self) -> bool:
        return bool(self.api_key)

    def _client(self, timeout: float):
        import httpx

        return httpx.Client(timeout=timeout)

    def txt2img(self, prompt: str, timeout: float = 180.0) -> bytes:
        payload = {
            "model": "gpt-image-2",
            "prompt": prompt,
            "size": _OPENAI_GEN_SIZE,
            "quality": self.quality,
            "n": 1,
        }
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        with self._client(timeout) as client:
            resp = client.post(OPENAI_GEN_URL, json=payload, headers=headers)
            if resp.status_code != 200:
                raise RuntimeError(
                    f"OpenAI image API error {resp.status_code}: {resp.text[:500]}"
                )
            data = resp.json()
        return base64.b64decode(data["data"][0]["b64_json"])

    def img2img(self, prompt: str, ref_path: Path, timeout: float = 180.0) -> bytes:
        # Reference-conditioned edit via /v1/images/edits with gpt-image-2 —
        # identical pathway to the ship-sprite generator's --reference mode.
        mime, _ = mimetypes.guess_type(str(ref_path))
        mime = mime or "image/png"
        data = {
            "model": "gpt-image-2",
            "prompt": prompt,
            "n": "1",
            "size": _OPENAI_GEN_SIZE,
            "quality": self.quality,
        }
        headers = {"Authorization": f"Bearer {self.api_key}"}
        with open(ref_path, "rb") as fh:
            files = [("image", (ref_path.name, fh, mime))]
            with self._client(timeout) as client:
                resp = client.post(
                    OPENAI_EDIT_URL, data=data, files=files, headers=headers
                )
                if resp.status_code != 200:
                    raise RuntimeError(
                        f"OpenAI image edit error {resp.status_code}: {resp.text[:500]}"
                    )
                payload = resp.json()
        return base64.b64decode(payload["data"][0]["b64_json"])


# ---------------------------------------------------------------------------
# Google Gemini image generation
# ---------------------------------------------------------------------------
class GeminiBackend(ImageBackend):
    name = "gemini"

    def __init__(self, api_key: Optional[str] = None):
        self.api_key = api_key or os.environ.get("GEMINI_API_KEY")

    def available(self) -> bool:
        return bool(self.api_key)

    def _endpoint(self) -> str:
        return (
            f"https://generativelanguage.googleapis.com/v1beta/models/"
            f"{GEMINI_MODEL}:generateContent"
        )

    def _generate(self, parts: list, timeout: float) -> bytes:
        import httpx

        body = {"contents": [{"parts": parts}]}
        headers = {
            "x-goog-api-key": self.api_key,
            "Content-Type": "application/json",
        }
        with httpx.Client(timeout=timeout) as client:
            resp = client.post(self._endpoint(), json=body, headers=headers)
            if resp.status_code != 200:
                raise RuntimeError(
                    f"Gemini API error {resp.status_code}: {resp.text[:500]}"
                )
            data = resp.json()
        # Walk candidate parts for the first inline image blob.
        for cand in data.get("candidates", []):
            for part in cand.get("content", {}).get("parts", []):
                inline = part.get("inlineData") or part.get("inline_data")
                if inline and inline.get("data"):
                    return base64.b64decode(inline["data"])
        raise RuntimeError(f"Gemini returned no image data: {str(data)[:500]}")

    def txt2img(self, prompt: str, timeout: float = 180.0) -> bytes:
        return self._generate([{"text": prompt}], timeout)

    def img2img(self, prompt: str, ref_path: Path, timeout: float = 180.0) -> bytes:
        mime, _ = mimetypes.guess_type(str(ref_path))
        mime = mime or "image/png"
        ref_b64 = base64.b64encode(ref_path.read_bytes()).decode("ascii")
        parts = [
            {"text": prompt},
            {"inlineData": {"mimeType": mime, "data": ref_b64}},
        ]
        return self._generate(parts, timeout)


# ---------------------------------------------------------------------------
# Placeholder (offline, zero-spend) backend
# ---------------------------------------------------------------------------
class PlaceholderBackend(ImageBackend):
    """Draws an obvious labelled card so the pipeline runs with no API key.

    Deterministic per (character-ish) prompt so reference + lines share a hue
    family — makes the fallback read as 'the same character' at a glance.
    """

    name = "placeholder"

    def __init__(self, label: str = "PLACEHOLDER"):
        self.label = label

    def available(self) -> bool:
        return True

    # -- helpers -----------------------------------------------------------
    @staticmethod
    def _font(size: int):
        for candidate in (
            "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
            "/System/Library/Fonts/Helvetica.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        ):
            try:
                return ImageFont.truetype(candidate, size)
            except Exception:
                continue
        return ImageFont.load_default()

    @staticmethod
    def _hue_from(seed: str) -> tuple:
        h = hashlib.sha256(seed.encode("utf-8")).digest()
        # muted, dark-ish base so it reads like the moody style bible
        r = 40 + h[0] % 90
        g = 40 + h[1] % 90
        b = 40 + h[2] % 90
        return (r, g, b, 255)

    @staticmethod
    def _wrap(draw, text, font, max_w):
        words = text.split()
        lines, cur = [], ""
        for w in words:
            trial = (cur + " " + w).strip()
            if draw.textlength(trial, font=font) <= max_w:
                cur = trial
            else:
                if cur:
                    lines.append(cur)
                cur = w
        if cur:
            lines.append(cur)
        return lines

    def _card(self, seed: str, title: str, body: str) -> bytes:
        base = self._hue_from(seed)
        img = Image.new("RGBA", PORTRAIT_SIZE, base)
        draw = ImageDraw.Draw(img)
        # vignette-ish inner border
        draw.rectangle([8, 8, PORTRAIT_W - 9, PORTRAIT_H - 9], outline=(220, 220, 230, 90), width=2)
        # a crude "head" silhouette so it reads as a portrait slot
        cx = PORTRAIT_W // 2
        head = (min(255, base[0] + 40), min(255, base[1] + 40), min(255, base[2] + 45), 255)
        draw.ellipse([cx - 90, 120, cx + 90, 310], fill=head)
        draw.rounded_rectangle([cx - 130, 300, cx + 130, 470], radius=40, fill=head)
        # title (character name)
        tf = self._font(38)
        tw = draw.textlength(title, font=tf)
        draw.text((cx - tw / 2, 40), title, font=tf, fill=(245, 245, 250, 255))
        # body (line/emotion text), wrapped
        bf = self._font(24)
        y = 500
        for ln in self._wrap(draw, body, bf, PORTRAIT_W - 60)[:5]:
            lw = draw.textlength(ln, font=bf)
            draw.text((cx - lw / 2, y), ln, font=bf, fill=(230, 230, 235, 255))
            y += 30
        # tag
        sf = self._font(18)
        tag = f"[{self.label}]"
        draw.text((16, PORTRAIT_H - 34), tag, font=sf, fill=(255, 210, 120, 255))
        out = io.BytesIO()
        img.save(out, "PNG")
        return out.getvalue()

    def txt2img(self, prompt: str, timeout: float = 180.0) -> bytes:
        title, body = _split_label(prompt)
        return self._card(seed=title, title=title, body=body or "reference")

    def img2img(self, prompt: str, ref_path: Path, timeout: float = 180.0) -> bytes:
        title, body = _split_label(prompt)
        # seed on the character folder so lines share the ref's hue family
        seed = ref_path.parent.name or title
        return self._card(seed=seed, title=title, body=body or "line")


def _split_label(prompt: str) -> tuple:
    """Extract a short (title, body) for the placeholder card from the prompt.

    The CLI injects '||NAME||' and '||LINE||' hint markers so the card is
    readable; if absent we fall back to a truncated prompt.
    """
    name = "CHARACTER"
    line = ""
    if "||NAME||" in prompt:
        try:
            name = prompt.split("||NAME||", 1)[1].split("||", 1)[0].strip() or name
        except Exception:
            pass
    if "||LINE||" in prompt:
        try:
            line = prompt.split("||LINE||", 1)[1].split("||", 1)[0].strip()
        except Exception:
            pass
    return name, line


# ---------------------------------------------------------------------------
# Factory
# ---------------------------------------------------------------------------
_REAL_BACKENDS = {"openai": OpenAIBackend, "gemini": GeminiBackend}


def get_backend(name: Optional[str] = None, allow_placeholder: bool = True) -> ImageBackend:
    """Pick a backend.

    * ``name`` explicit ("openai" / "gemini" / "placeholder") — honored, but a
      real backend with no key gracefully degrades to placeholder (unless
      ``allow_placeholder=False``, which raises instead).
    * ``name`` None — auto: OpenAI if keyed, else Gemini if keyed, else
      placeholder.
    """
    if name == "placeholder":
        return PlaceholderBackend()

    if name in _REAL_BACKENDS:
        backend = _REAL_BACKENDS[name]()
        if backend.available():
            return backend
        if not allow_placeholder:
            raise RuntimeError(
                f"Backend '{name}' selected but its API key env var is not set."
            )
        return PlaceholderBackend(label=f"NO {name.upper()} KEY")

    # auto
    for cls in (OpenAIBackend, GeminiBackend):
        b = cls()
        if b.available():
            return b
    return PlaceholderBackend(label="OFFLINE / NO KEY")

"""Cinematic preview / iterate client (Phase 4.1).

A thin ``dev_remote`` client (base ``http://127.0.0.1:47001``) that drives the
author/iterate loop against the *running game*:

1. ``reload`` the cinematic (picks up JSON edits live) and ``play`` it;
2. poll ``GET /events?since=N``, keeping only ``category == "cinematic"`` beats,
   and parse them into a structured timeline (started / cue / line / music /
   sfx / seeked / ended) so the caller can assert
   ``line_shown grayson at t=3.0`` without eyeballing pixels;
3. at a set of key timestamps (passed in, or auto-derived from ``line`` / ``sfx``
   cue times) ``POST /cinematic/seek`` then ``POST /screenshot``, collect the
   PNGs, and assemble a **contact sheet** (a labelled grid) via Pillow.

If the game isn't running it fails fast with a clear message instead of hanging
(short connect timeout, no retries).

    python -m tools.cinematics.preview intro_troy
    python -m tools.cinematics.preview intro_troy --at 1,3,8
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional

import httpx
from PIL import Image, ImageDraw, ImageFont

BASE_URL = "http://127.0.0.1:47001"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def cinematics_dir() -> Path:
    return repo_root() / "assets" / "cinematics"


def previews_dir() -> Path:
    d = Path(__file__).resolve().parent / "previews"
    d.mkdir(parents=True, exist_ok=True)
    return d


class GameNotRunning(RuntimeError):
    """Raised when the dev_remote endpoint can't be reached."""


# ---------------------------------------------------------------------------
# dev_remote HTTP client
# ---------------------------------------------------------------------------
class DevRemote:
    """Typed wrapper over the Phase 2 cinematic control-plane endpoints."""

    def __init__(self, base_url: str = BASE_URL, timeout: float = 4.0):
        self.base_url = base_url.rstrip("/")
        # Short connect timeout => quick, clear failure when the game is down.
        self._client = httpx.Client(
            base_url=self.base_url,
            timeout=httpx.Timeout(timeout, connect=2.0),
        )

    def close(self) -> None:
        self._client.close()

    def __enter__(self) -> "DevRemote":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _get(self, path: str) -> dict:
        try:
            r = self._client.get(path)
        except httpx.ConnectError as e:
            raise GameNotRunning(
                f"Cannot reach dev_remote at {self.base_url} \u2014 is the game "
                f"running? ({e})") from e
        except httpx.TimeoutException as e:
            raise GameNotRunning(
                f"dev_remote at {self.base_url} timed out ({e})") from e
        r.raise_for_status()
        return r.json()

    def _post(self, path: str, body: Optional[dict] = None) -> dict:
        try:
            r = self._client.post(path, json=body or {})
        except httpx.ConnectError as e:
            raise GameNotRunning(
                f"Cannot reach dev_remote at {self.base_url} \u2014 is the game "
                f"running? ({e})") from e
        except httpx.TimeoutException as e:
            raise GameNotRunning(
                f"dev_remote at {self.base_url} timed out ({e})") from e
        r.raise_for_status()
        return r.json()

    # -- endpoints ------------------------------------------------------------
    def status(self) -> dict:
        return self._get("/cinematic/status")

    def events(self, since: int = 0) -> dict:
        return self._get(f"/events?since={since}")

    def play(self, cid: str) -> dict:
        return self._post("/cinematic/play", {"id": cid})

    def reload(self, cid: str = "") -> dict:
        return self._post("/cinematic/reload", {"id": cid} if cid else {})

    def seek(self, t: float) -> dict:
        return self._post("/cinematic/seek", {"t": float(t)})

    def stop(self) -> dict:
        return self._post("/cinematic/stop")

    def screenshot(self) -> dict:
        return self._post("/screenshot")


# ---------------------------------------------------------------------------
# Cinematic-event parsing
# ---------------------------------------------------------------------------
def parse_cinematic_event(text: str) -> dict:
    """Parse a ``"cinematic"`` event body into a structured dict.

    Emitted forms (see src/cinematic.cpp):
        started id=<id> dur=<f>      cue cmd=<name> t=<f>
        music file=<f> t=<f>         sfx file=<f> t=<f>
        line speaker=<s> portrait=<p> side=<s> t=<f>
        seeked t=<f>                 reloaded id=<id>
        ended reason=<r> t=<f>
    Values contain no spaces, so a simple whitespace split is exact.
    """
    parts = text.split()
    if not parts:
        return {"kind": "", "raw": text}
    out: dict = {"kind": parts[0], "raw": text}
    for tok in parts[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = float(v) if k in ("t", "dur") else v
            except ValueError:
                out[k] = v
    return out


def collect_timeline(events: List[dict]) -> List[dict]:
    """Filter/parse the ``category == "cinematic"`` beats from an events list."""
    return [
        {"seq": e["seq"], **parse_cinematic_event(e["text"])}
        for e in events if e.get("category") == "cinematic"
    ]


# ---------------------------------------------------------------------------
# Key-timestamp derivation
# ---------------------------------------------------------------------------
def load_cinematic(cid: str) -> dict:
    path = cinematics_dir() / f"{Path(cid).stem}.json"
    return json.loads(path.read_text())


# The engine's portrait panel slides in over this many seconds (must match
# k_slide_s in src/cinematic.cpp). At exactly a line's start t the panel is
# fully OFF-screen with alpha 0, so we must sample INSIDE its window.
_SLIDE_S = 0.35


def derive_key_times(data: dict) -> List[float]:
    """Auto key timestamps: a fully-visible sample per ``line`` / ``sfx`` cue.

    IMPORTANT (np-cinematic bug #5): we do NOT sample at a ``line`` cue's
    START time. The comm-VDU portrait panel slides in over ``_SLIDE_S`` and is
    fully off-screen (alpha 0) at exactly ``t`` \u2014 seeking there captured an
    empty frame, which is why the old contact sheet never showed dialogue
    beats. Instead we sample each line at its MID-POINT (``t + dur/2``), where
    the panel is fully slid in and the subtitle is up. ``sfx`` cues are
    instantaneous, so they keep their start time (nudged just past the slide
    threshold). The engine's /cinematic/seek already reconstructs the overlay
    at any timestamp (draw_overlay is a pure function of the timeline clock),
    so seeking to a mid-line time renders exactly the frame a viewer sees.

    Falls back to a few evenly-spaced samples if there's no dialogue or sfx.
    """
    times: set = set()
    for c in data.get("timeline", []):
        t = c.get("t")
        if not isinstance(t, (int, float)):
            continue
        if c.get("cmd") == "line":
            dur = c.get("dur")
            dur = float(dur) if isinstance(dur, (int, float)) and dur > 0 else 2.0
            # Mid-window, but never before the slide-in completes.
            times.add(round(float(t) + max(min(dur / 2.0, dur - 0.05),
                                           _SLIDE_S + 0.05), 3))
        elif c.get("cmd") == "sfx":
            times.add(round(float(t) + _SLIDE_S + 0.05, 3))
    if times:
        return sorted(times)
    all_t = [float(c["t"]) for c in data.get("timeline", [])
             if isinstance(c.get("t"), (int, float))]
    if not all_t:
        return [0.0]
    lo, hi = min(all_t), max(all_t)
    span = hi - lo or 1.0
    return [round(lo + span * f, 2) for f in (0.0, 0.25, 0.5, 0.75, 1.0)]


# ---------------------------------------------------------------------------
# Contact sheet
# ---------------------------------------------------------------------------
def _font(size: int = 16):
    try:
        return ImageFont.truetype("DejaVuSans.ttf", size)
    except OSError:
        return ImageFont.load_default()


def build_contact_sheet(shots: List[dict], out_path: Path,
                        cell_w: int = 480, cols: int = 3,
                        title: str = "") -> Path:
    """Assemble a labelled grid of screenshots.

    ``shots`` is a list of ``{"t": float, "label": str, "image": Path}``.
    """
    label_h = 26
    thumbs = []
    for s in shots:
        img = Image.open(s["image"]).convert("RGB")
        ratio = cell_w / img.width
        cell_h = max(1, int(img.height * ratio))
        thumbs.append((s, img.resize((cell_w, cell_h), Image.Resampling.LANCZOS)))

    cell_h = max((t.height for _, t in thumbs), default=270)
    rows = (len(thumbs) + cols - 1) // cols
    title_h = 34 if title else 0
    sheet = Image.new("RGB",
                      (cols * cell_w, title_h + rows * (cell_h + label_h)),
                      (18, 20, 24))
    draw = ImageDraw.Draw(sheet)
    if title:
        draw.text((10, 8), title, fill=(120, 220, 255), font=_font(20))

    for idx, (s, thumb) in enumerate(thumbs):
        r, c = divmod(idx, cols)
        x = c * cell_w
        y = title_h + r * (cell_h + label_h)
        sheet.paste(thumb, (x, y))
        draw.rectangle([x, y, x + cell_w - 1, y + thumb.height - 1],
                       outline=(51, 204, 255), width=2)
        label = s.get("label", f"t={s['t']}")
        ly = y + thumb.height
        draw.rectangle([x, ly, x + cell_w - 1, ly + label_h - 1], fill=(10, 12, 15))
        draw.text((x + 6, ly + 5), label, fill=(230, 230, 230), font=_font(14))

    sheet.save(out_path)
    return out_path


# ---------------------------------------------------------------------------
# The iterate loop
# ---------------------------------------------------------------------------
def preview(cid: str, at: Optional[List[float]] = None,
            base_url: str = BASE_URL, settle: float = 0.4,
            shot_settle: float = 0.12) -> dict:
    """Run one reload->play->sample->contact-sheet iteration.

    Returns ``{"events": [...parsed cinematic timeline...],
               "shots": [...], "contact_sheet": <path or None>,
               "status": <final status dict>}``.
    Raises :class:`GameNotRunning` (fast) if the game isn't up.
    """
    data = load_cinematic(cid)
    key_times = at if at else derive_key_times(data)

    with DevRemote(base_url) as dev:
        # Fail fast + clearly if the game is down (short connect timeout).
        dev.status()

        since = dev.events().get("latest", 0)
        dev.reload(cid)
        dev.play(cid)
        time.sleep(settle)

        shots: List[dict] = []
        shot_dir = previews_dir() / cid
        shot_dir.mkdir(parents=True, exist_ok=True)

        for t in key_times:
            # The cinematic keeps ticking after a seek, so shoot promptly
            # (a few frames to let the render + /screenshot pipeline flush).
            # A tiny drift is harmless: line samples are taken mid-window
            # where the portrait panel is fully slid in.
            dev.seek(t)
            time.sleep(shot_settle)
            resp = dev.screenshot()
            label = f"t={t:g}s"
            if resp.get("ok") and resp.get("path"):
                src = Path(resp["path"])
                dst = shot_dir / f"shot_{t:g}.png"
                try:
                    shutil.copyfile(src, dst)
                    shots.append({"t": t, "label": label, "image": dst})
                except OSError as e:
                    print(f"[preview] screenshot copy failed for t={t}: {e}")
            else:
                print(f"[preview] screenshot not captured at t={t}")

        # Drain the cinematic beats produced this run.
        ev = dev.events(since=since)
        timeline = collect_timeline(ev.get("events", []))
        final_status = dev.status()

    # Annotate each shot with the cinematic beat nearest its timestamp.
    for s in shots:
        beat = _nearest_beat(timeline, s["t"])
        if beat:
            extra = beat.get("speaker") or beat.get("cmd") or beat["kind"]
            s["label"] = f"{s['label']}  {beat['kind']}:{extra}"

    sheet_path = None
    if shots:
        sheet_path = build_contact_sheet(
            shots, previews_dir() / f"{cid}_contact.png",
            title=f"cinematic: {cid}")

    return {"events": timeline, "shots": shots,
            "contact_sheet": sheet_path, "status": final_status}


def _nearest_beat(timeline: List[dict], t: float) -> Optional[dict]:
    beats = [b for b in timeline if isinstance(b.get("t"), (int, float))]
    if not beats:
        return None
    return min(beats, key=lambda b: abs(b["t"] - t))


def _print_timeline(timeline: List[dict]) -> None:
    print("\n  Cinematic event timeline:")
    if not timeline:
        print("    (no cinematic events captured)")
        return
    for b in timeline:
        t = b.get("t")
        tstr = f"t={t:g}" if isinstance(t, (int, float)) else ""
        detail = " ".join(f"{k}={v}" for k, v in b.items()
                          if k not in ("kind", "raw", "seq", "t"))
        print(f"    #{b['seq']:<4} {b['kind']:<8} {tstr:<10} {detail}")


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(prog="tools.cinematics.preview",
                                description="Preview a cinematic in the running game.")
    p.add_argument("id", help="cinematic id (assets/cinematics/<id>.json)")
    p.add_argument("--at", default=None,
                   help="comma-separated seek timestamps, e.g. 1,3,8 "
                        "(default: auto from line/sfx cues)")
    p.add_argument("--base-url", default=BASE_URL)
    args = p.parse_args(argv)

    at = None
    if args.at:
        at = [float(x) for x in args.at.split(",") if x.strip()]

    try:
        result = preview(args.id, at=at, base_url=args.base_url)
    except GameNotRunning as e:
        print(f"[preview] {e}")
        print("[preview] Start the game and try again "
              "(dev_remote listens on :47001 during Flight).")
        return 2

    _print_timeline(result["events"])
    print(f"\n  Captured {len(result['shots'])} screenshot(s).")
    if result["contact_sheet"]:
        print(f"  Contact sheet: {result['contact_sheet']}")
    else:
        print("  No contact sheet (no screenshots captured).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

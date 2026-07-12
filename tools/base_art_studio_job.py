#!/usr/bin/env python3
"""Single-request worker for the in-game Base Art Studio.

Reads a JSON request, performs generate/edit/install/revert, and atomically
writes a JSON result. Intended to run out-of-process so the game never blocks.
"""
from __future__ import annotations
import argparse, base64, json, mimetypes, os, shutil, sys, tempfile, urllib.error, urllib.request, uuid
from datetime import datetime, timezone
from pathlib import Path

from gen_concourse_ai import API_URL, ASSET_ROOT, FILE_NAMES, REPO, generate_png, load_api_key

STAGE_ROOT = REPO / "generated" / "base_art_studio"
BACKUP_ROOT = STAGE_ROOT / "backups"
ALLOWED_ROOMS = frozenset(FILE_NAMES)


def safe_target(archetype: str, room: str) -> Path:
    if not archetype or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_-" for c in archetype):
        raise ValueError("invalid archetype")
    if room not in ALLOWED_ROOMS:
        raise ValueError("invalid room")
    target = (ASSET_ROOT / archetype / FILE_NAMES[room]).resolve()
    if ASSET_ROOT.resolve() not in target.parents:
        raise ValueError("asset path escapes root")
    return target


def stage_path(archetype: str, room: str) -> Path:
    return (STAGE_ROOT / archetype / room / "latest.png").resolve()


def atomic_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data); f.flush(); os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp): os.unlink(tmp)


def atomic_json(path: Path, value: dict) -> None:
    atomic_bytes(path, (json.dumps(value, indent=2) + "\n").encode())


def multipart(prompt: str, refs: list[Path], model: str, size: str, quality: str) -> tuple[bytes, str]:
    boundary = "----baseart" + uuid.uuid4().hex
    chunks: list[bytes] = []
    def field(name: str, value: str) -> None:
        chunks.extend([f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{value}\r\n".encode()])
    field("model", model); field("prompt", prompt); field("size", size); field("quality", quality)
    for ref in refs:
        if not ref.is_file(): raise ValueError(f"reference does not exist: {ref}")
        chunks.append((f"--{boundary}\r\nContent-Disposition: form-data; name=\"image[]\"; filename=\"{ref.name}\"\r\n"
                       f"Content-Type: {mimetypes.guess_type(ref.name)[0] or 'image/png'}\r\n\r\n").encode())
        chunks.extend([ref.read_bytes(), b"\r\n"])
    chunks.append(f"--{boundary}--\r\n".encode())
    return b"".join(chunks), boundary


def edit_png(prompt: str, refs: list[Path], key: str, model: str, size: str, quality: str, timeout: int) -> bytes:
    body, boundary = multipart(prompt, refs, model, size, quality)
    req = urllib.request.Request("https://api.openai.com/v1/images/edits", body,
        {"Authorization": f"Bearer {key}", "Content-Type": f"multipart/form-data; boundary={boundary}"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            result = json.loads(response.read())
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"OpenAI image edit error {exc.code}: {exc.read().decode('utf-8','replace')[:800]}") from exc
    try: return base64.b64decode(result["data"][0]["b64_json"], validate=True)
    except (KeyError, IndexError, ValueError) as exc: raise RuntimeError("OpenAI edit returned no valid image") from exc


def references(mode: str, target: Path, latest: Path) -> list[Path]:
    if mode == "text": return []
    # Once installation has captured it, "original" remains the pre-Studio
    # asset rather than silently becoming the most recently installed draft.
    try: relative = target.resolve().relative_to(ASSET_ROOT.resolve())
    except ValueError: relative = Path(target.name)  # test/custom-root seam
    saved_original = BACKUP_ROOT / "original" / relative
    original = saved_original if saved_original.is_file() else target
    if mode == "original": return [original]
    if mode == "latest": return [latest]
    if mode == "both": return [original, latest]
    raise ValueError("invalid reference_mode")


def run(job: dict) -> dict:
    action = job.get("action", "generate")
    archetype, room = str(job.get("archetype", "")), str(job.get("room", ""))
    target, latest = safe_target(archetype, room), stage_path(archetype, room)
    if action == "generate":
        prompt = str(job.get("prompt", "")).strip()
        if not prompt: raise ValueError("prompt is empty")
        refs = references(str(job.get("reference_mode", "text")), target, latest)
        key = load_api_key()
        if not key: raise RuntimeError("OPENAI_API_KEY or .openai_api_key is required")
        opts = (str(job.get("model", "gpt-image-2")), str(job.get("size", "1536x1024")), str(job.get("quality", "high")), int(job.get("timeout", 600)))
        data = edit_png(prompt, refs, key, *opts) if refs else generate_png(prompt, key, *opts)
        atomic_bytes(latest, data)
        return {"ok": True, "action": action, "preview_path": str(latest), "message": "generation complete"}
    if action == "install":
        if not latest.is_file(): raise ValueError("no latest image to install")
        # The stable original is captured once, so repeated iterations never
        # redefine what the explicit Revert Original action means.
        backup = BACKUP_ROOT / "original" / archetype / FILE_NAMES[room]
        if target.is_file() and not backup.exists(): atomic_bytes(backup, target.read_bytes())
        atomic_bytes(target, latest.read_bytes())
        return {"ok": True, "action": action, "preview_path": str(target), "backup_path": str(backup), "message": "installed with backup"}
    if action == "revert":
        original = BACKUP_ROOT / "original" / archetype / FILE_NAMES[room]
        if not original.is_file(): raise ValueError("no original backup exists")
        atomic_bytes(target, original.read_bytes())
        return {"ok": True, "action": action, "preview_path": str(target), "backup_path": str(original), "message": "reverted original backup"}
    raise ValueError("invalid action")


def main(argv=None) -> int:
    p = argparse.ArgumentParser(); p.add_argument("--request", type=Path, required=True); p.add_argument("--result", type=Path, required=True)
    a = p.parse_args(argv)
    try: result = run(json.loads(a.request.read_text(encoding="utf-8")))
    except Exception as exc: result = {"ok": False, "message": str(exc)}
    atomic_json(a.result, result)
    print(result["message"])
    return 0 if result["ok"] else 1

if __name__ == "__main__": raise SystemExit(main())

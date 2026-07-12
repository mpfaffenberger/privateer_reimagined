#!/usr/bin/env python3
"""Out-of-process worker for room art and landing ship composites."""
from __future__ import annotations
import argparse, base64, io, json, math, mimetypes, os, shutil, tempfile, urllib.error, urllib.request, uuid
from datetime import datetime, timezone
from pathlib import Path

from gen_concourse_ai import ASSET_ROOT, FILE_NAMES, REPO, generate_png, load_api_key

STAGE_ROOT = REPO / "generated" / "base_art_studio"
BACKUP_ROOT = STAGE_ROOT / "backups"
SHIP_ROOT = REPO / "assets" / "ships"
ALLOWED_ROOMS = frozenset(FILE_NAMES)


def safe_name(value: str, label: str) -> str:
    if not value or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789_-" for c in value):
        raise ValueError(f"invalid {label}")
    return value


def safe_target(archetype: str, room: str) -> Path:
    archetype = safe_name(archetype, "archetype")
    if room not in ALLOWED_ROOMS:
        raise ValueError("invalid room")
    target = (ASSET_ROOT / archetype / FILE_NAMES[room]).resolve()
    if ASSET_ROOT.resolve() not in target.parents:
        raise ValueError("asset path escapes root")
    return target


def safe_pair_target(archetype: str, ship: str) -> Path:
    archetype, ship = safe_name(archetype, "archetype"), safe_name(ship, "ship")
    if not (ASSET_ROOT / archetype / "concourse.json").is_file():
        raise ValueError("unknown archetype")
    if not (SHIP_ROOT / ship / "ship.json").is_file():
        raise ValueError("unknown ship")
    return (ASSET_ROOT / archetype / "landing_ships" / f"{ship}.png").resolve()


def stage_path(archetype: str, room: str) -> Path:
    return (STAGE_ROOT / archetype / room / "latest.png").resolve()


def pair_stage_path(archetype: str, ship: str) -> Path:
    return (STAGE_ROOT / "landing_ships" / archetype / ship / "latest.png").resolve()


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
    boundary, chunks = "----baseart" + uuid.uuid4().hex, []
    def field(name: str, value: str) -> None:
        chunks.append(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{value}\r\n".encode())
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
        with urllib.request.urlopen(req, timeout=timeout) as response: result = json.loads(response.read())
    except urllib.error.HTTPError as exc:
        raise RuntimeError(f"OpenAI image edit error {exc.code}: {exc.read().decode('utf-8','replace')[:800]}") from exc
    try: return base64.b64decode(result["data"][0]["b64_json"], validate=True)
    except (KeyError, IndexError, ValueError) as exc: raise RuntimeError("OpenAI edit returned no valid image") from exc


def references(mode: str, target: Path, latest: Path) -> list[Path]:
    if mode == "text": return []
    try: relative = target.resolve().relative_to(ASSET_ROOT.resolve())
    except ValueError: relative = Path(target.name)
    saved_original = BACKUP_ROOT / "original" / relative
    original = saved_original if saved_original.is_file() else target
    if mode == "original": return [original]
    if mode == "latest": return [latest]
    if mode == "both": return [original, latest]
    raise ValueError("invalid reference_mode")


def ship_pose(archetype: str, ship: str) -> dict:
    pose = {"az": 35.0, "el": -20.0, "rect": [0.34, 0.42, 0.32, 0.34]}
    path = ASSET_ROOT / archetype / "links.json"
    if not path.is_file(): return pose
    root = json.loads(path.read_text(encoding="utf-8"))
    candidates = list(root.get("_ships", []))
    if isinstance(root.get("_ship"), dict): candidates.append(root["_ship"])
    for item in candidates:
        if item.get("class") == ship:
            pose.update({k: item[k] for k in ("az", "el", "rect") if k in item})
            break
    return pose


def angle_vector(az: float, el: float) -> tuple[float, float, float]:
    az, el = math.radians(az), math.radians(el)
    return math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az)


def posed_ship_reference(archetype: str, ship: str) -> tuple[Path, dict]:
    pose = ship_pose(archetype, ship)
    manifest = SHIP_ROOT / ship / "atlas_manifest_3d.json"
    if not manifest.is_file(): manifest = SHIP_ROOT / ship / "atlas_manifest.json"
    if not manifest.is_file(): raise ValueError(f"no sprite atlas manifest for {ship}")
    samples = json.loads(manifest.read_text(encoding="utf-8")).get("samples", [])
    if not samples: raise ValueError(f"sprite atlas has no samples for {ship}")
    wanted = angle_vector(float(pose["az"]), float(pose["el"]))
    def score(sample: dict) -> float:
        actual = angle_vector(float(sample.get("az", 0)), float(sample.get("el", 0)))
        return sum(a * b for a, b in zip(wanted, actual))
    sample = max(samples, key=score)
    path = (REPO / "assets" / str(sample["sprite"])).resolve()
    if not path.is_file(): raise ValueError(f"posed sprite does not exist: {path}")
    return path, pose


def make_placement_guide(background: Path, sprite: Path, pose: dict, output: Path) -> Path:
    try: from PIL import Image
    except ImportError as exc: raise RuntimeError("Pillow is required to build the placement guide") from exc
    with Image.open(background) as bg_src, Image.open(sprite) as ship_src:
        bg, ship = bg_src.convert("RGBA"), ship_src.convert("RGBA")
        x, y, w, h = (float(v) for v in pose["rect"])
        box_w, box_h = max(1, round(w * bg.width)), max(1, round(h * bg.height))
        scale = min(box_w / ship.width, box_h / ship.height)
        size = (max(1, round(ship.width * scale)), max(1, round(ship.height * scale)))
        ship = ship.resize(size, Image.Resampling.LANCZOS)
        left = round(x * bg.width + (box_w - size[0]) / 2)
        top = round(y * bg.height + (box_h - size[1]) / 2)
        bg.alpha_composite(ship, (left, top))
        data = io.BytesIO(); bg.convert("RGB").save(data, format="PNG")
    atomic_bytes(output, data.getvalue())
    return output


def pair_references(job: dict, archetype: str, ship: str, target: Path, latest: Path) -> list[Path]:
    enabled = job.get("references", {})
    background = safe_target(archetype, "landing")
    sprite, pose = posed_ship_reference(archetype, ship)
    refs: list[Path] = []
    if enabled.get("placement_guide", True):
        refs.append(make_placement_guide(background, sprite, pose, latest.with_name("placement_guide.png")))
    if enabled.get("background_installed", True): refs.append(background)
    if enabled.get("background_original"):
        original = BACKUP_ROOT / "original" / archetype / FILE_NAMES["landing"]
        if original.is_file(): refs.append(original)
    if enabled.get("background_latest"):
        candidate = stage_path(archetype, "landing")
        if candidate.is_file(): refs.append(candidate)
    if enabled.get("ship_pose", True): refs.append(sprite)
    if enabled.get("ship_design", True): refs.extend(sorted((SHIP_ROOT / ship).glob("canonical_reference*.png")))
    if enabled.get("composite_installed") and target.is_file(): refs.append(target)
    if enabled.get("composite_latest") and latest.is_file(): refs.append(latest)
    unique: list[Path] = []
    for ref in refs:
        resolved = ref.resolve()
        if resolved not in unique: unique.append(resolved)
    if not unique: raise ValueError("select at least one available reference")
    return unique[:16]


def generation(job: dict, latest: Path, refs: list[Path]) -> dict:
    prompt = str(job.get("prompt", "")).strip()
    if not prompt: raise ValueError("prompt is empty")
    key = load_api_key()
    if not key: raise RuntimeError("OPENAI_API_KEY or .openai_api_key is required")
    opts = (str(job.get("model", "gpt-image-2")), str(job.get("size", "1536x1024")),
            str(job.get("quality", "high")), int(job.get("timeout", 600)))
    data = edit_png(prompt, refs, key, *opts) if refs else generate_png(prompt, key, *opts)
    atomic_bytes(latest, data)
    return {"ok": True, "action": "generate", "preview_path": str(latest),
            "references": [str(path) for path in refs], "message": "generation complete"}


def run_room(job: dict) -> dict:
    action = job.get("action", "generate")
    archetype, room = str(job.get("archetype", "")), str(job.get("room", ""))
    target, latest = safe_target(archetype, room), stage_path(archetype, room)
    if action == "generate": return generation(job, latest, references(str(job.get("reference_mode", "text")), target, latest))
    if action == "install":
        if not latest.is_file(): raise ValueError("no latest image to install")
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


def run_pair(job: dict) -> dict:
    action = job.get("action", "generate")
    archetype, ship = str(job.get("archetype", "")), str(job.get("ship", ""))
    target, latest = safe_pair_target(archetype, ship), pair_stage_path(archetype, ship)
    if action == "generate": return generation(job, latest, pair_references(job, archetype, ship, target, latest))
    if action == "install":
        if not latest.is_file(): raise ValueError("no latest composite to install")
        backup = None
        if target.is_file():
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            backup = BACKUP_ROOT / "landing_ships" / archetype / ship / f"{stamp}.png"
            atomic_bytes(backup, target.read_bytes())
        atomic_bytes(target, latest.read_bytes())
        return {"ok": True, "action": action, "preview_path": str(target),
                "backup_path": str(backup or ""), "message": "composite installed" + (" with backup" if backup else "")}
    raise ValueError("invalid action")


def run(job: dict) -> dict:
    return run_pair(job) if job.get("target_kind") == "landing_ship" else run_room(job)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--request", type=Path, required=True); parser.add_argument("--result", type=Path, required=True)
    args = parser.parse_args(argv)
    try: result = run(json.loads(args.request.read_text(encoding="utf-8")))
    except Exception as exc: result = {"ok": False, "message": str(exc)}
    atomic_json(args.result, result); print(result["message"])
    return 0 if result["ok"] else 1

if __name__ == "__main__": raise SystemExit(main())

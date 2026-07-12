#!/usr/bin/env python3
"""Generate cohesive base-room backgrounds with OpenAI's image API.

Safe by default: images land in ``generated/base_imagery``. Pass ``--install``
to copy them into ``assets/concourse/<archetype>``; replaced files are backed
up first. Every run writes a JSON manifest containing the exact prompts.

Examples:
    python tools/gen_concourse_ai.py --archetype mining --room shipdealer --dry-run
    python tools/gen_concourse_ai.py --archetype mining --room shipdealer
    python tools/gen_concourse_ai.py --archetype pirate --room all --skip-existing
    python tools/gen_concourse_ai.py --archetype all --room concourse
    python tools/gen_concourse_ai.py --archetype mining --room all --install

Set OPENAI_API_KEY or put the key in repo-root ``.openai_api_key`` for real
 generation. The environment takes precedence. Dry runs need no key and cost nothing.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import json
import os
import shutil
import sys
import time
import urllib.error
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

REPO = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = REPO / "generated" / "base_imagery"
ASSET_ROOT = REPO / "assets" / "concourse"
API_URL = "https://api.openai.com/v1/images/generations"

# This is intentionally art direction, not a named living artist or a request
# to imitate Privateer's original paintings. Cohesion beats copyright roulette.
STYLE = (
    "Original cinematic retro-futurist space-opera environment concept art. "
    "A believable lived-in frontier in the 27th century; grounded industrial "
    "materials, chunky practical machinery, layered depth, atmospheric haze, "
    "warm pools of light against cool shadows, restrained amber/cyan accents, "
    "hand-painted texture with modern high-detail readability. Wide game "
    "background composition, eye-level camera, clear navigable floor and "
    "doorways, strong silhouettes, no fisheye lens. No text, signage lettering, "
    "logos, UI, borders, watermarks, captions, or recognizable characters."
)

ARCHETYPES = {
    "agricultural": (
        "Prosperous agricultural colony: greenhouse architecture, hydroponic "
        "infrastructure, pale stone, warm wood, greenery, clean civic spaces."
    ),
    "military": (
        "Confederation military installation: disciplined modular construction, "
        "navy grey armor, blue-white task lighting, secure doors, immaculate order."
    ),
    "mining": (
        "Asteroid mining colony carved into ochre rock: heavy beams, dust, worn "
        "steel, cable runs, hydraulic equipment, practical amber work lights."
    ),
    "newcon": (
        "Wealthy metropolitan orbital hub: monumental modern space architecture, "
        "polished dark metal, glass, elegant indirect lighting, bustling scale."
    ),
    "pirate": (
        "Hidden pirate outpost assembled from salvaged hulls: patched steel, "
        "exposed wiring, improvised neon, contraband clutter, dangerous atmosphere."
    ),
    "pleasure": (
        "Luxury pleasure world starport: lush landscaping, water features, curved "
        "architecture, sunset colors, expensive materials, decadent calm."
    ),
    "refinery": (
        "Orbital refinery complex: pipes, pressure vessels, heat shielding, oily "
        "steel, steam, orange furnace glow, dense functional industrial detail."
    ),
}

ROOMS = {
    "landing": (
        "Spacecraft landing bay viewed from the service apron. A broad empty pad "
        "dominates the foreground, with guide lights, blast shielding, hangar doors, "
        "fuel equipment and a clearly visible pedestrian exit. No spacecraft."
    ),
    "concourse": (
        "Main public concourse and navigation hub. Multiple visually distinct exits "
        "lead to commerce, guilds, hangars and entertainment; open central floor, "
        "seating and environmental storytelling around the perimeter."
    ),
    "bar": (
        "Frontier bar interior viewed from the entrance. Long service counter, booths, "
        "shadowed conversation alcoves and a small open area for patrons; inviting but "
        "slightly dangerous. No people, bottles may appear without readable labels."
    ),
    "commodity": (
        "Commodity exchange and cargo brokerage office. Trading counter, warehouse "
        "windows, cargo samples, scales and abstract glowing market displays with no "
        "legible symbols or text; central space kept clear for game UI."
    ),
    "shipdealer": (
        "Spacecraft dealership showroom office overlooking a hangar. A sales desk and "
        "large panoramic windows frame distant anonymous ship silhouettes; one obvious "
        "dealer position and a side doorway toward the equipment department. No people."
    ),
    "equipment": (
        "Spacecraft equipment dealer and workshop. Weapon housings, shield components, "
        "engine parts and tool benches displayed in organized wall bays; broad clear "
        "central region for inventory UI, no readable labels."
    ),
    "mercguild": (
        "Mercenary guild contract hall. Tactical briefing alcoves, armored lockers, "
        "mission terminal desk and trophy cases; severe, professional atmosphere, no "
        "weapons pointed toward camera and no people."
    ),
    "merchguild": (
        "Merchants guild office and contract hall. Refined trade-house interior with "
        "cargo route sculpture, negotiation desks, secure archive doors and abstract "
        "data displays; prosperous but practical, no people."
    ),
}

FILE_NAMES = {
    "landing": "landing_bg.png",
    "concourse": "concourse_bg.png",
    "bar": "bar_bg.png",
    "commodity": "commodity_bg.png",
    "shipdealer": "shipdealer_bg.png",
    "equipment": "equipment_bg.png",
    "mercguild": "mercguild_bg.png",
    "merchguild": "merchguild_bg.png",
}


@dataclass(frozen=True)
class Job:
    archetype: str
    room: str
    prompt: str
    staged_path: str
    install_path: str


def compose_prompt(archetype: str, room: str, location: str, faction: str) -> str:
    context = ""
    if location:
        context += f" This specific location is {location.strip()}."
    if faction:
        context += f" Its operator/faction is {faction.strip()}."
    return f"{ROOMS[room]} {ARCHETYPES[archetype]}{context} {STYLE}"


def selected(value: str, choices: dict[str, str]) -> list[str]:
    return list(choices) if value == "all" else [value]


def plan_jobs(args: argparse.Namespace) -> list[Job]:
    jobs: list[Job] = []
    for archetype in selected(args.archetype, ARCHETYPES):
        for room in selected(args.room, ROOMS):
            staged = args.output / archetype / FILE_NAMES[room]
            installed = ASSET_ROOT / archetype / FILE_NAMES[room]
            jobs.append(Job(
                archetype=archetype,
                room=room,
                prompt=compose_prompt(archetype, room, args.location, args.faction),
                staged_path=str(staged),
                install_path=str(installed),
            ))
    return jobs


def load_api_key(key_file: Path = REPO / ".openai_api_key") -> str:
    """Resolve the API key without logging it or leaking it into manifests."""
    environment_key = os.environ.get("OPENAI_API_KEY", "").strip()
    if environment_key:
        return environment_key
    if key_file.is_file():
        return key_file.read_text(encoding="utf-8").strip()
    return ""


def generate_png(prompt: str, api_key: str, model: str, size: str,
                 quality: str, timeout: int) -> bytes:
    payload = json.dumps({
        "model": model,
        "prompt": prompt,
        "size": size,
        "quality": quality,
        "n": 1,
    }).encode("utf-8")
    request = urllib.request.Request(
        API_URL,
        data=payload,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:800]
        raise RuntimeError(f"OpenAI image API error {exc.code}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"OpenAI image API request failed: {exc.reason}") from exc
    try:
        return base64.b64decode(body["data"][0]["b64_json"], validate=True)
    except (KeyError, IndexError, ValueError) as exc:
        raise RuntimeError("OpenAI response did not contain a valid b64_json image") from exc


def backup_and_install(source: Path, destination: Path, backup_root: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        relative = destination.relative_to(ASSET_ROOT)
        backup = backup_root / relative
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(destination, backup)
    shutil.copy2(source, destination)


def write_manifest(path: Path, args: argparse.Namespace, jobs: Iterable[Job],
                   results: list[dict[str, str]]) -> None:
    payload = {
        "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "model": args.model,
        "size": args.size,
        "quality": args.quality,
        "dry_run": args.dry_run,
        "installed": args.install,
        "jobs": [asdict(job) for job in jobs],
        "results": results,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--archetype", choices=[*ARCHETYPES, "all"], required=True)
    p.add_argument("--room", choices=[*ROOMS, "all"], required=True)
    p.add_argument("--location", default="", help="Optional named base/location context")
    p.add_argument("--faction", default="", help="Optional owning faction context")
    p.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                   help=f"Safe staging root (default: {DEFAULT_OUTPUT})")
    p.add_argument("--model", default="gpt-image-2")
    p.add_argument("--size", default="1536x1024")
    p.add_argument("--quality", choices=["low", "medium", "high", "auto"], default="high")
    p.add_argument("--timeout", type=int, default=600)
    p.add_argument("--dry-run", action="store_true", help="Print prompts; make no API calls")
    p.add_argument("--skip-existing", action="store_true", help="Resume without regenerating staged files")
    p.add_argument("--install", action="store_true",
                   help="Copy generated files into assets/concourse and back up originals")
    return p


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    args.output = args.output.resolve()
    jobs = plan_jobs(args)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    backup_root = args.output / "backups" / stamp
    results: list[dict[str, str]] = []

    if args.install and args.dry_run:
        print("note: --install has no effect during --dry-run", file=sys.stderr)

    api_key = load_api_key()
    if not args.dry_run and not api_key:
        print(
            "error: set OPENAI_API_KEY or add a key to repo-root "
            ".openai_api_key; use --dry-run to review prompts",
            file=sys.stderr,
        )
        return 2

    print(f"Planned {len(jobs)} image(s); staging under {args.output}")
    for index, job in enumerate(jobs, 1):
        staged = Path(job.staged_path)
        print(f"\n[{index}/{len(jobs)}] {job.archetype}/{job.room}")
        print(f"  output: {staged}")
        if args.dry_run:
            print(f"  prompt: {job.prompt}")
            results.append({"job": f"{job.archetype}/{job.room}", "status": "dry-run"})
            continue

        if args.skip_existing and staged.is_file() and staged.stat().st_size > 0:
            status = "skipped-existing"
        else:
            staged.parent.mkdir(parents=True, exist_ok=True)
            started = time.monotonic()
            png = generate_png(job.prompt, api_key, args.model, args.size,
                               args.quality, args.timeout)
            staged.write_bytes(png)
            status = f"generated ({len(png)} bytes, {time.monotonic() - started:.1f}s)"
        print(f"  {status}")

        if args.install:
            backup_and_install(staged, Path(job.install_path), backup_root)
            print(f"  installed: {job.install_path}")
        results.append({"job": f"{job.archetype}/{job.room}", "status": status})

    manifest = args.output / f"manifest-{stamp}.json"
    write_manifest(manifest, args, jobs, results)
    print(f"\nManifest: {manifest}")
    if args.install:
        print(f"Backups:  {backup_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Generate landing-bay composites for every base archetype and ship class.

The batch uses the same reference recipe as the proven F11 workflow, runs a
small bounded worker pool, installs successful images with backups, and writes
an atomic progress manifest so interrupted batches are safely resumable.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import base_art_studio_job as studio

MANIFEST = studio.STAGE_ROOT / "landing_ships" / "batch_manifest.json"
DEFAULT_REFERENCES = {
    "placement_guide": True,
    "background_installed": True,
    "background_original": False,
    "background_latest": False,
    "ship_pose": True,
    "ship_design": True,
    "composite_installed": True,
    "composite_latest": False,
}


@dataclass(frozen=True)
class Pair:
    archetype: str
    ship: str


@dataclass
class Result:
    archetype: str
    ship: str
    status: str
    message: str
    seconds: float = 0.0
    output: str = ""


def discover_pairs(archetypes: set[str] | None = None,
                   ships: set[str] | None = None) -> list[Pair]:
    available_archetypes = sorted(
        path.name for path in studio.ASSET_ROOT.iterdir()
        if (path / "concourse.json").is_file() and (path / "landing_bg.png").is_file()
    )
    available_ships = sorted(
        path.name for path in studio.SHIP_ROOT.iterdir()
        if (path / "ship.json").is_file()
    )
    if archetypes:
        unknown = archetypes - set(available_archetypes)
        if unknown:
            raise ValueError(f"unknown archetype(s): {', '.join(sorted(unknown))}")
        available_archetypes = [name for name in available_archetypes if name in archetypes]
    if ships:
        unknown = ships - set(available_ships)
        if unknown:
            raise ValueError(f"unknown ship(s): {', '.join(sorted(unknown))}")
        available_ships = [name for name in available_ships if name in ships]
    return [Pair(archetype, ship) for archetype in available_archetypes
            for ship in available_ships]


def prompt_for(pair: Pair) -> str:
    return (
        f"Create a finished cinematic landing-bay scene for the {pair.ship} at a "
        f"{pair.archetype} base. Integrate the exact referenced ship design naturally "
        f"into the referenced {pair.archetype} landing bay. Use the placement guide "
        "for approximate position, scale, and camera angle, but correct perspective "
        "and contact with the ground. Match environmental lighting, shadows, reflected "
        "color, painterly detail, atmospheric depth, and resolution so ship and bay "
        "look like one authored image. Preserve the bay layout and ship identity. "
        "This image will be shared by all bases of this archetype using this ship. "
        "No text, logos, UI, people, watermarks, or additional spacecraft."
    )


def references_for(repaint: bool) -> dict:
    """The F11 recipe. After a bay repaint the installed composite shows the
    old bay, so it must not be a reference."""
    return {**DEFAULT_REFERENCES, "composite_installed": not repaint}


def request_for(pair: Pair, action: str = "generate", repaint: bool = False) -> dict:
    return {
        "target_kind": "landing_ship",
        "action": action,
        "archetype": pair.archetype,
        "ship": pair.ship,
        "prompt": prompt_for(pair),
        "references": references_for(repaint),
        "model": "gpt-image-2",
        "size": "1536x1024",
        "quality": "high",
        "timeout": 600,
    }


def generate_pair(pair: Pair, install: bool, repaint: bool = False) -> Result:
    started = time.monotonic()
    try:
        generated = studio.run(request_for(pair, repaint=repaint))
        output = str(generated.get("preview_path", ""))
        message = str(generated.get("message", "generation complete"))
        if install:
            installed = studio.run(request_for(pair, "install", repaint))
            output = str(installed.get("preview_path", output))
            message += "; " + str(installed.get("message", "installed"))
        return Result(pair.archetype, pair.ship, "complete", message,
                      time.monotonic() - started, output)
    except Exception as exc:
        return Result(pair.archetype, pair.ship, "failed", str(exc),
                      time.monotonic() - started)


def write_manifest(path: Path, results: dict[str, Result], total: int,
                   workers: int, install: bool) -> None:
    complete = sum(item.status == "complete" for item in results.values())
    failed = sum(item.status == "failed" for item in results.values())
    studio.atomic_json(path, {
        "total": total,
        "complete": complete,
        "failed": failed,
        "pending": total - complete - failed,
        "workers": workers,
        "install": install,
        "references": DEFAULT_REFERENCES,
        "results": {key: asdict(value) for key, value in sorted(results.items())},
    })


def load_results(path: Path) -> dict[str, Result]:
    if not path.is_file():
        return {}
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
        return {key: Result(**value) for key, value in payload.get("results", {}).items()}
    except (OSError, ValueError, TypeError):
        return {}


def run_batch(pairs: list[Pair], workers: int, install: bool, force: bool,
              manifest: Path = MANIFEST, repaint: bool = False) -> int:
    prior = {} if force else load_results(manifest)
    results: dict[str, Result] = {}
    pending: list[Pair] = []
    for pair in pairs:
        key = f"{pair.archetype}/{pair.ship}"
        installed = studio.safe_pair_target(pair.archetype, pair.ship)
        if not force and installed.is_file():
            results[key] = Result(pair.archetype, pair.ship, "complete",
                                  "skipped existing installed composite", output=str(installed))
        elif not force and key in prior and prior[key].status == "complete" and not install:
            results[key] = prior[key]
        else:
            pending.append(pair)
    write_manifest(manifest, results, len(pairs), workers, install)
    print(f"Landing composites: {len(pairs)} total, {len(results)} skipped, "
          f"{len(pending)} queued with {workers} worker(s)")
    if not pending:
        return 0

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(generate_pair, pair, install, repaint): pair for pair in pending}
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            key = f"{result.archetype}/{result.ship}"
            results[key] = result
            write_manifest(manifest, results, len(pairs), workers, install)
            marker = "OK" if result.status == "complete" else "FAIL"
            print(f"[{len(results):3d}/{len(pairs):3d}] {marker} {key}: {result.message}",
                  flush=True)
    return 1 if any(item.status == "failed" for item in results.values()) else 0


def parse_names(values: list[str] | None) -> set[str] | None:
    return set(values) if values else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workers", type=int, default=2,
                        help="parallel OpenAI requests (default: 2)")
    parser.add_argument("--archetype", action="append",
                        help="limit to an archetype; repeatable")
    parser.add_argument("--ship", action="append", help="limit to a ship; repeatable")
    parser.add_argument("--generate-only", action="store_true",
                        help="stage outputs without installing them")
    parser.add_argument("--force", action="store_true",
                        help="regenerate existing successful/installed pairs")
    parser.add_argument("--repaint", action="store_true",
                        help="the bay was repainted: don't reference the old installed "
                             "composite (use with --force)")
    parser.add_argument("--dry-run", action="store_true",
                        help="list the plan without making API requests")
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    args = parser.parse_args(argv)
    if not 1 <= args.workers <= 8:
        parser.error("--workers must be between 1 and 8")
    try:
        pairs = discover_pairs(parse_names(args.archetype), parse_names(args.ship))
    except ValueError as exc:
        parser.error(str(exc))
    if args.dry_run:
        print(f"Would process {len(pairs)} landing composites with {args.workers} worker(s)")
        for pair in pairs:
            print(f"  {pair.archetype}/{pair.ship}")
        return 0
    return run_batch(pairs, args.workers, not args.generate_only,
                     args.force, args.manifest, args.repaint)


if __name__ == "__main__":
    raise SystemExit(main())

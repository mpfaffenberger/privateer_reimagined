#!/usr/bin/env python3
"""Generate high-resolution base exteriors from decoded canonical GOG frames."""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import batch_generate_ship_sprites as sprite_gen  # noqa: E402
from extract_base_exterior_refs import BASE_APPEARANCES  # noqa: E402

def load_api_key() -> None:
    if os.environ.get("OPENAI_API_KEY"):
        return
    key_file = REPO / ".openai_api_key"
    if not key_file.is_file():
        raise RuntimeError("OPENAI_API_KEY and .openai_api_key are both missing")
    os.environ["OPENAI_API_KEY"] = key_file.read_text(encoding="utf-8").strip()


DISPLAY_NAMES = {
    "agricultural": "Agricultural planet",
    "new_constantinople": "New Constantinople station",
    "new_detroit": "New Detroit planet",
    "oxford": "Oxford planet",
    "perry": "Perry Naval Base",
    "pleasure": "Pleasure planet",
    "refinery": "Refinery station",
    "mining": "Asteroid mining base",
}
PLANETS = {"agricultural", "new_detroit", "oxford", "pleasure"}


def prompt_for(identity: str) -> str:
    if identity in PLANETS:
        return f"""Create a finished high-resolution space-view sprite of the canonical
Privateer {DISPLAY_NAMES[identity]}. This subject is a PLANETARY GLOBE, never a
space station. Follow all four reference frames strictly. Preserve the reference
surface colors, oceans, continents, clouds, night-side illumination, atmospheric
rim, and recognizable planetary identity. Repaint the low-resolution source as a
cinematic 1990s science-fiction planet. Show one complete spherical planet centered
with generous transparent-background clearance. No station, spacecraft, mechanical
hull, antenna, docking structure, city floating in space, starfield, nebula, text,
logo, border, UI, moon, or additional object. Transparent background outside the
atmospheric limb.
"""
    if identity == "perry":
        return """Create a high-resolution canonical Privateer Perry Naval Base sprite.
GEOMETRY IS LOCKED TO THE REFERENCES: one large near-spherical upper command body,
a thin dark horizontal equatorial docking/service belt, a long narrow vertical neck
hanging from the bottom center, and one much smaller spherical module at the bottom.
The complete silhouette is tall and vertically stacked, never horizontal. Preserve
sparse top antennas, red navigation lights, gray-blue plated hull, window bands, and
the canonical PERRY lettering curved across the upper sphere. Do not invent rings,
sideways dumbbells, radial wheels, a second large sphere, lateral station arms, or
additional modules. One complete station, centered, front-biased three-quarter view,
transparent background. No planet, starfield, ships, people, UI, border, extra text,
or watermark.
"""
    lettering = "Do not add text, labels, logos, insignia, numbers, or watermarks."
    return f"""Create a finished high-resolution space-view sprite of the canonical
Privateer {DISPLAY_NAMES[identity]}. This subject is a SPACE STATION, not a planet.
Follow all four reference frames strictly. Preserve the exact silhouette,
proportions, modules, docking structures, antennae, and identifying architecture.
Repaint the low-resolution source with detailed 1990s cinematic science-fiction
hull plating, restrained weathering, windows, and practical navigation lights.
Use a front-biased three-quarter presentation suitable for a camera-facing billboard.
Show one complete station centered with generous transparent-background clearance.
No planet, starfield, nebula, ships, people, border, UI, or cast-off debris.
{lettering}
"""


def make_job(identity: str, refs_root: Path, stage: Path) -> sprite_gen.SpriteJob:
    composite = refs_root / identity / "composite.png"
    if not composite.is_file():
        raise ValueError(f"{identity}: missing composed canonical reference {composite}")
    refs = [composite]
    perry_reference = REPO / "generated/base_imagery/perry_space_reference.png"
    if identity == "perry" and perry_reference.is_file():
        refs = [perry_reference, composite]
    prompt = prompt_for(identity)
    prompt_path = stage / "prompts" / f"{identity}.txt"
    prompt_path.parent.mkdir(parents=True, exist_ok=True)
    prompt_path.write_text(prompt, encoding="utf-8")
    return sprite_gen.SpriteJob(
        ship=identity,
        az=0.0,
        el=0.0,
        reference=refs[0],
        canonical_refs=tuple(refs[1:]),
        raw_output=stage / "raw" / f"base_{identity}.png",
        clean_output=stage / "clean" / f"base_{identity}.png",
        prompt_file=prompt_path,
        prompt=prompt,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", action="append", choices=sorted(BASE_APPEARANCES))
    parser.add_argument("--refs", type=Path, default=REPO / "generated/base_exterior_refs")
    parser.add_argument("--stage", type=Path, default=REPO / "generated/base_exterior_generation")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--quality", default="high", choices=("low", "medium", "high"))
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.workers <= 4:
        parser.error("--workers must be between 1 and 4")

    identities = args.identity or list(BASE_APPEARANCES)
    jobs = [make_job(identity, args.refs, args.stage) for identity in identities]
    for job in jobs:
        print(f"{job.ship}: {len(job.all_references)} refs -> {job.clean_output}")
    if args.dry_run:
        return 0
    load_api_key()

    def generate(job: sprite_gen.SpriteJob) -> tuple[str, str]:
        try:
            if args.force or not job.clean_output.is_file():
                sprite_gen.run_pixelart_tool(job, args.quality)
                sprite_gen.clean_sprite(job, preview=False)
            target = REPO / "assets/sprites" / f"base_{job.ship}.png"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(job.clean_output, target)
            return job.ship, "installed"
        except Exception as exc:  # batch reports every identity
            return job.ship, f"FAILED: {type(exc).__name__}: {exc}"

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        for identity, status in pool.map(generate, jobs):
            print(f"{identity}: {status}", flush=True)
            failed += status.startswith("FAILED")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

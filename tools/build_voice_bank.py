#!/usr/bin/env python3
"""Build assets/data/voice_bank.json from assets/speech/generated/comms.json.

The engine needs two lookups into the generated comm audio:
  1. by (faction, category) -> list of mp3 paths
  2. by voice_id           -> list of mp3 paths

This script DERIVES that manifest purely from comms.json, only keeping lines
whose mp3 actually exists on disk. It is idempotent: running it twice with the
same inputs produces the same output. Beautiful is better than ugly; explicit
is better than implicit. -- Zen of Python, lightly paraphrased for a puppy.
"""

from __future__ import annotations

import json
import os
import sys

# --- Paths (resolved relative to the repo root, not the cwd) -----------------
THIS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(THIS_DIR)

COMMS_JSON = os.path.join(REPO_ROOT, "assets", "speech", "generated", "comms.json")
AUDIO_DIR = os.path.join(REPO_ROOT, "assets", "speech", "generated", "audio")
OUTPUT_JSON = os.path.join(REPO_ROOT, "assets", "data", "voice_bank.json")

# Audio paths are stored repo-relative with forward slashes so the manifest is
# portable regardless of where the repo lives on disk.
AUDIO_REL_PREFIX = "assets/speech/generated/audio"

# Female voice classification (Phase-0). The preferred placeholder for the
# Confed female voice is PrivFlightV0001; the rest are fallbacks so the alias
# can still resolve if the preferred voice ever drops out of comms.json.
PREFERRED_CONFED_F_VOICE = "PrivFlightV0001"
KNOWN_FEMALE_VOICES = (
    "PrivFlightV0001",
    "PrivFlightV0401",
    "PrivFlightV1401",
)

CONFED_F_NOTE = (
    "confed_f is a Phase-0 placeholder female Confed voice; it currently "
    "aliases an existing female voice until dedicated Confed voices arrive "
    "in Phase 2."
)


def audio_rel_path(line_id: str) -> str:
    """Return the repo-relative mp3 path for a given line id."""
    return f"{AUDIO_REL_PREFIX}/{line_id}.mp3"


def audio_exists(line_id: str) -> bool:
    """True only if the mp3 for this line actually exists on disk."""
    return os.path.isfile(os.path.join(AUDIO_DIR, f"{line_id}.mp3"))


def build_manifest(comms: dict) -> dict:
    """Derive the voice_bank manifest from parsed comms.json data."""
    by_faction_category: dict[str, dict[str, list[str]]] = {}
    by_voice: dict[str, list[str]] = {}
    female_voice_paths: dict[str, list[str]] = {}

    kept = 0
    skipped = 0

    for line in comms.get("lines", []):
        line_id = line.get("id")
        faction = line.get("faction")
        category = line.get("category")
        voice_id = line.get("voice_id")

        if not line_id or not audio_exists(line_id):
            skipped += 1
            continue

        path = audio_rel_path(line_id)
        kept += 1

        if faction and category:
            by_faction_category.setdefault(faction, {}).setdefault(
                category, []
            ).append(path)

        if voice_id:
            by_voice.setdefault(voice_id, []).append(path)
            if voice_id in KNOWN_FEMALE_VOICES:
                female_voice_paths.setdefault(voice_id, []).append(path)

    aliases = {"confed_f": resolve_confed_f(by_voice, female_voice_paths)}

    manifest = {
        "_note": CONFED_F_NOTE,
        "by_faction_category": by_faction_category,
        "by_voice": by_voice,
        "aliases": aliases,
    }
    return manifest, kept, skipped


def resolve_confed_f(
    by_voice: dict[str, list[str]],
    female_voice_paths: dict[str, list[str]],
) -> list[str]:
    """Pick the placeholder female Confed voice paths.

    Prefer PrivFlightV0001; otherwise fall back to any known female voice
    that is actually present in the manifest.
    """
    if PREFERRED_CONFED_F_VOICE in by_voice:
        return list(by_voice[PREFERRED_CONFED_F_VOICE])

    for voice_id in KNOWN_FEMALE_VOICES:
        if voice_id in female_voice_paths:
            return list(female_voice_paths[voice_id])

    return []


def main() -> int:
    with open(COMMS_JSON, "r", encoding="utf-8") as fh:
        comms = json.load(fh)

    manifest, kept, skipped = build_manifest(comms)

    os.makedirs(os.path.dirname(OUTPUT_JSON), exist_ok=True)
    with open(OUTPUT_JSON, "w", encoding="utf-8") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True, ensure_ascii=False)
        fh.write("\n")

    # Verify the file we just wrote is valid JSON.
    with open(OUTPUT_JSON, "r", encoding="utf-8") as fh:
        json.load(fh)

    n_factions = len(manifest["by_faction_category"])
    n_voices = len(manifest["by_voice"])
    n_confed_f = len(manifest["aliases"]["confed_f"])
    print(
        f"voice_bank.json: {kept} lines kept, {skipped} skipped, "
        f"{n_factions} factions, {n_voices} voices, "
        f"confed_f={n_confed_f} paths -> {os.path.relpath(OUTPUT_JSON, REPO_ROOT)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

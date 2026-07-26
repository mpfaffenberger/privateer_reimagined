#!/usr/bin/env python3
"""Import the supplied Freelancer bar-music playlist for local game use.

The audio is deliberately written under assets/music/original/, which is
.gitignored. A clean clone contains no copyrighted game music; owners can run
this importer against a source they are entitled to use.

Requires yt-dlp and ffmpeg on PATH. yt-dlp downloads the playlist's best audio
and asks ffmpeg to produce the PCM16 WAV files expected by audio.cpp.

Usage::

    python tools/import_freelancer_bar_music.py
    python tools/import_freelancer_bar_music.py --url PLAYLIST_URL
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_URL = "https://www.youtube.com/playlist?list=PL08663519BF051CBE"
EXPECTED_TRACKS = 14


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def require_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise SystemExit(f"[bar-music] missing required tool: {name}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=DEFAULT_URL, help="YouTube playlist URL")
    parser.add_argument(
        "--force", action="store_true", help="replace WAV files already present"
    )
    args = parser.parse_args(argv)

    require_tool("yt-dlp")
    require_tool("ffmpeg")

    output_dir = repo_root() / "assets" / "music" / "original"
    output_dir.mkdir(parents=True, exist_ok=True)
    template = output_dir / "bar_music_%(playlist_index)02d.%(ext)s"

    command = [
        "yt-dlp",
        "--extract-audio",
        "--audio-format", "wav",
        "--audio-quality", "0",
        "--output", str(template),
        "--no-overwrites" if not args.force else "--force-overwrites",
        args.url,
    ]
    print(f"[bar-music] importing into {output_dir}")
    result = subprocess.run(command, cwd=repo_root(), check=False)
    if result.returncode:
        return result.returncode

    tracks = sorted(output_dir.glob("bar_music_[0-9][0-9].wav"))
    if len(tracks) != EXPECTED_TRACKS:
        print(
            f"[bar-music] warning: expected {EXPECTED_TRACKS} tracks, "
            f"found {len(tracks)}",
            file=sys.stderr,
        )
        return 1

    total_mb = sum(path.stat().st_size for path in tracks) / (1024 * 1024)
    print(f"[bar-music] ready: {len(tracks)} PCM WAVs ({total_mb:.1f} MiB, local-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

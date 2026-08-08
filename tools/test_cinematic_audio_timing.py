#!/usr/bin/env python3
"""Focused tests for generated MP3 duration measurement."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from cinematics.audio_timing import mp3_duration_seconds


class CinematicAudioTimingTests(unittest.TestCase):
    def test_counts_mpeg1_layer3_frames_after_id3_tag(self) -> None:
        # MPEG-1 Layer III, 128 kbps, 44.1 kHz, no padding: 417-byte frames.
        header = bytes.fromhex("FF FB 90 00")
        frame = header + bytes(417 - len(header))
        id3 = b"ID3\x04\x00\x00\x00\x00\x00\x00"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "voice.mp3"
            path.write_bytes(id3 + frame * 100)
            self.assertAlmostEqual(100 * 1152 / 44100,
                                   mp3_duration_seconds(path), places=5)

    def test_rejects_non_mp3_payload(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "noise.mp3"
            path.write_bytes(b"definitely not an mp3")
            with self.assertRaisesRegex(ValueError, "no MPEG"):
                mp3_duration_seconds(path)

    def test_m04_lines_and_cameras_cover_generated_voices(self) -> None:
        root = Path(__file__).resolve().parents[1]
        cinematic_dir = root / "assets" / "cinematics"
        data = json.loads(
            (cinematic_dir / "m04_customs_intercept.json").read_text(encoding="utf-8")
        )
        timeline = data["timeline"]
        lines = [cue for cue in timeline if cue["cmd"] == "line"]
        self.assertEqual(6, len(lines))
        for index, line in enumerate(lines):
            audio_duration = mp3_duration_seconds(cinematic_dir / line["voice_file"])
            self.assertGreaterEqual(line["dur"], audio_duration + 0.34)
            cameras = [
                cue for cue in timeline
                if cue["cmd"] == "camera_path" and cue["t"] == line["t"]
            ]
            self.assertEqual(1, len(cameras))
            self.assertEqual(line["dur"], cameras[0]["dur"])
            if index + 1 < len(lines):
                gap = lines[index + 1]["t"] - (line["t"] + line["dur"])
                self.assertGreaterEqual(gap, 0.249)


if __name__ == "__main__":
    unittest.main()

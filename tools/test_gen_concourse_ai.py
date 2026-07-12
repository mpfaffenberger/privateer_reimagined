#!/usr/bin/env python3
"""Dependency-free tests for gen_concourse_ai.py."""
from __future__ import annotations

import argparse
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import gen_concourse_ai as generator


class PromptTests(unittest.TestCase):
    def test_prompt_combines_room_archetype_and_optional_context(self) -> None:
        prompt = generator.compose_prompt("mining", "shipdealer", "Basque", "Militia")
        self.assertIn("dealership showroom", prompt)
        self.assertIn("Asteroid mining colony", prompt)
        self.assertIn("Basque", prompt)
        self.assertIn("Militia", prompt)
        self.assertIn("No text", prompt)

    def test_all_plan_is_cartesian_product(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            args = argparse.Namespace(
                archetype="all", room="all", location="", faction="",
                output=Path(tmp),
            )
            jobs = generator.plan_jobs(args)
        self.assertEqual(len(jobs), len(generator.ARCHETYPES) * len(generator.ROOMS))
        self.assertEqual(len({(job.archetype, job.room) for job in jobs}), len(jobs))

    def test_single_plan_uses_staging_and_install_roots(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            args = argparse.Namespace(
                archetype="pirate", room="bar", location="", faction="",
                output=Path(tmp),
            )
            job = generator.plan_jobs(args)[0]
            self.assertEqual(Path(job.staged_path), Path(tmp) / "pirate" / "bar_bg.png")
            self.assertEqual(
                Path(job.install_path),
                generator.ASSET_ROOT / "pirate" / "bar_bg.png",
            )


class CredentialTests(unittest.TestCase):
    def test_environment_key_takes_precedence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            key_file = Path(tmp) / ".openai_api_key"
            key_file.write_text("file-key\n", encoding="utf-8")
            with patch.dict("os.environ", {"OPENAI_API_KEY": " env-key "}):
                self.assertEqual(generator.load_api_key(key_file), "env-key")

    def test_key_file_is_trimmed_when_environment_is_unset(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            key_file = Path(tmp) / ".openai_api_key"
            key_file.write_text("  file-key\r\n", encoding="utf-8")
            with patch.dict("os.environ", {}, clear=True):
                self.assertEqual(generator.load_api_key(key_file), "file-key")

    def test_missing_key_returns_empty_string(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with patch.dict("os.environ", {}, clear=True):
                self.assertEqual(generator.load_api_key(Path(tmp) / "missing"), "")


class InstallTests(unittest.TestCase):
    def test_install_backs_up_existing_destination(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old_asset_root = generator.ASSET_ROOT
            try:
                generator.ASSET_ROOT = root / "assets"
                source = root / "fresh.png"
                destination = generator.ASSET_ROOT / "mining" / "bar_bg.png"
                backup_root = root / "backups"
                source.write_bytes(b"fresh")
                destination.parent.mkdir(parents=True)
                destination.write_bytes(b"ancient")

                generator.backup_and_install(source, destination, backup_root)

                self.assertEqual(destination.read_bytes(), b"fresh")
                self.assertEqual(
                    (backup_root / "mining" / "bar_bg.png").read_bytes(),
                    b"ancient",
                )
            finally:
                generator.ASSET_ROOT = old_asset_root


if __name__ == "__main__":
    unittest.main()

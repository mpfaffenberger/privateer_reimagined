"""Regression tests for optional/private mission template generation."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("gen_mission_templates.py")
MARKERS = (
    "k_corp", "k_enmy", "k_ptrl_summary", "k_scou_summary",
    "k_dfnd_summary", "k_crgo_summary", "k_atak_summary", "k_bnty_summary",
)


class MissionTemplateGeneratorTests(unittest.TestCase):
    def run_generator(self, source: Path, output: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(source), str(output)],
            text=True, capture_output=True, check=False,
        )

    def test_empty_optional_catalog_preserves_valid_fallback(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, output = root / "mission_text.json", root / "fallback.h"
            source.write_text("{}", encoding="utf-8")
            original = "\n".join(MARKERS)
            output.write_text(original, encoding="utf-8")
            result = self.run_generator(source, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(output.read_text(encoding="utf-8"), original)
            self.assertIn("preserving fallback", result.stdout)

    def test_empty_catalog_without_fallback_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, output = root / "mission_text.json", root / "missing.h"
            source.write_text("{}", encoding="utf-8")
            result = self.run_generator(source, output)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())

    def test_complete_catalog_generates_header(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, output = root / "mission_text.json", root / "generated.h"
            types = {
                "ENMY": {"names": ["enemy"]},
                "CORP": {"names": ["corp"]},
            }
            for key in ("PTRL", "SCOU", "DFND", "CRGO", "ATAK", "BNTY"):
                types[key] = {"summary": key, "copy": [f"{key} copy"]}
            source.write_text(json.dumps({"types": types}), encoding="utf-8")
            result = self.run_generator(source, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            generated = output.read_text(encoding="utf-8")
            for marker in MARKERS:
                self.assertIn(marker, generated)


if __name__ == "__main__":
    unittest.main()

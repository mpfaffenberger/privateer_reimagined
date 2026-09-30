"""Network-free tests for batch landing composite generation."""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import batch_generate_landing_ships as batch


class LandingBatchTests(unittest.TestCase):
    def test_screenshot_reference_recipe(self):
        self.assertEqual(batch.DEFAULT_REFERENCES, {
            "placement_guide": True,
            "background_installed": True,
            "background_original": False,
            "background_latest": False,
            "ship_pose": True,
            "ship_design": True,
            "composite_installed": True,
            "composite_latest": False,
        })

    def test_discovers_cartesian_product(self):
        pairs = batch.discover_pairs({"mining"}, {"centurion", "talon"})
        self.assertEqual(pairs, [
            batch.Pair("mining", "centurion"),
            batch.Pair("mining", "talon"),
        ])

    def test_request_matches_f11_generation(self):
        request = batch.request_for(batch.Pair("mining", "centurion"))
        self.assertEqual(request["target_kind"], "landing_ship")
        self.assertEqual(request["model"], "gpt-image-2")
        self.assertEqual(request["references"], batch.DEFAULT_REFERENCES)
        self.assertIn("centurion", request["prompt"])
        self.assertIn("mining", request["prompt"])

    def test_repaint_drops_the_old_composite_reference(self):
        request = batch.request_for(batch.Pair("agricultural", "tarsus"), repaint=True)
        self.assertFalse(request["references"]["composite_installed"])
        self.assertEqual({k: v for k, v in request["references"].items()
                          if k != "composite_installed"},
                         {k: v for k, v in batch.DEFAULT_REFERENCES.items()
                          if k != "composite_installed"})

    def test_batch_installs_and_records_progress(self):
        pairs = [batch.Pair("mining", "centurion"), batch.Pair("mining", "talon")]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / "manifest.json"
            with mock.patch.object(batch.studio, "safe_pair_target",
                                   side_effect=lambda a, s: root / f"{a}-{s}.png"), \
                 mock.patch.object(batch.studio, "run",
                                   side_effect=lambda request: {
                                       "ok": True,
                                       "preview_path": str(root / (request["ship"] + ".png")),
                                       "message": request["action"] + " complete",
                                   }):
                code = batch.run_batch(pairs, workers=2, install=True,
                                       force=False, manifest=manifest)
            self.assertEqual(code, 0)
            results = batch.load_results(manifest)
            self.assertEqual(set(results), {"mining/centurion", "mining/talon"})
            self.assertTrue(all(item.status == "complete" for item in results.values()))

    def test_existing_install_is_resumably_skipped(self):
        pair = batch.Pair("mining", "centurion")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            installed = root / "installed.png"
            installed.write_bytes(b"done")
            with mock.patch.object(batch.studio, "safe_pair_target", return_value=installed), \
                 mock.patch.object(batch.studio, "run") as run:
                code = batch.run_batch([pair], 1, True, False, root / "manifest.json")
            self.assertEqual(code, 0)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()

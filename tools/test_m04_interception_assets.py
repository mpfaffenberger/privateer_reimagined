#!/usr/bin/env python3
"""Regression checks for the M04 customs-interception art and fleet."""

from __future__ import annotations

import json
import math
import unittest
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
CIN_DIR = ROOT / "assets" / "cinematics"
CID = "studio_req_1785636336"
EXPECTED_FLEET = {
    "paradigm_lead": "paradigm",
    "broadsword_port": "broadsword",
    "broadsword_center": "broadsword",
    "broadsword_starboard": "broadsword",
}
RENDERED_RADIUS_M = {"paradigm": 315.0, "broadsword": 77.0}


class M04InterceptionAssetsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.data = json.loads((CIN_DIR / f"{CID}.json").read_text(encoding="utf-8"))
        cls.timeline = cls.data["timeline"]

    def test_cinematic_and_outcome_fleets_match_rebalanced_contract(self) -> None:
        spawns = {
            cue["actor"]: cue
            for cue in self.timeline
            if cue["cmd"] == "spawn" and cue["actor"] != "grayson_ship"
        }
        self.assertEqual(EXPECTED_FLEET, {
            actor: cue["class"] for actor, cue in spawns.items()
        })
        self.assertEqual(
            [("paradigm", 1, True), ("broadsword", 3, True)],
            [(item["class"], item["count"], item["hostile"])
             for item in self.data["outcome"]["spawns"]],
        )

    def test_confed_formation_has_generous_physical_clearance(self) -> None:
        spawns = {
            cue["actor"]: cue
            for cue in self.timeline
            if cue["cmd"] == "spawn" and cue["actor"] in EXPECTED_FLEET
        }
        actors = list(spawns)
        for index, left in enumerate(actors):
            for right in actors[index + 1:]:
                a = spawns[left]["pos"]
                b = spawns[right]["pos"]
                distance = math.dist(a, b)
                required = 3.0 * (
                    RENDERED_RADIUS_M[spawns[left]["class"]]
                    + RENDERED_RADIUS_M[spawns[right]["class"]]
                )
                self.assertGreater(distance, required, f"{left} too close to {right}")

    def test_rourke_portraits_are_helmeted_asset_set_shape(self) -> None:
        paths = [CIN_DIR / "portraits" / "rourke" / "_ref.png"]
        paths.extend(
            CIN_DIR / "portraits" / "rourke" / f"{CID}_{index:02}.png"
            for index in range(1, 4)
        )
        for path in paths:
            with Image.open(path) as portrait:
                self.assertEqual((512, 640), portrait.size)
                self.assertEqual("RGBA", portrait.mode)
        characters = json.loads(
            (ROOT / "assets" / "data" / "characters.json").read_text(encoding="utf-8")
        )
        self.assertIn("helmet", characters["characters"]["rourke"]["wardrobe"].lower())


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Regression contract for Tayla's fourth-job Riordian ambush."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
CID = "m05_riordian_grudge"


class RiordianAmbushTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cinematic = json.loads(
            (ASSETS / "cinematics" / f"{CID}.json").read_text(encoding="utf-8")
        )
        cls.triggers = json.loads(
            (ASSETS / "cinematics" / "triggers.json").read_text(encoding="utf-8")
        )["triggers"]
        cls.scenarios = json.loads(
            (ASSETS / "data" / "scripted_encounters.json").read_text(encoding="utf-8")
        )["scenarios"]

    def test_cinematic_is_sole_m05_riordian_owner(self) -> None:
        obsolete = {"m05_riordian_oakham_ambush", "m05_riordian_jump_reambush"}
        self.assertTrue(obsolete.isdisjoint(item.get("id") for item in self.scenarios))
        for scenario in self.scenarios:
            flags = scenario.get("trigger", {}).get("requires_flags", [])
            unique = [
                spawn.get("unique")
                for wave in scenario.get("waves", [])
                for spawn in wave.get("spawns", [])
            ]
            self.assertFalse("m05_active" in flags and "riordian" in unique)

    def test_trigger_fires_only_at_pentonville_119ce_jump(self) -> None:
        matches = [item for item in self.triggers if item["cinematic"] == CID]
        self.assertEqual(1, len(matches))
        when = matches[0]["when"]
        self.assertEqual("pentonville", when["system"])
        self.assertEqual({"nav": "119CE Jump", "radius_m": 20000}, when["near_nav"])
        self.assertEqual(["m05_active"], when["requires_flags"])
        self.assertEqual([f"{CID}_seen"], when["forbids_flags"])
        self.assertNotIn("killed:riordian", when["forbids_flags"])
        self.assertEqual([{"commodity": "brilliance", "min_units": 20}], when["cargo"])

    def test_outcome_places_riordian_and_three_talons_at_119ce(self) -> None:
        self.assertEqual(
            {"system": "pentonville", "nav": "119CE Jump"},
            self.cinematic["location"],
        )
        self.assertEqual("119CE Jump", self.cinematic["outcome"]["player_at_nav"])
        self.assertEqual(
            [
                ("centurion", 1, True, "killed:riordian"),
                ("talon", 3, True, None),
            ],
            [
                (item["class"], item["count"], item["hostile"],
                 item.get("cleared_flag"))
                for item in self.cinematic["outcome"]["spawns"]
            ],
        )


if __name__ == "__main__":
    unittest.main()

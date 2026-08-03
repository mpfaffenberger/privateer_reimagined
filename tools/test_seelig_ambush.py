#!/usr/bin/env python3
"""Regression contract for Roman Lynch's Captain Seelig dismissal ambush."""

from __future__ import annotations

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
CID = "studio_req_1785724793"


class SeeligAmbushTests(unittest.TestCase):
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

    def test_trigger_is_scoped_to_first_lynch_mission_at_119ce(self) -> None:
        matches = [item for item in self.triggers if item["cinematic"] == CID]
        self.assertEqual(1, len(matches))
        when = matches[0]["when"]
        self.assertEqual("pentonville", when["system"])
        self.assertEqual({"nav": "119CE Jump", "radius_m": 20000}, when["near_nav"])
        self.assertEqual(["m06_active"], when["requires_flags"])
        self.assertEqual(
            ["m06_message_delivered", "killed:seelig", f"{CID}_seen"],
            when["forbids_flags"],
        )

    def test_outcome_starts_single_hostile_seelig_fight(self) -> None:
        self.assertEqual(
            {"system": "pentonville", "nav": "119CE Jump"},
            self.cinematic["location"],
        )
        self.assertEqual("119CE Jump", self.cinematic["outcome"]["player_at_nav"])
        self.assertEqual(
            [{
                "class": "talon",
                "faction": "pirate",
                "count": 1,
                "hostile": True,
                "cleared_flag": "killed:seelig",
            }],
            self.cinematic["outcome"]["spawns"],
        )

    def test_cinematic_supersedes_legacy_nav_three_dialogue(self) -> None:
        legacy = next(item for item in self.scenarios if item["id"] == "m06_seelig_delivery")
        self.assertIn("m06_message_delivered", legacy["trigger"]["forbids_flags"])
        end = next(cue for cue in self.cinematic["timeline"] if cue["cmd"] == "end")
        self.assertIn("set_flag:m06_message_delivered", end["actions"])
        self.assertIn(f"set_flag:{CID}_seen", end["actions"])

    def test_player_ship_dialogue_and_media_contract(self) -> None:
        timeline = self.cinematic["timeline"]
        player = next(cue for cue in timeline
                      if cue["cmd"] == "spawn" and cue["actor"] == "grayson_ship")
        self.assertEqual("$player", player["class"])
        lines = [cue for cue in timeline if cue["cmd"] == "line"]
        self.assertEqual(4, len(lines))
        self.assertIn("I am profoundly disappointed in you", lines[1]["text"])
        self.assertTrue(lines[-1]["text"].endswith(
            "Oh well... guess I'm gonna have to blast one more idiot..."
        ))
        for line in lines:
            for field in ("portrait", "voice_file"):
                path = ASSETS / "cinematics" / line[field]
                self.assertTrue(path.is_file() and path.stat().st_size > 0, path)


if __name__ == "__main__":
    unittest.main()

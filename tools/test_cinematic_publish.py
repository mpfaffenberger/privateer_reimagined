#!/usr/bin/env python3
"""Failure-path tests for tools/cinematics/publish.py (#368).

Run from tools/:  python3 -m unittest test_cinematic_publish
"""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from cinematics.publish import PublishError, publish

EXISTING = {"cinematic": "old_scene", "once": True,
            "when": {"system": "troy", "requires_flags": ["a"]}}


class FakeCinematic:
    def __init__(self, cid: str) -> None:
        self.id = cid

    def to_dict(self) -> dict:
        return {"id": self.id, "cues": []}


def ok(_path):
    return []


def bad(_path):
    return [{"severity": "error", "cue_index": 0, "message": "broken"}]


def trigger(cid: str, system: str = "perry") -> dict:
    return {"cinematic": cid, "once": True, "when": {"system": system}}


class PublishTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.triggers = self.dir / "triggers.json"
        self.triggers.write_text(json.dumps({"triggers": [EXISTING]}), encoding="utf-8")
        self.original = self.triggers.read_text(encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def assertUntouched(self, cid: str) -> None:
        self.assertFalse((self.dir / f"{cid}.json").exists(), "orphan cinematic left behind")
        self.assertEqual(self.triggers.read_text(encoding="utf-8"), self.original)
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ["triggers.json"],
                         "staged files left behind")

    def test_success_writes_both(self) -> None:
        out = publish(FakeCinematic("new_scene"), trigger("new_scene"),
                      cin_dir=self.dir, validator=ok)
        self.assertEqual(json.loads(out.read_text())["id"], "new_scene")
        names = [t["cinematic"] for t in json.loads(self.triggers.read_text())["triggers"]]
        self.assertEqual(names, ["old_scene", "new_scene"])

    def test_validation_failure_writes_nothing(self) -> None:
        with self.assertRaises(PublishError):
            publish(FakeCinematic("new_scene"), trigger("new_scene"),
                    cin_dir=self.dir, validator=bad)
        self.assertUntouched("new_scene")

    def test_duplicate_cinematic_trigger_writes_nothing(self) -> None:
        with self.assertRaises(PublishError):
            publish(FakeCinematic("old_scene_2"), trigger("old_scene"),
                    cin_dir=self.dir, validator=ok)
        with self.assertRaises(PublishError):
            publish(FakeCinematic("old_scene"), trigger("old_scene"),
                    cin_dir=self.dir, validator=ok)
        self.assertUntouched("old_scene")

    def test_duplicate_conditions_write_nothing(self) -> None:
        dup = {"cinematic": "new_scene", "when": dict(EXISTING["when"])}
        with self.assertRaises(PublishError):
            publish(FakeCinematic("new_scene"), dup, cin_dir=self.dir, validator=ok)
        self.assertUntouched("new_scene")

    def test_malformed_triggers_file_writes_nothing(self) -> None:
        self.triggers.write_text('{"not_triggers": []}', encoding="utf-8")
        self.original = self.triggers.read_text(encoding="utf-8")
        with self.assertRaises(PublishError):
            publish(FakeCinematic("new_scene"), trigger("new_scene"),
                    cin_dir=self.dir, validator=ok)
        self.assertUntouched("new_scene")

    def test_existing_id_is_refused(self) -> None:
        (self.dir / "new_scene.json").write_text("{}", encoding="utf-8")
        with self.assertRaises(PublishError):
            publish(FakeCinematic("new_scene"), trigger("new_scene"),
                    cin_dir=self.dir, validator=ok)
        self.assertEqual(self.triggers.read_text(encoding="utf-8"), self.original)

    def test_triggers_swap_failure_rolls_back_cinematic(self) -> None:
        real_replace = os.replace

        def flaky(src, dst):
            if Path(dst).name == "triggers.json":
                raise OSError("disk full")
            return real_replace(src, dst)

        with mock.patch("cinematics.publish.os.replace", side_effect=flaky):
            with self.assertRaises(OSError):
                publish(FakeCinematic("new_scene"), trigger("new_scene"),
                        cin_dir=self.dir, validator=ok)
        self.assertUntouched("new_scene")

    def test_no_trigger_writes_only_cinematic(self) -> None:
        publish(FakeCinematic("solo"), None, cin_dir=self.dir, validator=ok)
        self.assertTrue((self.dir / "solo.json").exists())
        self.assertEqual(self.triggers.read_text(encoding="utf-8"), self.original)


if __name__ == "__main__":
    unittest.main()

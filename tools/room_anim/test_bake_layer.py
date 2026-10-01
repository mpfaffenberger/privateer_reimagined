#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow", "numpy"]
# ///
"""Focused tests for bake_layer.write_sheet() atlas sizing (#615).

    uv run tools/room_anim/test_bake_layer.py
"""
import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

import bake_layer


def sprite(w, h, value=200):
    return np.full((h, w, 4), value, dtype=np.uint8)


def write(tmp, sprites):
    """write_sheet() a one-shot sheet -> (atlas RGBA, manifest)."""
    frames = [(img, [0, 0, img.shape[1], img.shape[0]]) for img in sprites]
    with contextlib.redirect_stdout(io.StringIO()):
        bake_layer.write_sheet(tmp, "t", frames, list(range(len(frames))), (2048, 1024),
                               24.0, len(frames), 0)
    return (np.asarray(Image.open(tmp / "t.png")),
            json.loads((tmp / "t.json").read_text()))


class WriteSheetWidthTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_narrow_sprites_keep_the_default_width(self):
        atlas, _ = write(self.tmp, [sprite(300, 40), sprite(500, 20)])
        self.assertEqual(bake_layer.ATLAS_W, atlas.shape[1])

    def test_sprite_wider_than_the_default_atlas_packs(self):
        img = sprite(1100, 347)
        atlas, manifest = write(self.tmp, [img])
        self.assertEqual((347, 2048, 4), atlas.shape)
        x, y, w, h = manifest["frames"][0]["src"]
        self.assertEqual((1100, 347), (w, h))
        np.testing.assert_array_equal(img, atlas[y:y + h, x:x + w])

    def test_sprite_needing_exactly_the_default_width_stays_narrow(self):
        atlas, _ = write(self.tmp, [sprite(bake_layer.ATLAS_W - bake_layer.PAD, 8)])
        self.assertEqual(bake_layer.ATLAS_W, atlas.shape[1])

    def test_sprite_wider_than_the_limit_is_a_clear_error(self):
        with self.assertRaisesRegex(SystemExit, "wide sprite"):
            write(self.tmp, [sprite(bake_layer.MAX_ATLAS_W, 8)])


if __name__ == "__main__":
    unittest.main()

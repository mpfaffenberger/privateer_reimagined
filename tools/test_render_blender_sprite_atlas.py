#!/usr/bin/env python3
"""Post-process checks for the Blender sprite-atlas capture (#699): raw
frames become centred cells, projected lights follow the crop into cell UV,
hidden lights are dropped, and the manifest lists every view.

    cd tools && uv run --with pillow python -m unittest test_render_blender_sprite_atlas
"""
from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image

import render_3d_sprite_atlases as engine_capture
import render_blender_sprite_atlas as capture

SHIP = "testship"


def _frame() -> Image.Image:
    """100x100 transparent frame with a 40x20 opaque hull at x 20..59, y 40..59."""
    im = Image.new("RGBA", (100, 100), (0, 0, 0, 0))
    im.paste((200, 200, 200, 255), (20, 40, 60, 60))
    return im


class PostTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.ships = Path(tmp.name)
        for module in (capture, engine_capture):
            patcher = mock.patch.object(module, "SHIPS_DIR", self.ships)
            patcher.start()
            self.addCleanup(patcher.stop)
        self.raw = self.ships / SHIP / "sprites_3d" / "raw"
        self.raw.mkdir(parents=True)
        lights = [
            {"pos": [0, 0, 0], "normal": [0, 0, -1], "color": [51, 140, 255], "size": 15, "kind": "steady", "hz": 0},
            {"pos": [0, 0, 0], "normal": [0, 0, 1], "color": [255, 26, 26], "size": 10, "kind": "steady", "hz": 0},
        ]
        self.lights = Path(tmp.name) / "ship.lights3d.json"
        self.lights.write_text(json.dumps({"lights": lights}))
        projections = {}
        for az, el in capture.ALL_VIEWS:
            cell = capture.cell_name(SHIP, az, el)
            _frame().save(self.raw / cell)
            projections[cell] = [{"px": 40, "py": 50, "visible": True},   # hull centre
                                 {"px": 20, "py": 40, "visible": False}]  # hidden
        (self.raw / "projections.json").write_text(json.dumps(projections))

    def test_cells_lights_and_manifest(self) -> None:
        n_ok, n_lit = capture.post(SHIP, "testmesh", self.lights, self.raw, 512)
        self.assertEqual((len(capture.ALL_VIEWS), len(capture.ALL_VIEWS)), (n_ok, n_lit))

        cell = self.ships / SHIP / "sprites_3d" / capture.cell_name(SHIP, 90.0, 0.0)
        with Image.open(cell) as im:
            self.assertEqual((512, 512), im.size)
            x0, y0, x1, y1 = im.getbbox()
        self.assertAlmostEqual(256, (x0 + x1) / 2, delta=1)   # hull centred
        self.assertAlmostEqual(256, (y0 + y1) / 2, delta=1)

        spots = json.loads(cell.with_name(cell.stem + ".lights.json").read_text())
        self.assertEqual(1, len(spots))                       # hidden light dropped
        self.assertAlmostEqual(0.5, spots[0]["u"], places=2)  # followed the crop
        self.assertAlmostEqual(0.5, spots[0]["v"], places=2)
        self.assertEqual([51, 140, 255], spots[0]["color"])
        self.assertEqual(15, spots[0]["size"])

        manifest = json.loads((self.ships / SHIP / "atlas_manifest_3d.json").read_text())
        self.assertEqual("testmesh", manifest["mesh_codename"])
        self.assertEqual(len(capture.ALL_VIEWS), len(manifest["samples"]))

    def test_no_lights_file_writes_no_sidecars(self) -> None:
        n_ok, n_lit = capture.post(SHIP, "testmesh", None, self.raw, 128)
        self.assertEqual((len(capture.ALL_VIEWS), 0), (n_ok, n_lit))
        self.assertEqual([], list((self.ships / SHIP / "sprites_3d").glob("*.lights.json")))


class ViewTests(unittest.TestCase):
    def test_positive_elevation_cameras_sit_above_the_hull(self) -> None:
        # Engine picker: el > 0 = camera above (+Y). #720 shipped belly views
        # in the el+ cells because the poses inherited the engine capture's
        # negation.
        for view, (az, el) in zip(capture.views(SHIP), capture.ALL_VIEWS):
            y = view["cam"][1]
            if el > 0:
                self.assertGreater(y, 0, view["file"])
            elif el < 0:
                self.assertLess(y, 0, view["file"])
            else:
                self.assertAlmostEqual(0, y, places=6)


if __name__ == "__main__":
    unittest.main()

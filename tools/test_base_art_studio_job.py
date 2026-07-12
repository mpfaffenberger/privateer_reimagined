"""Dependency-free network-free tests for the Base Art Studio worker."""
import tempfile, unittest
from pathlib import Path
from unittest import mock
import base_art_studio_job as job

PNG = b"\x89PNG\r\n\x1a\nmock"

class StudioJobTests(unittest.TestCase):
    def test_path_validation(self):
        with self.assertRaises(ValueError): job.safe_target("../bad", "bar")
        with self.assertRaises(ValueError): job.safe_target("mining", "unknown")
        with self.assertRaises(ValueError): job.safe_pair_target("mining", "../talon")

    def test_atomic_write(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "nested" / "x.png"; job.atomic_bytes(path, PNG)
            self.assertEqual(path.read_bytes(), PNG)

    def test_reference_modes(self):
        a, b = Path("a"), Path("b")
        self.assertEqual(job.references("text", a, b), [])
        self.assertEqual(job.references("both", a, b), [a, b])
        with self.assertRaises(ValueError): job.references("bogus", a, b)

    def test_generate_text_atomic_result(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(job, "load_api_key", return_value="secret"), \
             mock.patch.object(job, "generate_png", return_value=PNG), \
             mock.patch.object(job, "stage_path", return_value=Path(d) / "latest.png"):
            result = job.run({"action":"generate", "archetype":"mining", "room":"bar",
                              "prompt":"new bar", "reference_mode":"text"})
            self.assertTrue(result["ok"])
            self.assertEqual((Path(d) / "latest.png").read_bytes(), PNG)

    def test_install_and_revert_room(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); target = root / "assets" / "bar.png"; latest = root / "latest.png"
            target.parent.mkdir(); target.write_bytes(b"old"); latest.write_bytes(b"new")
            with mock.patch.object(job, "safe_target", return_value=target), \
                 mock.patch.object(job, "stage_path", return_value=latest), \
                 mock.patch.object(job, "BACKUP_ROOT", root / "backups"):
                self.assertTrue(job.run({"action":"install", "archetype":"mining", "room":"bar"})["ok"])
                self.assertEqual(target.read_bytes(), b"new")
                self.assertTrue(job.run({"action":"revert", "archetype":"mining", "room":"bar"})["ok"])
                self.assertEqual(target.read_bytes(), b"old")

    def test_missing_links_uses_default_pose(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(job, "ASSET_ROOT", Path(d)):
            self.assertEqual(job.ship_pose("pirate", "talon"),
                             {"az":35.0, "el":-20.0, "rect":[0.34, 0.42, 0.32, 0.34]})

    def test_posed_reference_uses_nearest_spherical_sample(self):
        sprite, pose = job.posed_ship_reference("mining", "centurion")
        self.assertTrue(sprite.is_file())
        self.assertEqual(pose["az"], 65.0)
        self.assertIn("az067p5_el+030", sprite.name)

    def test_placement_guide_matches_background_size(self):
        try: from PIL import Image
        except ImportError: self.skipTest("Pillow unavailable")
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); bg, ship, output = root/"bg.png", root/"ship.png", root/"guide.png"
            Image.new("RGB", (200, 100), "navy").save(bg)
            Image.new("RGBA", (20, 20), (255, 0, 0, 255)).save(ship)
            job.make_placement_guide(bg, ship, {"rect":[.25, .25, .5, .5]}, output)
            with Image.open(output) as made:
                self.assertEqual(made.size, (200, 100))
                self.assertEqual(made.getpixel((100, 50)), (255, 0, 0))

    def test_pair_install_creates_then_backs_up(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); target, latest = root/"installed.png", root/"latest.png"
            latest.write_bytes(b"first")
            patches = (mock.patch.object(job, "safe_pair_target", return_value=target),
                       mock.patch.object(job, "pair_stage_path", return_value=latest),
                       mock.patch.object(job, "BACKUP_ROOT", root/"backups"))
            with patches[0], patches[1], patches[2]:
                first = job.run({"target_kind":"landing_ship", "action":"install",
                                 "archetype":"mining", "ship":"talon"})
                self.assertEqual(first["backup_path"], "")
                latest.write_bytes(b"second")
                second = job.run({"target_kind":"landing_ship", "action":"install",
                                  "archetype":"mining", "ship":"talon"})
                self.assertTrue(Path(second["backup_path"]).is_file())
                self.assertEqual(Path(second["backup_path"]).read_bytes(), b"first")
                self.assertEqual(target.read_bytes(), b"second")

if __name__ == "__main__": unittest.main()

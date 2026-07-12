"""Dependency-free tests for the Base Art Studio worker."""
import json, tempfile, unittest
from pathlib import Path
from unittest import mock
import base_art_studio_job as job

PNG = b"\x89PNG\r\n\x1a\nmock"

class StudioJobTests(unittest.TestCase):
    def test_path_validation(self):
        with self.assertRaises(ValueError): job.safe_target("../bad", "bar")
        with self.assertRaises(ValueError): job.safe_target("mining", "unknown")

    def test_atomic_write(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "nested" / "x.png"; job.atomic_bytes(p, PNG)
            self.assertEqual(p.read_bytes(), PNG)

    def test_reference_modes(self):
        a, b = Path("a"), Path("b")
        self.assertEqual(job.references("text", a, b), [])
        self.assertEqual(job.references("both", a, b), [a, b])
        with self.assertRaises(ValueError): job.references("bogus", a, b)

    def test_generate_text_atomic_result(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(job, "load_api_key", return_value="secret"), \
             mock.patch.object(job, "generate_png", return_value=PNG), \
             mock.patch.object(job, "stage_path", return_value=Path(d)/"latest.png"):
            result = job.run({"action":"generate", "archetype":"mining", "room":"bar", "prompt":"new bar", "reference_mode":"text"})
            self.assertTrue(result["ok"]); self.assertEqual((Path(d)/"latest.png").read_bytes(), PNG)

    def test_install_and_revert(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); target=root/"assets"/"bar.png"; latest=root/"latest.png"; backups=root/"backups"
            target.parent.mkdir(); target.write_bytes(b"old"); latest.write_bytes(b"new")
            with mock.patch.object(job, "safe_target", return_value=target), mock.patch.object(job, "stage_path", return_value=latest), mock.patch.object(job, "BACKUP_ROOT", backups):
                self.assertTrue(job.run({"action":"install","archetype":"mining","room":"bar"})["ok"])
                self.assertEqual(target.read_bytes(), b"new")
                self.assertTrue(job.run({"action":"revert","archetype":"mining","room":"bar"})["ok"])
                self.assertEqual(target.read_bytes(), b"old")

if __name__ == "__main__": unittest.main()

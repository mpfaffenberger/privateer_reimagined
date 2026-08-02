#!/usr/bin/env python3
"""Regression checks for native-detail smooth sprite generation mode."""

from __future__ import annotations

import io
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image

from pixelart import generate_sprite as generator


def _gradient_png() -> bytes:
    image = Image.new("RGBA", (16, 12))
    pixels = image.load()
    for y in range(image.height):
        for x in range(image.width):
            pixels[x, y] = (20 + x * 8, 30 + y * 9, 80 + x + y, 255)
    payload = io.BytesIO()
    image.save(payload, "PNG")
    return payload.getvalue()


class SmoothSpriteGenerationTests(unittest.TestCase):
    def test_smooth_mode_preserves_native_detail_and_dimensions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "smooth.png"
            with mock.patch.object(generator, "_call_openai_image", return_value=_gradient_png()):
                result = generator.generate_sprite(
                    subject="a smooth plated station",
                    output_path=str(output),
                    pixel_grid=8,
                    render_size=8,
                    transparent_bg=True,
                    size="1024x1024",
                    api_key="test-key",
                    render_style="smooth",
                )

            self.assertEqual([16, 12], result["final_dimensions"])
            self.assertIsNone(result["pixel_grid_dimensions"])
            self.assertEqual("smooth", result["render_style"])
            self.assertIn("smooth gradients", result["prompt"])
            self.assertIn("no pixel art", result["prompt"])
            with Image.open(output) as generated:
                self.assertEqual((16, 12), generated.size)

    def test_unknown_render_style_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "render_style"):
            generator.generate_sprite(
                subject="station",
                api_key="test-key",
                render_style="oil-on-toast",
            )


if __name__ == "__main__":
    unittest.main()

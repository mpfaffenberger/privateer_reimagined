#!/usr/bin/env python3
"""Focused regression checks for edge-safe base exterior upscaling."""

from __future__ import annotations

import unittest

from PIL import Image, ImageDraw

from upscale_base_exteriors import alpha_stats, upscale_image


class UpscaleBaseExteriorsTests(unittest.TestCase):
    def test_upscale_adds_soft_alpha_without_dark_edge(self) -> None:
        source = Image.new("RGBA", (24, 20), (0, 0, 0, 0))
        draw = ImageDraw.Draw(source)
        draw.polygon(((4, 16), (12, 2), (20, 16)), fill=(180, 110, 55, 255))

        output = upscale_image(source, scale=2)
        stats = alpha_stats(output)

        self.assertEqual((48, 40), output.size)
        self.assertGreater(stats.partial, 0)
        self.assertGreater(stats.transparent, 0)
        self.assertGreater(stats.opaque, 0)

        rgba = output.tobytes()
        partial_colors = [
            (rgba[index], rgba[index + 1], rgba[index + 2])
            for index in range(0, len(rgba), 4)
            if 32 <= rgba[index + 3] <= 223
        ]
        self.assertTrue(partial_colors)
        # Premultiplied reconstruction should preserve the warm hull color at
        # the edge instead of blending it toward transparent black.
        self.assertGreater(min(r for r, _, _ in partial_colors), 120)
        self.assertGreater(min(g for _, g, _ in partial_colors), 70)

    def test_rejects_already_antialiased_canonical_input(self) -> None:
        source = Image.new("RGBA", (4, 4), (10, 20, 30, 128))
        with self.assertRaisesRegex(ValueError, "binary alpha"):
            upscale_image(source)

    def test_rejects_non_upscale_factor(self) -> None:
        source = Image.new("RGBA", (4, 4), (10, 20, 30, 255))
        with self.assertRaisesRegex(ValueError, "at least 2"):
            upscale_image(source, scale=1)


if __name__ == "__main__":
    unittest.main()

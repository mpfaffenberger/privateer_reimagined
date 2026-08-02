#!/usr/bin/env python3
"""Focused tests for generated base exterior display-size policy."""

from __future__ import annotations

import unittest

from base_exterior_policy import display_length_meters
from normalize_base_exterior_sizes import normalize_text


class BaseExteriorPolicyTests(unittest.TestCase):
    def test_station_and_planet_sizes(self) -> None:
        self.assertEqual(1200, display_length_meters("sprites/base_refinery"))
        self.assertEqual(1200, display_length_meters("sprites/base_perry"))
        self.assertEqual(1600, display_length_meters("sprites/base_new_detroit"))
        self.assertEqual(1600, display_length_meters("sprites/base_oxford"))

    def test_unknown_sprite_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown"):
            display_length_meters("sprites/base_mystery_meat")

    def test_normalizer_preserves_json5_and_changes_only_length(self) -> None:
        source = '''{
          // comment intentionally makes this JSON5
          "placed_sprites": [{
            "sprite": "sprites/base_refinery",
            "position": [1, 2, 3],
            "length_meters": 3000
          }]
        }
        '''
        expected = source.replace('"length_meters": 3000', '"length_meters": 1200')
        normalized, count, errors = normalize_text(source)
        self.assertEqual(expected, normalized)
        self.assertEqual(1, count)
        self.assertEqual([], errors)


if __name__ == "__main__":
    unittest.main()

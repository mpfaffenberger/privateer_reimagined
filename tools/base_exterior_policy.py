#!/usr/bin/env python3
"""Canonical display sizes for generated base exterior billboards."""

from __future__ import annotations

STATION_LENGTH_METERS = 1200
PLANET_LENGTH_METERS = 1600
PLANET_SPRITES = frozenset(
    {
        "sprites/base_agricultural",
        "sprites/base_new_detroit",
        "sprites/base_oxford",
        "sprites/base_pleasure",
    }
)
STATION_SPRITES = frozenset(
    {
        "sprites/base_mining",
        "sprites/base_new_constantinople",
        "sprites/base_perry",
        "sprites/base_refinery",
    }
)
BASE_SPRITES = PLANET_SPRITES | STATION_SPRITES


def display_length_meters(sprite: str) -> int:
    """Return the authored billboard width for a generated base sprite."""
    if sprite in PLANET_SPRITES:
        return PLANET_LENGTH_METERS
    if sprite in STATION_SPRITES:
        return STATION_LENGTH_METERS
    raise ValueError(f"unknown generated base exterior: {sprite}")

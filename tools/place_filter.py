#!/usr/bin/env python3
"""Shared filter: drop comm lines that name a SPECIFIC location.

The generated comm corpus templated a {location} slot with both generic fills
("this sector", "this quadrant") and proper place names ("the Oxford run",
"the Troy system"). The static voice clips can't know which system the player
is actually in, so a Confed in Troy announcing "New Detroit" breaks immersion.

We keep the generic variants and drop only the proper-noun ones. Both bank
builders import is_location_specific() so the rule lives in exactly one place.
"""
import re

# Proper place names that appear in the corpus (systems/regions). "Confed" is a
# faction, not a place, so it stays.
PLACE_NAMES = ("Troy", "Oxford", "Palan", "Detroit", "Pleiades", "Humboldt")

_PAT = re.compile(r"\b(" + "|".join(PLACE_NAMES) + r")\b", re.IGNORECASE)


def is_location_specific(text: str) -> bool:
    """True if the line names a specific place (and should be excluded)."""
    return bool(_PAT.search(text or ""))

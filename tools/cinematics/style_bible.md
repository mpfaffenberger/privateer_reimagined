# Cinematic Portrait — Shared Style Bible

Every portrait generation call (both `gen-ref` and `gen-line`) prepends the
**style prefix** below. This is the single source of truth for the look; keep
all characters visually consistent by never diverging from it. The machine-
readable copy of this prefix lives in `portraits.py` (`STYLE_PREFIX`) and MUST
be kept in sync with this document.

## The one-look contract

- **Medium:** modern painted sci-fi — clean, high-fidelity digital painting.
  Think polished concept-art character portrait, NOT the chunky 90s VGA
  original and NOT photoreal, NOT anime.
- **Framing:** chest-up, 3/4 view (subject turned ~30° off-camera, face toward
  viewer). Head near the top third, shoulders filling the lower frame.
- **Aspect / size:** 4:5 portrait, delivered at exactly **512 x 640 px, RGBA**.
- **Lighting:** consistent cool key light from the upper left (cockpit-instrument
  glow), warm rim light from the lower right. Dark cockpit / ship-interior
  ambience. Moody but readable — the face is always clearly lit.
- **Background:** simple and non-distracting — a dark, softly-vignetted cockpit
  or hangar backdrop, or transparent. NO busy scenery, NO text, NO logos, NO
  frame/border (the engine draws the VDU frame itself).
- **Palette:** desaturated industrial base (gunmetal, charcoal, worn browns)
  with a single character-defining accent color carried from the reference.
- **Expression:** neutral-to-purposeful by default; `gen-line` overrides this
  with the per-line emotion/direction.

## Why a reference-first pipeline

Per-line generation makes **character consistency** the central problem. We do
NOT solve it with fresh text-to-image per line (identity drifts every frame).
Instead:

1. `gen-ref` paints ONE canonical headshot per character from the bible
   `appearance` + this style prefix. A human approves it once.
2. `gen-line` conditions on that approved `_ref.png` via an image-EDIT /
   reference endpoint, changing only expression/pose/lighting for the line.
   The reference holds the identity.

## Prompt assembly order (both commands)

```
STYLE_PREFIX
  + character.appearance
  + character.wardrobe
  + [gen-line only] "Expression/direction: <emotion>. They are saying: \"<line>\""
  + NEGATIVE (no text, no border, no watermark, no extra limbs, single subject)
```

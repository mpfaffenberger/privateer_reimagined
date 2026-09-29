# New Constantinople concourse + hangar animation pipeline (#515, #553)

The painted concourse (`assets/concourse/newcon/concourse_bg.png`, 1536x1024)
stays the hero plate. Animated layers are rendered in Blender against a
camera-matched proxy of the painting and baked into sprites that the engine
composites with plain alpha-over (`src/room_anim.{h,cpp}`).

```
render_layers.py (Blender) -> build/newcon_concourse/<layer>/   raw passes
bake_layer.py              -> assets/concourse/newcon/anim/<layer>.{png,json}
bake_sky.py                -> assets/concourse/newcon/anim/sky_*.png, stars_*.png
composite_preview.py       -> preview GIF/MP4/PNG, drawn exactly like the engine
```

## Rebuild everything

From the repo root (Blender 5.2, `uv`; a CUDA GPU makes it ~40 min):

```sh
blender --background --factory-startup \
    --python tools/newcon_concourse/render_layers.py -- --layer all
uv run tools/newcon_concourse/bake_layer.py --all
uv run tools/newcon_concourse/bake_sky.py
uv run tools/newcon_concourse/composite_preview.py --seconds 16 --out build/newcon_concourse/preview.mp4
```

`--review-blend` regenerates `newcon_concourse.blend` (the hall, occluders,
lights and every layer's actors, a collection each) for poking at the setup
in Blender. Renders never come from that file; the scripts are the source.

## Files

| file | runs in | what |
|---|---|---|
| `scene.py` | Blender | camera match, proxy decks, holdout occluders, lights |
| `actors.py` | Blender | procedural hover-car and pedestrian proxies + animation |
| `render_layers.py` | Blender | layer definitions; beauty / mask / empty passes |
| `bake_layer.py` | uv | plate-aware sprite encoding + atlas packing |
| `layers.json` | - | per-layer loop period and phase |
| `bake_sky.py` | uv | sky mask, starless sky fill, star tiles |
| `composite_preview.py` | uv | engine-faithful preview from `concourse.json` |

## How it fits the painting

**Camera.** The plate is one-point perspective with vertical verticals, so the
camera is level with lens shift. The red guide stripe and the kerb lamps meet
at the vanishing point (1230, 551); eye height 5 m, 28 mm on a 36 mm sensor.
Landmarks were measured as `(y - 551) / (1230 - x) = (5 - z) / -X`: stripe
X -21.6, kerb X -25, walkway top 0.9 m, shopfront base X -31 (see `scene.py`).

**Floor interaction without replacing paint.** Each frame renders the actor
over proxy decks (A), the decks alone (B) and the actor's coverage (m). The
bake builds the target in linear light: the actor where m says so, and on the
floor `P + (A - B)` where the actor adds light (reflections, glow) or
`P * A/B` where it removes it (shadows). The painted floor gets the
interaction; the proxy deck's own look never shows.

**No engine blend modes.** The plate under a sprite never changes, so any
target can be written as a straight-alpha colour over it with the minimum
alpha that keeps the colour in range. Plain ImGui alpha-over reproduces
additive reflections exactly. Full-res sprites use the same NEAREST sampler
as the plate, so each texel lands on the plate texel it was encoded against.

**Stars.** Not a Blender job: the sky is flat 2D behind a painted cutout. The
mask is hand-authored window polygons refined by a darkness threshold, with
bright-star holes filled by a flood from the floor. Stars are resampled from
the painting's own star population (density, brightness, colour), in two
slightly different drift speeds. The engine draws fill + tiles, then the
plate with the mask as alpha.

## Gotchas found the hard way

* A clipped channel turns a shadow into a colour: under a shopfront lamp the
  proxy walkway clipped red, so the walker's shadow only removed green and
  blue and printed red. Passes now render at -2 EV (`scene.EXPOSURE_EV`,
  recorded in `pass.json`) and the bake undoes it in linear light.
* 1-2 LSB render noise becomes a visible speck on near-black paint once it's
  converted to linear light, so changes under 3 LSB in the render are ignored.
* Off-screen frames are skipped, not rendered: the bake takes timeline slots
  from frame numbers in file names, so gaps stay blank.
* OptiX fails to compile on the dev box's driver; CUDA is used.

## The hangar (#553)

The landing pad isn't one painting: it shows one of 18 full-frame composites
(`landing_ships/<hull>.png`, one per player hull), each framed differently.
So nothing is camera-matched to a single plate. Instead:

```
bake_hangar.py              -> anim/hangar/<hull>_{mask,fill,stars_*}.png, anchors.json
render_hangar.py (Blender)  -> build/newcon_concourse/hangar/<layer>/  straight-alpha frames
bake_traffic.py             -> anim/hangar/<layer>_{under,over}.{png,json}
composite_preview.py --room landing --plate <hull>
```

```sh
uv run tools/newcon_concourse/bake_hangar.py --debug build/newcon_concourse/mouths.png
blender --background --factory-startup \
    --python tools/newcon_concourse/render_hangar.py -- --layer all
uv run tools/newcon_concourse/bake_traffic.py --all
uv run tools/newcon_concourse/composite_preview.py --room landing --plate tarsus \
    --seconds 30 --out build/newcon_concourse/hangar.mp4
```

**Finding the mouth.** Per composite: flood the darkest star-removed blob in
the upper middle, fit a circle to the left/right edges of its top 120 rows
(the top arc is clean everywhere; lower down hull shadow can join the flood,
as on the drayman), and keep only the flood inside that circle. The mask is
ramped by luminance near the threshold, so the painted light shaft's haze
fades into space instead of being cut off in a jagged line. The circle is the
composite's anchor `[cx, cy, r]`. The haze ramp measures after an 11 px
opening, so big painted stars aren't taken for haze and left frozen in view.

**Spinning stars.** Star layers take `"spin"` (deg/s, clockwise) about the
anchor, so the field turns around the mouth like a rotating station
(`room_anim_data.cpp star_uvs()`: one affine quad with a REPEAT sampler).
The paintings disagree (tarsus ~13 stars/10k px, drayman ~2), so every
composite gets its own tiles from its own painted stars. Sub-pixel tile stars
partly fall under the detector, so the bake builds each composite, measures
it like the paint, and corrects the density until they agree (within 10%;
floor 2/10k px so near-starless mouths still visibly spin). The few big stars
get a third "hero" tile of two-tone glows (core + blue halo tints), found by
brightest channel: luma barely counts blue, so vivid blue stars read as dim.
`--stars-only` re-makes just the tiles in about two minutes.

**Canonical mouth.** `render_hangar.py` renders against a mouth at
`ANCHOR = (768, 360, 200)` px, 220 m out (focal length exactly 1024 px). The
engine moves and scales each frame from that anchor onto the composite's
(`room_anim_data.cpp place()`), so one render fits all 18 framings.

**Under and over.** A ship beyond the mouth plane is drawn *under* the plate
(through the sky mask), so the tunnel rim and the parked ship occlude it. In
the tunnel it's drawn *over*. One render is split into two sheets on one
timeline, so there's no pop at the crossing (the ship is inside the open disc
there). In-tunnel paths stay above the parked hulls, whose tallest (drayman)
reaches cy - 0.23 r; `bake_traffic.py` refuses frames below cy - 0.25 r.

**Ships.** Real game meshes (`assets/meshes/ships_wcnews`), textured from
their `<stem>.materials.json` sidecars by `ships.py`; `ships.lookdev()`
renders marked axes to find each mesh's orientation fix.

| file | runs in | what |
|---|---|---|
| `bake_hangar.py` | uv | per-composite mouth mask, fill, anchor; hangar star tiles |
| `ships.py` | Blender | game-mesh import, sidecar materials, orientation |
| `render_hangar.py` | Blender | canonical camera, lights, flight paths, frames |
| `bake_traffic.py` | uv | trim, split at the mouth plane, pack |
| `hangar_layers.json` | - | per-layer loop period and phase |

## Adding a layer

1. Add a builder to `LAYERS` in `render_layers.py` (actors from `actors.py`).
2. Add its loop timing to `layers.json`.
3. Render, bake, and list `anim/<layer>.json` in the room's `"layers"` in
   `assets/concourse/newcon/concourse.json` (drawn in list order: far first).
4. Preview with `composite_preview.py`, then run `test_room_anim`.

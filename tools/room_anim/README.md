# Animated base rooms: the art pipeline (#515, #553, #557)

Each base's painted plate (`assets/concourse/<base>/concourse_bg.png`,
1536x1024) stays the hero image. Animated layers are rendered in Blender
against a camera-matched proxy of the painting and baked into sprites that
the engine composites with plain alpha-over (`src/room_anim.{h,cpp}`).

## Layout

Shared, base-agnostic machinery lives here; everything tied to one painting
lives in `<base>/`. `base.py` is the single source of truth for paths:
`assets/concourse/<base>/` (plates, `concourse.json`, baked `anim/`),
`build/room_anim/<base>/` (raw renders, never committed) and
`tools/room_anim/<base>/` (scripts and loop timing).

| file | runs in | what |
|---|---|---|
| `base.py` | both | per-base paths |
| `render.py` | Blender | beauty / mask / empty passes, render border |
| `ships.py` | Blender | game-mesh import, sidecar materials, orientation |
| `bake_layer.py` | uv | plate-aware sprite encoding + atlas packing (`--base`) |
| `sky.py` | uv | star removal, mask solidify, sky fill, painted-star stats, star tiles |
| `composite_preview.py` | uv | engine-faithful preview from `concourse.json` (`--base`) |
| `stage.py` | Blender | render settings, boxes, materials, lights, straight passes |
| `newcon/` | | New Constantinople: concourse (#515) and hangar (#553) |
| `mining/` | | Mining base concourse: the ore train (#558) |

## New Con concourse

```
newcon/render_layers.py (Blender) -> build/room_anim/newcon/<layer>/   raw passes
bake_layer.py --base newcon       -> assets/concourse/newcon/anim/<layer>.{png,json}
newcon/bake_sky.py                -> assets/concourse/newcon/anim/sky_*.png, stars_*.png
composite_preview.py              -> preview GIF/MP4/PNG, drawn exactly like the engine
```

From the repo root (Blender 5.2, `uv`; a CUDA GPU makes it ~40 min):

```sh
blender --background --factory-startup \
    --python tools/room_anim/newcon/render_layers.py -- --layer all
uv run tools/room_anim/bake_layer.py --base newcon --all
uv run tools/room_anim/newcon/bake_sky.py
uv run tools/room_anim/composite_preview.py --base newcon --seconds 16 \
    --out build/room_anim/newcon/preview.mp4
```

`--review-blend` regenerates `newcon/newcon_concourse.blend` (the hall,
occluders, lights and every layer's actors, a collection each) for poking at
the setup in Blender. Renders never come from that file; the scripts are the
source.

| `newcon/` file | runs in | what |
|---|---|---|
| `scene.py` | Blender | camera match, proxy decks, holdout occluders, lights |
| `actors.py` | Blender | procedural hover-car and pedestrian proxies + animation |
| `render_layers.py` | Blender | layer definitions |
| `layers.json` | - | per-layer loop period and phase |
| `bake_sky.py` | uv | window polygons -> sky mask, fill, star tiles |

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
render_hangar.py (Blender)  -> build/room_anim/newcon/hangar/<layer>/  straight-alpha frames
bake_traffic.py             -> anim/hangar/<layer>_{under,over}.{png,json}
composite_preview.py --room landing --plate <hull>
```

```sh
uv run tools/room_anim/newcon/bake_hangar.py --debug build/room_anim/newcon/mouths.png
blender --background --factory-startup \
    --python tools/room_anim/newcon/render_hangar.py -- --layer all
uv run tools/room_anim/newcon/bake_traffic.py --all
uv run tools/room_anim/composite_preview.py --base newcon --room landing --plate tarsus \
    --seconds 30 --out build/room_anim/newcon/hangar.mp4
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

| `newcon/` file | runs in | what |
|---|---|---|
| `bake_hangar.py` | uv | per-composite mouth mask, fill, anchor; hangar star tiles |
| `render_hangar.py` | Blender | canonical camera, lights, flight paths, frames |
| `bake_traffic.py` | uv | trim, split at the mouth plane, pack |
| `hangar_layers.json` | - | per-layer loop period and phase |

## Mining concourse (#558)

The original game's mining concourse had a small yellow tug towing an ore
hopper across the floor (the legacy `concourse_car` overlay). It's rebuilt
from the original base-vehicle meshes (`ships_wcnews/truck.obj`, `cart.obj`),
with a heap of ore, headlights and a turning amber beacon, driving down the
floor's plated guide strip toward the camera.

```sh
blender --background --factory-startup \
    --python tools/room_anim/mining/render_layers.py -- --check      # camera-match overlay
blender --background --factory-startup \
    --python tools/room_anim/mining/render_layers.py -- --layer all
uv run tools/room_anim/bake_layer.py --base mining --all
uv run tools/room_anim/composite_preview.py --base mining --seconds 32 \
    --out build/room_anim/mining/preview.mp4
```

**Camera.** Unlike New Con this plate is two-point: the camera is level but
yawed ~16 deg right of the tunnel, so the cross-floor grates tilt. Tunnel
vanishing point (475, 557) from the guide strip and walls; cross vanishing
point ~(4500, 565) from a Hough fit of the grates. Two perpendicular
vanishing points on one horizon give f^2 = (768 - 475)(4500 - 768), so
f ~ 1050 px (24.6 mm) and yaw = atan(293 / f). `scene.floor_point()` maps
plate pixels to the floor; guide-strip points 5-52 m deep all land at
X ~ 3.7 m, and `--check` draws the strip edges and cross lines over the
plate to confirm.

| `mining/` file | runs in | what |
|---|---|---|
| `scene.py` | Blender | two-point camera match, floor + guide-strip deck, lights |
| `actors.py` | Blender | tug + ore hopper (game meshes), ore pile, headlights, beacon |
| `render_layers.py` | Blender | the ore-train layer; `--check` overlay |
| `layers.json` | - | loop period and phase |
| `bake_landing.py` | uv | landing pad: sky masks + rim anchors, star tiles, freighter sheet |
| `render_landing.py` | Blender | the Galaxy freighter pass over the landing pad |
| `landing_layers.json` | - | landing sky layer timing |
| `render_bar.py` | Blender | the bar's 3D patron (woman in orange), camera-matched |
| `bake_bar.py` | uv | bar: clean-plate patch + patron sheet; `--preview` crops |
| `sources/bar_orange_clean_gen.png` | - | AI clean-plate edit of her crop (she's painted out) |

### Mining bar: a 3D patron (#564 spike)

The painted woman in orange on the bench is replaced by a rigged 3D model
(Meshy AI, `characters/rustbound_ranger_sit_cross_legged.glb`) idling on a
seamless 9.6 s loop. `bar_bg.png` is untouched: a one-frame clean-plate patch
paints her out, then her render draws over it. If the layers are missing,
the painting shows as painted.

```sh
blender --background --factory-startup --python tools/room_anim/prep_character.py -- \
    <meshy_clip.glb> tools/room_anim/characters/<name>.glb
blender --background --factory-startup --python tools/room_anim/mining/render_bar.py
uv run --with scipy tools/room_anim/mining/bake_bar.py
uv run --with scipy tools/room_anim/mining/bake_bar.py --preview 0,114,228
```

- **Models.** Meshy exports are ~100k tris with 2K/4K textures, ~30 MB per
  clip. `prep_character.py` shrinks textures to 1K (7.5 MB) and keeps the
  full mesh: triangles cost nothing in a pre-rendered pipeline, and collapse
  decimation tears the UVs (at 10% it shredded her face). The source zips
  stay out of the repo.
- **Camera.** One-point perspective with the horizon through the seated
  heads (y 415) and the eye at seated head height (1.25 m). She's placed by
  her hips over the painted bench (fitted live in the MCP Blender against the
  clean plate), not by the painted woman's outstretched boots.
- **Shadow catchers** (floor, bench, railing) are built from her measured
  pose. Catchers are holdouts to the camera, so one that intersects her cuts
  her out; they also don't cast shadows, so only hers lands on the set.
- **Clean plate.** An AI edit of her crop; only her region is taken
  (differences from the plate inside her box, plus the whole seat box),
  feathered, and never below the bar counter's edge, which is in front of
  her. Her render is clipped at that edge too.
- **Tone** is tuned against the painted woman (`build/room_anim/bar_tone.py`
  scratch): luminance p10/p50/p90 17/45/74 vs the painting's 16/46/73,
  saturation 0.62 vs 0.63.

### Mining landing pad (#561)

The landing pad is an open crater: a thin strip of black sky over a jagged
rim, in 18 per-hull composites framed differently. Stars drift slowly through
the strip, and every 55 s a Galaxy freighter (`ships_wcnews/mrchship.obj`)
climbs out from behind the right rim and crosses overhead, the original
game's `landing_shp` beat.

```sh
uv run tools/room_anim/mining/bake_landing.py --debug build/room_anim/mining/skies.png
# (bake the skies first: render_landing.py reads tarsus's rim from anchors.json)
blender --background --factory-startup --python tools/room_anim/mining/render_landing.py
uv run tools/room_anim/mining/bake_landing.py --layers-only
uv run tools/room_anim/composite_preview.py --base mining --room landing --plate tarsus \
    --crop 0 0 1536 400 --seconds 55 --out build/room_anim/mining/landing.mp4
```

- **Sky.** Each composite has its own black level (max channel 0 to 19), some
  skies are shaded, and the rock's crevices are about as dark, so no
  brightness threshold works (a loose one floods down crevices, a tight one
  punches holes in shaded skies). Structure does: the rim is a height field.
  Each column is sky down to its first sustained run of rock (level + 12),
  and a 1-D opening of that rim profile removes the drips down crevices that
  are dark right up to the rim. The painted sky is starless; the tiles are a
  sparse synthetic field.
- **Anchor.** The composites differ mainly in rim height, so the anchor is
  vertical only: `cy` = the rim's median height, and `cx`/`r` are fixed.
  Anchored layers slide with the rim and are never rescaled.
- **Freighter.** Rendered against tarsus (its rim height is read from
  `anchors.json`) with a level f = 1024 px camera. It flies at a constant
  altitude while closing on the camera, so it rises and grows gently. The
  path is solved from rim-relative screen targets: its centre goes from
  rim + 10 to rim - 30, which keeps the ~50 px ship inside even the
  tightest sky (~61 px).

Shared Blender helpers (render settings, boxes, materials, lights, emitters,
straight passes) live in `stage.py`; `bake_layer.load_frame()` trims a
straight-alpha pass for sky layers; `ships.import_ship()` takes `grounded=` (origin on
the underside, for vehicles) and `tint=` (multiply the textures, e.g. grime).

## Adding a layer

1. Add a builder to `LAYERS` in `<base>/render_layers.py`.
2. Add its loop timing to `<base>/layers.json`.
3. Render, `bake_layer.py --base <base>`, and list `anim/<layer>.json` in the
   room's `"layers"` in `assets/concourse/<base>/concourse.json` (drawn in
   list order: far first).
4. Preview with `composite_preview.py --base <base>`, then run `test_room_anim`.

## Adding a base

1. Make `tools/room_anim/<base>/` with a camera-matched `scene.py` (see
   `newcon/scene.py`: camera, proxy decks, holdout occluders, lights), a
   `render_layers.py` that builds actors and calls `render.render_passes()`
   into `paths("<base>").build`, and a `layers.json`.
2. Scripts in `<base>/` put `tools/room_anim/` on `sys.path` to import the
   shared modules (see the top of `newcon/render_layers.py`).
3. Then follow *Adding a layer*.

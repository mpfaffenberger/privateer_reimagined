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
| `patron_room.py` | both | loads a room file: camera, lights, paths, patrons (#577) |
| `render_patrons.py` | Blender | a room's 3D patrons, camera-matched (`--room`, `--patron`) |
| `bake_patrons.py` | uv | clean-plate patch + patron sheet each (`--room`); `--preview` crops |
| `prep_character.py` | Blender | Meshy GLB -> committable model (1K textures, one clip) |
| `meshy.py` | uv | Meshy API: image-to-3D + rig (`character`), clips (`animate`) |
| `still.py` | uv | a still patron's small moves: glances, a smile, the fidget timeline |
| `wire_room.py` | uv | point one room's `layers` at a bake, in every base's `concourse.json` |
| `characters/` | | prepped, rigged 3D characters |
| `guild/` | | the guild rooms every base shares: Mercenaries' (#578), Merchants' (#579) |
| `newcon/` | | New Constantinople: concourse (#515) and hangar (#553) |
| `mining/` | | Mining base concourse: the ore train (#558) |
| `agricultural/` | | Agricultural: concourse clouds and aircraft (#582), landing pad sky traffic (#583) |

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
| `bar_patrons.json` | - | the bar's room file: camera, lights, and each 3D patron |
| `sources/bar_*_clean_gen.png` | - | AI clean-plate edits of each patron's crop (painted out) |

### Mining bar: 3D patrons (#564, #566, #568, #570, #572, #571)

Painted patrons are replaced by rigged 3D models (Meshy AI, `characters/`)
idling on seamless loops: the woman in orange on the bench (#564), the
man at the back table (#566), the bartender (#568), and the woman at the left
table (#570) with the bald man across from her (#572), and the big man in
the foreground (#571): every patron in the room. `bar_bg.png` is untouched: per patron, a
one-frame clean-plate patch paints them out, then their render draws over
it. If the layers are missing, the painting shows as painted. Each patron is
one entry in `bar_patrons.json`, the bar's room file (see "3D patrons in any
room" below); the camera and lighting are the room's.

**Overlapping patrons (the left table) are stacked.** `bar_patrons.json` lists
patrons back to front. Each clean plate is an AI edit of the plate *with every
earlier patron already painted out* (`bake_patrons.cleaned_plate`): use that crop
as the edit's reference, and it registers against it. So a nearer patron's
patch never paints a farther one back in. The room draws every patch first,
then every patron in list order, so nearer patrons cover farther ones. A
nearer patron who's still painted is a `front` occluder of the farther one,
until they go 3D too.

```sh
blender --background --factory-startup --python tools/room_anim/prep_character.py -- \
    <meshy_clip.glb> tools/room_anim/characters/<name>.glb [--clip-from <other_clip.glb>]
blender --background --factory-startup --python tools/room_anim/render_patrons.py -- \
    --room tools/room_anim/mining/bar_patrons.json [--patron patron_backtable]
uv run --with scipy tools/room_anim/bake_patrons.py --room tools/room_anim/mining/bar_patrons.json [--patron ...]
uv run --with scipy tools/room_anim/bake_patrons.py --room tools/room_anim/mining/bar_patrons.json \
    --patron patron_orange --preview 0,114,228
```

- **Borrowed clips.** All six Meshy characters share one 28-bone Mixamo
  skeleton, so `--clip-from` puts any character's clip on any other's mesh
  by a straight copy of the action. Their rest poses differ by up to ~24 deg
  at hands and feet, but the copy still looked right, and a world-space
  retarget twisted the whole body. The Mechanic borrows `Chair_Sit_Idle_M`
  from the Blue Jacket Worker; the woman's loop is phase-shifted from his so
  the bar doesn't idle in sync.
- **Painted furniture** in front of or under a patron is modelled as
  cylinder catchers (`props`): they hold the patron out exactly where the
  painted table/stool is and catch their shadow (hands resting on the
  tabletop). Flat occluders (the counter edge, the back table's mug) are
  `front` masks instead: never patched, and clipped out of the render.
- **Only real catchers.** `set` lists what the painted patron actually sits
  on / against. A railing catcher just behind the back-table man (the
  woman's bench is against the rail; his table is ~2 m from it) printed a
  big shadow wedge across the painted railing, and a seat catcher under his
  hidden stool a grey box: he gets the floor only.
- **Painters cheat.** The back-table man is painted ~1.3x broader than a
  model at his depth, so a patron can be `scale`d; and a model's clothes
  needn't match the painted ones, so its textures can be `tint`ed (his grey
  work jacket towards the painting's navy denim: saturation 0.17 -> 0.21 vs
  the painting's 0.25, midtones matched at lum 18).
- **Borrowed clips can overact.** `lean` damps the torso's rotation keys
  towards the clip's (upright) first frame: `Chair_Sit_Idle_M` dives over
  the table; at 0.65 its deepest lean is the painted man's hunch. At 0.4 he
  sat bolt upright and his hands, still fully animated, hovered at his
  chest. The clip starts and ends upright, so the loop stays seamless.
- **The bartender: leaning on a bar with no leaning clip.** Meshy has no
  such clip, so he plays his own standing `Idle_3` with his arms `aim`ed
  (Damped Track) at his painted elbows and hands on the counter: they stay
  planted while the idle sways his body over them. IK straightened his
  arms into zombie arms, because the targets sat at the edge of his reach;
  aiming has nothing to flip. He's a bust (feet hidden), so he's placed by
  his crown (`head`), leaned forward from the feet (`pitch` 18) and his chin
  raised back (`head_up`, through the neck and head keys, keeping the
  idle's glances). Fit lesson: the clean plate puts the counter's back edge
  at ~2.56 m, so he must lean *over* it (crown ~2.6 m), not stand at the
  2.8 m his painted head size suggests. The model's upper arm is too short
  for his painted elbows, so his hands rest on the bar with elbows bent.
- **Per-patron light and grade.** The bartender's corner is lit softly from
  the glowing counter, not by the room's lamps: `light` scales the room's
  key/fill/rim (0.75 / 17 / 0), taking his contrast from 20 to 5 (painting
  4.7). No rim: the room's rim sits behind each patron, which for him is
  behind the bar, so it threw his shadow forward across the painted counter
  (the painting's counter is evenly lit). The painter's palette is vivid, so he also gets `saturation` 1.35
  plus a cool `tint`: that gives blue denim instead of a dull navy, without
  the tangerine skin that saturation alone produced (sat 0.45 vs 0.46).
- **The left-table woman** borrows the Rustbound Ranger's `Chair_Sit_Idle_F`
  and leans on the table like the bartender leans on his bar: placed by her
  crown (her feet are hidden), `pitch` 12, arms `aim`ed at the painted
  elbows and mug. The clip slumps her head almost to the table halfway
  through; `lean` 0.25 (the fraction of torso motion *kept*) takes it out.
- **The bald man** (Weathered Enforcer) borrows the Blue Jacket Worker's
  `Sitting_Answering_Questions`: from behind, its gestures read as him
  talking to the woman. The painter drew him narrow but long-backed, so
  `scale` takes `[width, depth, height]` in the body's frame: [0.82, 0.82, 1].
  Uniform 0.8 fit his width but left his hips hovering over the stool,
  because the crown is pinned. He's lit at key/fill x1.7 (the render ran
  darker than the painted jacket). His painted jeans still show through the
  foreground's glass bottle (a `front` occluder is never patched): the same
  glass compromise as the bartender's bottle. Going 3D, he leaves the
  woman's `front` list, so her sheet is re-baked here.
- **The foreground man** (Weathered Sentinel, borrowing `Chair_Sit_Idle_M`)
  sits on the stool with his forearm `aim`ed along his table's rim. Crown
  anchoring puts his hips at 0.50 m, right on the painted stool. Fit
  lesson: size a table from its post's floor contact, not an assumed
  height. Assuming 0.75 m put it at 2.25 m; the base on the floor says
  2.68 m, with the top at 0.62 m and a 0.5 m radius. `pitch` -10 to sit him
  up hunched him into a ball; `lean` 0.3 alone tames the clip's slump.
  His legs are `aim`ed too: thighs *down* to knees under the table edge
  (z 0.36), shins to the floor. Aiming the thighs level raised the knees
  to the tabletop. His scale is [1.25, 1.25, 1.0] for the broad back. Never
  scale z on a crown-anchored patron: a taller body drops the hips into
  the stool. No floor catcher (`set: []`): his shadow on the dark floor
  rendered as a black slab over the painted floor. He's in the darkest
  corner: key x0.7, fill x1.6 to keep detail in the legs.
  `aim` only points a bone; its **twist** still comes from the clip. His
  idle flipped his hand into a claw and twisted his forearm, and the bent
  wrist turned that twist into a 75 px sweep of the fingers onto the
  bottle, even though the elbow moved only 14 px. `hold` freezes the
  whole right-arm chain (`RightShoulder`, `RightArm`, `RightForeArm`,
  `RightHand`) at frame 0: the hand rests beside the mug with 25 px of
  drift from the hips. At full size his 258 frames
  (~350x580 px) need a 4096x25799 atlas, past the 8192 limit, so he's
  `half_size`: sprites stored at half size and scaled back by the engine.
  That's fine in the softest, darkest part of the painting; everyone else
  stays full size.
- **Blender MCP gotcha:** a freshly `images.load()`ed camera background can
  sit at 0x0 and draw nothing until its pixels are touched
  (`img.pixels[0]`).

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

## Agricultural concourse (#582)

The concourse looks out through tall windows over farmland at dusk: long
cloud streaks over a glass farm dome, a lit settlement and far ridges. The
original game scrolled a band of clouds across its window view (the legacy
`concourse_wtr` overlay); here the painted streaks themselves drift, and two
craft pass outside: a Galaxy freighter (`mrchship`) climbing away from the
spaceport right of the windows, and a yellow aircar (`aircar`) skimming the
fields in front of the dome.

```sh
uv run tools/room_anim/agricultural/bake_sky.py --debug build/room_anim/agricultural/sky_debug.png
blender --background --factory-startup \
    --python tools/room_anim/agricultural/render_traffic.py -- --layer all
uv run tools/room_anim/agricultural/bake_traffic.py --all
uv run tools/room_anim/composite_preview.py --base agricultural --seconds 30 \
    --out build/room_anim/agricultural/preview.mp4
```

- **Clouds through the sky machinery.** The engine's sky is a fill plus
  alpha-over tiles, with no additive blend, so the painting is split into a
  *fill* (a horizontal upper envelope of the painted sky: the glow between
  the streaks) and *tiles* holding the streaks as darkening: per row one
  colour, the row's darkest cloud tone, whose alpha pulls the envelope down
  to the painting. The tiles scroll sideways only, so a per-row colour is
  allowed, and darkening is a ratio, so a streak reads the same over the
  brighter right pane. At t=0 the composite matches the plate (p99.9 error
  6/255). Two tiles give parallax: the high streaks drift at 2.6 px/s,
  the low ones by the horizon at 1.4.
- **Tile layout.** Tile x = plate x mod 860, so the centre and right panes
  (x 620..1480) sit side by side in one tile; gaps (mullion, frame bevels,
  the arch) are filled from their neighbours *shifted*, not mirrored (a
  mirror drew an X at every gap). The mask fades in below the starry top of
  the sky and out above the far ridges, so the stars and the land stay put;
  painted stars are kept in the fill, brightened so the streaks darken them
  back to the painting at t=0.
- **Gotchas.** In the dim upper sky the envelope is barely above the dark
  tone, and the ratio turned brush noise into drifting grain (`SPAN_MIN`
  fades it out). Bright specks inside the glow band are cloud texture, not
  stars (`STAR_SKY_LUM`). The arch left of the centre pane has a lit grey
  bevel as bright as the high sky, so the glass is hand-traced
  (`windows.py`), not found by darkness.
- **Aircraft.** Nothing outside is near enough to camera-match, so the
  camera is matched to the horizon only: level, f = 1000 px, the far
  ridges at y 172, 60 m above the fields (which makes the farm dome
  ~370 m across). Paths are solved from screen targets. The sun is just
  down behind the dome, so hulls are backlit; nav lights keep them
  readable. They fly in front of the painted land, so they are drawn *over*
  the plate: `bake_traffic.py` hazes each frame toward the painted horizon
  glow by its depth and clips it to the window glass.

| `agricultural/` file | runs in | what |
|---|---|---|
| `windows.py` | uv | hand-traced window glass (centre + right panes) |
| `bake_sky.py` | uv | cloud fill, streak tiles, sky mask |
| `flight.py` | Blender | horizon-matched camera, paths from screen targets, the render loop |
| `render_traffic.py` | Blender | dusk light and the two craft |
| `bake_traffic.py` | uv | haze, clip to the glass, pack |
| `traffic_layers.json` | - | loop period and phase |

### Agricultural landing pad (#583)

The pad sits by a lake under a violet dusk sky with two moons, a brick tower
and two docking pylons against it, in 18 per-hull composites framed
differently. Two craft pass in the sky, under the plate, so the tower and
pylons occlude them on every hull: a transport (`transprt`) crossing right
to left behind them, and a Galaxy freighter (`mrchship`) coming in high on
the right and sliding down behind the tower's top.

```sh
uv run tools/room_anim/agricultural/bake_landing.py --debug build/room_anim/agricultural/skies.png
# (bake the skies first: render_landing.py reads tarsus's anchor from anchors.json)
blender --background --factory-startup \
    --python tools/room_anim/agricultural/render_landing.py -- --layer all
uv run tools/room_anim/agricultural/bake_landing.py --layers-only
uv run tools/room_anim/composite_preview.py --base agricultural --room landing --plate tarsus \
    --crop 0 0 1536 560 --seconds 45 --out build/room_anim/agricultural/landing.mp4
```

- **Sky.** It's a saturated blue-to-violet-to-pink gradient, and the moons
  are bluish too, while green stays low in all of it. The tower, pylons,
  hulls and far shore have no blue lead over green, so sky = pixels well
  bluer than green, flooded from the top edge. The dark far shore runs all
  the way across, so the lake, which mirrors the sky's colour, is never
  reached. Dralthi's darker top sky dithers around blue 120, so the floor
  is blue 90.
- **Moons in the fill.** The moons are inside the mask and the fill is the
  painted sky at half resolution (not the usual blurred quarter-res fill),
  so a craft passing in front of a moon covers it. No star tiles: the
  painted sky has none.
- **Anchor.** Layers follow the big moon's centre (the largest blob that
  stands out from its row's sky colour; lit pink on gladius, shaded on the
  rest). Translation only: a shaded limb melts into the sky, so the
  detected size isn't a reliable zoom, and `r` is fixed.
- **Aircraft** use `flight.py` (above): a level f = 1024 px camera with tarsus's
  far shore (y 505) on the horizon, and paths solved from tarsus screen
  targets. Dusk light: a blue sky dome and a low pink key from behind.

| `agricultural/` file | runs in | what |
|---|---|---|
| `bake_landing.py` | uv | per-composite sky mask, half-res fill, moon anchors; layer sheets |
| `render_landing.py` | Blender | dusk light, the transport and the freighter |
| `landing_layers.json` | - | loop period and phase |

Shared Blender helpers (render settings, boxes, materials, lights, emitters,
straight passes) live in `stage.py`; `bake_layer.load_frame()` trims a
straight-alpha pass for sky layers; `ships.import_ship()` takes `grounded=` (origin on
the underside, for vehicles) and `tint=` (multiply the textures, e.g. grime).

## 3D patrons in any room (#577)

The bar's patron pipeline works for any room with painted people in it. A
**room file** (format in `patron_room.py`) holds a top-level `"room"`
block and the patrons, back to front:

- **paths:** `plate`, `build` (raw renders), `out` (baked layers), `sources`
  (clean-plate edits), all repo-relative;
- **`camera`:** a level camera, `horizon_y` (plate px), `eye` (m), `focal_px`;
- **`lights`:** key/fill colours, key/fill/rim watts, key size, ambient.

`render_patrons.py --room <file>` and `bake_patrons.py --room <file>` do the
rest. The camera and world are named after the room (`BarCam`). Fit a new
room's camera the way the bar's was: pick the horizon from where the
painting's level lines converge, set `eye` from a head or counter of known
height, and check a patron's crown and hips land on the painting.

**New characters come from Meshy** (`meshy.py`):

```sh
uv run --with requests tools/room_anim/meshy.py character <name> --image <full-body ref.png> --height 1.7
uv run --with requests tools/room_anim/meshy.py library --search sit
uv run --with requests tools/room_anim/meshy.py animate <name> --action 33 --action 343
blender --background --factory-startup --python tools/room_anim/prep_character.py -- \
    build/room_anim/meshy/<name>/<Clip>.glb tools/room_anim/characters/<name>_<clip>.glb
```

- **Reference image:** Meshy wants one person, full body, front-facing,
  A-pose, plain background, flat light, nothing in their hands (a held prop
  fuses into the hand and wrecks the rig). A painted bust has no legs, so
  make the reference with an image model conditioned on the painted crop,
  and keep it in the room's `sources/`.
- **Costs** (credits): image-to-3D 30 (meshy-7.1, 2K textures), rigging 5,
  each clip 3. `animate` makes one task per clip, so every clip is its own
  GLB (`prep_character.py` keeps one clip per file) and idles can be
  auditioned side by side.
- **Resumable:** every task id goes into `build/room_anim/meshy/<name>/
  state.json` before it's polled, so a rerun polls the same task instead of
  paying for a new one. A failed task is refunded and dropped from the
  ledger, so a rerun retries it.
- **The key** comes from `MESHY_API_KEY` or `~/.config/meshy/api_key` and is
  never printed. A `402 "API key credit limit reached"` with credits in the
  account means the key's own monthly cap (Meshy Developer Platform) is
  spent.
- **No library clip** leans on a counter, types or smokes. `aim`/`hold`
  plant hands on furniture (the bartender, #568; the foreground man, #571).
  `Sit_and_Drink` (343) is the library's only hand-to-mouth gesture, and
  Meshy's Text to Motion can make custom clips.

### Mercenaries' Guild (#578)

`guild/mercguild_patrons.json`: the woman behind the counter, from Meshy
(`sources/merc_woman_ref.png` -> `characters/merc_woman_hand_rub.glb`). The
room file's `_doc` has the camera match and every fitting decision.

```sh
blender --background --factory-startup --python tools/room_anim/render_patrons.py -- \
    --room tools/room_anim/guild/mercguild_patrons.json
uv run --with scipy tools/room_anim/bake_patrons.py --room tools/room_anim/guild/mercguild_patrons.json
cd tools/room_anim && uv run wire_room.py mercguild \
    ../../shared_rooms/mercguild/merc_woman_patch.json ../../shared_rooms/mercguild/merc_woman.json
```

### Merchants' Guild (#579)

`guild/merchguild_patrons.json`: the merchant at his desk, smoking. He holds
one pose and takes a slow drag on a loop: the ember (a keyed emissive tip on
a `held` cigar, riding his finger skin) glows, his hand (animated `aim`
targets) takes the cigar off his lips, and he exhales a puff (`still.smoke`,
its own layer over him). With no finger bones, his fingers are straight: the
pose that works is the back of the hand to camera, fingers up beside his
mouth (probe hand poses over a clean plate, never the painting: the painted
hand passes for a grip). Its bake needs OpenCV: his clean
plate is `inpaint`ed, not an AI edit (the image model was out of reach).

```sh
uv run --with scipy --with opencv-python-headless tools/room_anim/bake_patrons.py \
    --room tools/room_anim/guild/merchguild_patrons.json
cd tools/room_anim && uv run wire_room.py merchguild ../../shared_rooms/merchguild/merch_man_patch.json \
    ../../shared_rooms/merchguild/merch_man.json ../../shared_rooms/merchguild/merch_man_smoke.json
```

**Rooms shared by every base** (the guild paintings are byte-identical in
all nine) bake once to `assets/shared_rooms/<room>/`, and each base's
`concourse.json` names the layers as `../../shared_rooms/<room>/<layer>.json`.
The engine joins the base dir and that path as-is, and the atlas resolves
next to its manifest. They stay out of `assets/concourse/`, whose every
subdirectory is treated as a base (archetype walkers, and
`other_archetypes_static` in `test_room_anim`).

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

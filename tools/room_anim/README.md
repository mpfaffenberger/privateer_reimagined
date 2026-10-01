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
| `bake_crater.py` | uv | crater landing pads: rim sky masks + anchors, star tiles, sky sheets (`--base`) |
| `flyover.py` | Blender | crater landing pads: canonical sky camera, rim-relative ship paths, render loop |
| `ships.py` | Blender | game-mesh import, sidecar materials, orientation |
| `bake_layer.py` | uv | plate-aware sprite encoding + atlas packing (`--base`); `bake_passes()`: landing-pad sky passes -> sheets |
| `sky.py` | uv | star removal, mask solidify, sky fill, `half_fill()` for smooth skies, painted-star stats, star tiles, `--debug` contact sheets |
| `composite_preview.py` | uv | engine-faithful preview from `concourse.json` (`--base`) |
| `stage.py` | Blender | render settings, boxes, materials, lights, straight/polyline paths, the `--check` overlay |
| `walkers.py` | Blender | pedestrian proxies and their walk (New Con, pirate); rigged walkers (New Detroit) |
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
| `pleasure/` | | Pleasure: concourse skylight stars, marquee chase, neon (#594), landing pad transport (#595) |
| `pirate/` | | Pirate base concourse: lanterns, pirates, a grav pod (#586) |
| `newdetroit/` | | New Detroit: concourse walkers on the platform and plaza (#590), aircars past the landing pad (#591) |
| `oxford/` | | Oxford: concourse air-cars and pedestrians in the garden square (#592, #610), landing pad departure at dusk (#593) |
| `refinery/` | | Refinery: concourse stars, ships over the dome, the ore train (#584), landing pad door and ships (#585) |
| `military/` | | Military: concourse stars, a fighter pair, the munitions train (#588), landing bay lamp chase (#589) |

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
| `actors.py` | Blender | procedural hover-car proxy; pedestrians come from `../walkers.py` |
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
| `render_landing.py` | Blender | the Galaxy freighter pass over the landing pad (`../flyover.py`) |
| `landing_layers.json` | - | landing sky layer timing, star seed (`../bake_crater.py`) |
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
uv run tools/room_anim/bake_crater.py --base mining --debug build/room_anim/mining/skies.png
# (bake the skies first: render_landing.py reads tarsus's rim from anchors.json)
blender --background --factory-startup --python tools/room_anim/mining/render_landing.py
uv run tools/room_anim/bake_crater.py --base mining --layers-only
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

The crater machinery is shared (#587): `bake_crater.py --base <base>` finds
the skies, anchors, star tiles and packs the sky layers listed in
`<base>/landing_layers.json` (which also holds the base's `_star_seed` and
any `_sky` detection options); `flyover.py` is the Blender side (canonical
camera, sun and crater bounce, `screen_path()` from rim-relative targets,
nav lights, the render loop). A base's `render_landing.py` only builds its
ships. Mining's masks, anchors and tiles re-bake byte-identical, and its
freighter scene matches the pre-refactor script frame for frame.

### Pirate landing pad (#587)

The pirate pad is the same crater, and so gets the same stars. Its ship is
its own, though: every 45 s two pirate Talons (`ships_wcnews/talon5.obj`,
orange exhaust, nav lights) come in low and fast from behind the left rim in
echelon and climb out behind the right one, a 5.5 s buzz.

```sh
uv run tools/room_anim/bake_crater.py --base pirate --debug build/room_anim/pirate/skies.png
blender --background --factory-startup --python tools/room_anim/pirate/render_landing.py
uv run tools/room_anim/bake_crater.py --base pirate --layers-only
uv run tools/room_anim/composite_preview.py --base pirate --room landing --plate tarsus \
    --crop 0 0 1536 400 --seconds 45 --out build/room_anim/pirate/landing.mp4
```

- **Sky.** The pirate composites are harder than the mining ones: some skies
  are navy over dim brown rock (broadsword: sky 30, rim rock 35-45), and some
  are shaded across the frame (drayman: 9 at the left, 25 at the right).
  A single brightness margin drips down crevices on the first kind and eats the
  bright side of the second. `landing_layers.json` turns on two
  `find_sky()` options (both off for mining):
  - `level_band` measures the sky level per 128 px column band, capped at one
    margin over the overall level, so a rock wall reaching the top corner
    can't lift it.
  - `warm_margin` also calls rock anything 8 warmer (red minus blue) than the
    sky. Navy sky runs -17 and brown rock +5 to +15, even where the rock is
    as dark as the sky (stiletto's top-right wall).
- **Talons.** They're 24 m long and 100 m up. The leader's centre goes from
  rim - 28 to rim - 48 px. The wingman sits 32 m back, 18 m out to port (away
  from the camera, so ~14 px lower) and 4 m up. Both are banked 10 degrees.
  On the starboard side the wingman was nearer the camera, so it flew higher
  on screen and clipped the top edge of the tightest skies.

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

## Pleasure concourse (#594)

The casino hall was repainted at 1536x1024 for this (#596; it was the last
320x200 concourse). Everything that moves here is light that's already in the
painting, so there's no Blender pass:

```sh
uv run tools/room_anim/pleasure/bake_sky.py --debug build/room_anim/pleasure/sky_debug.png
uv run tools/room_anim/pleasure/bake_lights.py --debug build/room_anim/pleasure/bulbs.png
uv run tools/room_anim/composite_preview.py --base pleasure --seconds 18 \
    --out build/room_anim/pleasure/preview.mp4
```

- **Skylight.** Stars drift through the glass pyramid's apex pane and one
  right-hand pane. The rafters are wide dark beams with lit rims, as dark as
  the sky, so the panes are hand-traced between them (no threshold keeps
  stars off a beam's face). Inside a pane, the glare streaks are separated
  from stars by shape: a blob brighter than the local sky is a star if it's
  small and glass if it's long. Unlike `sky.solidify()`, no holes are filled,
  since that would paint out the streaks. Most painted stars are faint, so
  `PaintedStars(rise=8)` counts them (~49/10k px vs ~10/10k at the default 18).
- **Marquee chase.** Warm bulb cores are found inside each canopy's outline
  and chained into rows (each bulb's successor is its nearest neighbour
  toward the rows' vanishing point); a bulb's place in its row is its chase
  phase. Each step dims every third bulb and its glow cone to 25% in linear
  light, and the step marches at 4 Hz. A 3-frame loop per canopy.
- **Neon.** The red sign stutters like a tired tube once every 9 s. The neon's
  share of each pixel (red excess, plus the near-white tube cores) is dimmed
  in linear light. Only the stutter frames have sprites, and the painted sign
  shows the rest of the time.
- Pedestrians were left out: the free floor is either a 1 m aisle behind the
  painted couches or right under the camera, where the proxy walkers would
  be 300+ px tall.

| `pleasure/` file | runs in | what |
|---|---|---|
| `bake_sky.py` | uv | skylight panes -> sky mask, fill, star tiles |
| `bake_lights.py` | uv | marquee chase (both canopies) and neon stutter sprites |
| `bake_landing.py` | uv | landing pad: sky masks, half-size fills, horizon anchors; layer sheet |
| `render_landing.py` | Blender | the transport over the sea |
| `landing_layers.json` | - | landing layer timing |

### Pleasure landing pad (#595)

An open-air pad on a resort world at dusk: two towers, a brick tower block on
an island, the sea, and a purple sky with two moons, in 18 per-hull
composites. A liner-sized transport (`ships_wcnews/transprt.obj`) comes out
from behind the block and cruises away toward a spaceport beyond the left
tower. At a constant altitude it sinks toward the horizon and shrinks as it
goes, blinking its nav lights and strobe. It crosses every 45 s.

```sh
uv run tools/room_anim/pleasure/bake_landing.py --debug build/room_anim/pleasure/skies.png
# (bake the skies first: render_landing.py reads tarsus's horizon from anchors.json)
blender --background --factory-startup --python tools/room_anim/pleasure/render_landing.py
uv run tools/room_anim/pleasure/bake_landing.py --layers-only
uv run tools/room_anim/composite_preview.py --base pleasure --room landing --plate tarsus \
    --seconds 14 --out build/room_anim/pleasure/landing.mp4
```

- **Sky.** The sky and the sea are the same saturated blue-purple, and the
  sunset glow is pink, so the colour test is blue over *green* (blue over red
  fails in the glow). The sky is also smooth (within 14 of a 60 px median),
  which the moons, brickwork, tower edges and sea are not. The mask is that
  region above the horizon and connected to the top of the frame. The fill is
  the painted sky itself at half size, so the moons and gradient are exact,
  with holes extrapolated by `sky.sky_fill()`.
- **Horizon (the anchor).** It's soft paint, and no cue finds it on all 18:
  the sky pinkens toward it on most composites but not all, and the far sea
  can be as glassy as the sky. Ripples are the safe cue (the first sustained
  run of row-to-row change). Checked by eye on all 18, that lands up to ~70 px
  *below* the painted horizon where the sea is glassy, but never more than a
  few px above it. So the path's lowest point is 110 px above the anchor:
  the error only ever moves the ship toward open sky, and it never gets
  pushed up into the moons.
- **Transport.** It's rendered against tarsus, with a level f = 1024 px
  camera lens-shifted so its horizon is tarsus's anchor. The path is solved
  from horizon-relative screen targets (230 px above at x 900, 110 px above
  at x 250) at a 500 m altitude. It's drawn under the plate, so each
  composite's own block, towers and parked ship occlude it.

## Pirate concourse (#586)

A smugglers' rock tunnel: warm cage lanterns down the left wall, a cold wash
across the metal floor, a fog-filled hall through the far arch and a side
bay off the right wall behind a rock pillar. Its lanterns flicker, pirates
cross between the far hall and the bay, and a grav pod (the game's
`ships_wcnews/trailer.obj` cargo pod, grimed, on a blue hover glow with a red
strobe) floats out of the bay and off into the hall.

```sh
blender --background --factory-startup \
    --python tools/room_anim/pirate/render_layers.py -- --check      # camera-match overlay
blender --background --factory-startup \
    --python tools/room_anim/pirate/render_layers.py -- --layer all
uv run tools/room_anim/bake_layer.py --base pirate --all
uv run tools/room_anim/pirate/bake_lanterns.py
uv run tools/room_anim/composite_preview.py --base pirate --seconds 46 \
    --out build/room_anim/pirate/preview.mp4
```

**Camera.** One-point, level. The left wall's foot and the right kerb (the
floor's lit edge strip) meet at (540, 430); the ceiling pipes converge
further right (~630, 397), because the painter's tunnel bends, and the floor
is what the actors touch. Eye height 2.7 m from the painted drums (0.9 m,
about a third of their below-horizon distance tall). The focal length (1024
px) hardly matters here: with a level camera, floor positions and upright
sizes follow from the horizon and the eye height alone
(`X = (px - vx) * eye / (py - hy)`); f only sets depth, i.e. how long things
take to cross. `--check` puts the floor edges, 5 m cross lines and posts on
the arch legs, pillar and drums over the plate.

**Where actors come and go.** The tunnel has no side doors big enough, so
everyone enters and leaves behind painted things, held out in every pass:
the far arch's legs (32.5 m out), the rock pillar in front of the side bay
(16.3 m) and the green drums. The pod stays 20 m+ out: nearer the camera the
game mesh's facets show and it outshone the painting.

**Lanterns** are 2D (`bake_lanterns.py`): a flame only changes how bright the
painting already is around it. The plate is scaled in linear light by
`1 + (m(t) - 1) * g`, g a Gaussian round the lantern and m a seamless wobble
with the odd gutter, then `bake_layer.encode()` makes each frame a
minimal-alpha sprite over the plate.

| `pirate/` file | runs in | what |
|---|---|---|
| `scene.py` | Blender | camera match, deck, holdouts (arch legs, pillar, drums), lights |
| `actors.py` | Blender | the grav pod (game mesh) and the pirates' looks |
| `render_layers.py` | Blender | pod + pirate layers; `--check` overlay |
| `layers.json` | - | loop period and phase |
| `bake_lanterns.py` | uv | lantern flicker sheets, straight from the plate |

Shared walker proxies (`build_walker`, `animate_walk_path`: a polyline
walk that turns through corners) live in `walkers.py`; `stage.animate_path()`
moves anything along a polyline, and `stage.overlay_on_plate()` makes a
`--check` image.

## New Detroit concourse (#590)

A wet plaza between towers: a bar under a red awning on the left, stairs to a
second level at the back, and on the right a raised platform under the
hangar building, its pillars lit by blue kerb lamps. Two people walk it:

* `walker_platform` comes in along the platform from frame-right, passes
  behind both pillars (a peek between them), steps down the kerb and crosses
  the plaza, reflected in the wet floor, into a side passage past the left
  building's corner.
* `walker_door` steps out of the platform's lit doorway, silhouetted, and
  walks off frame-right.

```sh
blender --background --factory-startup \
    --python tools/room_anim/newdetroit/render_layers.py -- --check
blender --background --factory-startup \
    --python tools/room_anim/newdetroit/render_layers.py -- --layer all
uv run tools/room_anim/bake_layer.py --base newdetroit --all
uv run tools/room_anim/composite_preview.py --base newdetroit --seconds 41 \
    --out build/room_anim/newdetroit/preview.mp4
```

**Camera.** Loose one-point perspective: the left wall's lamp strip, the
floor's grate channel and the platform kerb meet the horizon at y 555, but
scatter in x from ~190 to ~430 (the painting isn't strict), so the vanishing
point is their middle, x 330, with lens shift. Eye 2.8 m puts the bar
stools' seats at 0.9 m and the platform door at 2.0 m; f 1100 px keeps the
pillars ~1.5 m across. `scene.floor_point()` maps plate pixels to the floor,
and every landmark (kerb, pillars, back wall, doorway, the building corner)
is written in `scene.py` as the plate pixel it was measured at. `--check`
draws the holdouts, kerb and walker paths over the plate.

**Rigged walkers.** At 90-160 px tall the box proxies read as walking
crates, so these are the rigged characters from `characters/` (the Blue
Jacket Worker and the Ironclad Wanderer) with a procedural walk cycle
(`walkers.rigged_gait`) keyed onto their shared Mixamo skeleton: no paid
walk clip (`build_rigged_walker`, `animate_rigged_walk`). Every swing is a
rotation about the model's side axis, converted into each bone's rest
frame, so bone roll doesn't matter and parents carry children: stride, knee
fold, a sole kept near level, opposing arm swing, pelvis sway and drop.

- **Clearing a clip leaves its pose.** `animation_data_clear()` keeps each
  bone's last evaluated pose; the idle's hunched shoulders held both arms
  out like a tray. Every pose bone is reset to identity first.
- **Occluded, not faded.** Walkers enter and leave behind holdouts (pillars,
  the back wall either side of the doorway, the left building block), so
  nothing pops. Paths are polylines (`stage.animate_path`), whose floor
  height can be a function: the walker steps down the platform's kerb.
- **Lights are never seen.** Every light is hidden from the camera and from
  glossy rays. A lamp visible behind a walker clipped to white and haloed
  him (the bake's maths assumes nothing clips), and the proxy deck's mirror
  images of the lamps only added noise, printed as coloured specks. They
  cancel in A - B anyway; the painting has its own reflections.

## Oxford concourse (#592)

The university town's garden square, seen from high up. Two of the original
game's air-cars (`ships_wcnews/aircar.obj`) glide down its streets: an
oxblood one down the lamp-lit avenue toward the camera, slipping past the
striped market tents, and a racing-green one along the banner street, out
from under a roof, behind the clock tower and off to the right. Two people
walk the square (#610): a don in a black gown comes out from behind the
market tents, along the pavement by the central bed and up the walkway into
the clock tower's shadow, passing a student in tweed on his way down it and
off up the street behind the hall.

```sh
uv run tools/room_anim/oxford/check_camera.py          # street grid + routes over the plate
blender --background --factory-startup \
    --python tools/room_anim/oxford/render_layers.py -- --check --layer all
uv run tools/room_anim/bake_layer.py --base oxford --all      # or one layer by name
uv run tools/room_anim/composite_preview.py --base oxford --seconds 48 \
    --out build/room_anim/oxford/preview.mp4
```

- **Camera from the lines.** The plate is a long-lens, three-point view
  looking ~34 deg down, streets at ~45 deg to it: nothing to eyeball. OpenCV's
  line-segment detector finds the street edges and roof lines in two
  orthogonal families plus the tower's verticals, and a least-squares fit of
  focal length, pitch and roll to all three families' vanishing points
  (principal point at the centre) leaves ~0.4 deg median residual:
  f = 2895 px, pitch 34.4 deg, roll -0.4 deg. Scale: the street lamps (~4 m)
  and the tower's arches (~4.5 m) both give an eye 80 m up. The fit lives in
  `camera_match.py`, pure stdlib, so the Blender camera and uv scripts share
  it; `render_layers.py --check` holds Blender's projection to it (< 0.5 px).
- **Routes in a street frame.** Resampling the plate onto the street plane
  (u along one street direction, v the other) gives a top-down map where the
  flower beds come out as ~20 m squares, a check on the fit, and the routes
  (`routes.json`) are read straight off it in metres.
- **Painted silhouettes, not 3D fits, for occluders.** A 3D tent fitted from
  its eave corners needs an eave height, which was a guess: the car showed
  over the painted canopies. A grid-aligned tower box is seen corner-on
  (190 px wide to the painted spire's 125). Instead each occluder is traced
  on the plate and stood, camera-facing, at the view depth of its footprint
  (`scene.silhouette()`): anything farther is hidden exactly where the paint
  is. They're camera-only, so they cast no phantom shadows.
- **The air-car's look.** Its stock textures (low-res gold on everything)
  smear into an orange blob at 60-90 px, so every material slot gets a clean
  shader; from above the canopy dome is most of the car, and in glossy paint
  it catches the lamps as it passes (as dark glass it read as a hole). Light
  was calibrated against the plate: the rendered empty street is within
  ~0.75-1.6x of the painted one in linear light (the first pass was 2-8x
  darker, and the car came out black).
- **Sharp small heroes.** A car plus its light on the street runs just over
  `bake_layer`'s half-size threshold, which blurred it; `"max_px": null` in a
  layer's `layers.json` entry keeps every frame full size (atlas 4096 wide,
  like the bar patrons').
- **They never crash.** The routes cross, so the loops are locked (48 s is
  twice 24 s) with offsets that put the two cars at the crossing 12 s apart
  every time; `test_room_anim` checks the lock.
- **Pedestrians (#610)** are `walkers.py` proxies (`actors.LOOKS`): at ~40
  px, 1.75 m at 130 m, the silhouette, gait and lamplight carry them, and
  12 fps is plenty. Each comes and goes behind a holdout, never popping in:
  the don from under the south tent (he crosses the avenue there, unseen,
  timed 8 s behind the car) to behind the clock tower; the student from the
  tower to behind the hall's roof, a new `FrontHall` silhouette, stood at the
  hall's street-side wall so the street behind it is hidden and the pavement
  in front isn't. Those two cards stand upright (`scene.UPRIGHT`): the camera
  looks 34 deg down, so a camera-facing card leans toward it at the top and
  a walker crossing one vanished feet first, sinking into the pavement; the
  cars' cards stay as signed off (no car comes within 36 m of either). Paths
  were checked before rendering by projecting a 1.75 m box every metre
  against every silhouette. Their loops lock to
  the avenue car's too (48 s), and phase them to pass each other mid-walkway;
  the student, a metre farther off, is drawn first.

| `oxford/` file | runs in | what |
|---|---|---|
| `camera_match.py` | both | the fitted camera and the street frame, pure math |
| `check_camera.py` | uv | street grid, lamp posts and routes over the plate |
| `scene.py` | Blender | camera, street deck + beds, silhouette holdouts, lights |
| `actors.py` | Blender | the air-car (game mesh, clean shaders, lamps) and its drive; walker looks |
| `render_layers.py` | Blender | one layer per route, by its `kind`; `--check` camera vs `camera_match` |
| `routes.json` | - | each route: kind, uv path, speed; a car's hover + paint, a walker's look |
| `layers.json` | - | loop period, phase, `max_px` |
| `bake_landing.py` | uv | landing pad: sky masks, half-size fills, moon anchors; layer sheet |
| `render_landing.py` | Blender | the Oxford ship lifting off (`../flyover.py`, dusk `lights=`) |
| `landing_layers.json` | - | landing layer timing |

### Oxford landing pad (#593)

A spaceport at dusk: 18 per-hull composites, each framed and zoomed a little
differently, under a purple-to-orange sky with a big moon and a small one.
Every 40 s an Oxford ship (`ships_wcnews/oxship.obj`, the original game's)
climbs out from behind the hangar roofs and away off the left edge, engines
hot, passing in front of the clouds and behind everything on the ground.

```sh
uv run tools/room_anim/oxford/bake_landing.py --debug build/room_anim/oxford/skies.png
# (bake the skies first: render_landing.py reads tarsus's moon from anchors.json)
blender --background --factory-startup --python tools/room_anim/oxford/render_landing.py
uv run tools/room_anim/oxford/bake_landing.py --layers-only
uv run tools/room_anim/composite_preview.py --base oxford --room landing --plate tarsus \
    --seconds 20 --out build/room_anim/oxford/landing.mp4
```

- **Sky.** A smooth painted gradient, and everything else has edges: the sky
  is the edge-free region connected to the top edge. Some composites are
  soft (drone's skyline leaks at the default threshold and the flood reaches
  the tarmac), so the threshold steps down until the flood stops above 80%
  of the height. Holes the ground can't reach (painted clouds, specks) are
  sky: they're kilometres off, so the ship passes in front of them, and the
  fill, which is the plate's own sky at half size, redraws them beneath it.
  Only the moons stay painted over the ship. There are no star tiles: it's
  dusk.
- **Anchor.** The framings differ in zoom as well as position, so the anchor
  is the big moon (`[cx, cy, r]`), found in each composite by RANSAC circle
  fits to the edges inside the sky, where nearly every edge is a moon's
  limb. The moons are chosen as a *pair* (the small one ~half the size, lower
  right, in its expected place), because one circle alone may be a hangar
  arch, and the crisp small moon out-votes the big one's soft limb. Paradigm's
  small moon loses to its arches, so there a lone big moon high in the frame
  is taken and the small one is searched for where the pair puts it. The big
  moon's centre must sit in the top third; lower pairs are arches.
- **Ship.** Rendered on the crater pads' shared stage (`flyover.py`: level
  f = 1024 px camera against tarsus, nav lights, render loop), passing its
  own `lights=`: dusk, a warm sun just set ahead of the camera and a purple
  sky fill. It climbs, so unlike `fly_straight()` its path is a 3D line
  solved from two screen targets at two depths; that projects to a straight
  screen path that stays clear of the moons. From 2 km out it read as a
  bird; receding 5x in depth front-loaded the motion, so it only doubles
  (200 m to 420 m, ~43 m/s). The mesh has the old game's
  afterburner baked in as solid red and yellow flames: `ships.STRIP_MATERIALS`
  drops those faces at import, before the bbox sets the scale.

## Refinery concourse (#584)

The painting looks down from a high balcony into a round atrium: a ring of
floor round a sunken garden, shopfronts and cargo bays round the wall, a
column on a bridge in front. Above, the dome's glass looks out on space and
the refinery. The original game had ships crossing the dome (legacy
`sh0`/`sh1`); now stars drift through all three windows, a Galaxy freighter
crosses far out and a Demon shuttle drops in behind the towers (both *under*
the plate), and the mining base's ore train (#558) comes round the ring
floor and delivers into the cargo bay.

```sh
uv run tools/room_anim/refinery/bake_sky.py --debug build/room_anim/refinery/sky_debug.png
blender --background --factory-startup \
    --python tools/room_anim/refinery/render_sky.py -- --layer all
uv run tools/room_anim/refinery/bake_sky.py --layers-only
blender --background --factory-startup \
    --python tools/room_anim/refinery/render_layers.py -- --check      # camera-match overlay
blender --background --factory-startup \
    --python tools/room_anim/refinery/render_layers.py -- --layer all
uv run tools/room_anim/bake_layer.py --base refinery --all
uv run tools/room_anim/composite_preview.py --base refinery --seconds 48 \
    --out build/room_anim/refinery/preview.mp4
```

**Camera: fit to circles.** No straight floor lines here, but the column and
its cable are vertical at x ~1135 and the floor's rings are symmetric about
it, so that's the lens-shifted principal column and the atrium's axis. The
wall's foot and the ring of floor lamps were fitted as two concentric
circles by least squares. Rings alone can't separate focal length from pitch,
so f is fixed at 1100 px (pitch 9.5 deg down). The check the fit never
saw: the axis's foot lands at y 984, right under the painted column. The cargo bay door
(~120 px, taken as 4.7 m) sets the scale: wall radius 20 m. `scene.to_plate()`
and `plate_point()` map world and plate both ways, so routes and flight paths
are planned in plate pixels.

**Hazy sky.** Round the sun the sky is haze up to luminance ~70, not black.
`sky.window_mask()` (New Con's polygons + per-window threshold, now shared,
with `holes` for painted rocks and ships in the sky) keeps the glare
painted, and `sky.hazy_sky_fill()` fills with the plate's own star-removed
sky at full resolution: `sky_fill()`'s broad average of the masked sky came out
darker than the haze and outlined the mask. Hairlines crossing the sky (the
hanging cable, antennas) are cut back out of the mask as long vertical runs
of bright residual; the mask's opening would erase them. Holes and hairlines
are cut after `solidify()`, whose hole fill would take them back.

**Ships over the dome** use `flyover.py`'s crater-landing helpers (#587):
`hull_nav_lights()` (its `nav_lights()` sized per hull) and
`render_frames()`, the straight-alpha render loop that `flyover.run()` now
calls too. Only the camera (the
plate's, from `scene.py`) and the lights (the painted sun) are the dome's own.

**Ore train on a ring.** `refinery/actors.py` loads `mining/actors.py` by
path (both bases have a `scene` module, so it can't share `sys.path`) and
drives the same tug and hopper along a Chaikin-smoothed curve: 15 m out
round the ring, through the bay door (-38 to -16 deg), then left behind the
wall. The garden (a drum) and the wall beside the door are holdouts, so the
hopper vanishes past the painted jamb. The tug is grimed darker than on the
mining base; the stock yellow glared in this dimmer, browner room.

| `refinery/` file | runs in | what |
|---|---|---|
| `scene.py` | Blender | circle-fit camera, floor ring deck, garden + bay wall holdouts, lights |
| `actors.py` | Blender | the mining ore train on a curved floor path |
| `render_layers.py` | Blender | the ore-train layer; `--check` overlay |
| `layers.json` | - | ore-train loop timing |
| `bake_sky.py` | uv | window mask, hazy fill, star tiles; `--layers-only` bakes the ships |
| `render_sky.py` | Blender | the ships over the dome, straight alpha |
| `sky_layers.json` | - | the ships' loop timing |

## Military concourse (#588)

The original game's military concourse drifted stars across its big window
(the legacy `concourse_stt`/`stb` overlays), flew a fighter pair past it
(`sh0`) and ran a tug towing a flatbed of munitions down the lane (`car`).
All three are rebuilt on the #621 repaint: the painting's own stars drifting
behind the window, two Confed Stilettos (`ships_wcnews/stiletto.obj`)
crossing beyond it, and the tug (`truck.obj`) towing the hopper (`cart.obj`,
gunmetal, racked with missiles) out from behind the armoured ramp and down
the lane at the camera.

```sh
uv run tools/room_anim/military/bake_sky.py --debug build/room_anim/military/sky_debug.png
blender --background --factory-startup \
    --python tools/room_anim/military/render_layers.py -- --check     # camera-match overlay
blender --background --factory-startup \
    --python tools/room_anim/military/render_layers.py -- --layer all
blender --background --factory-startup --python tools/room_anim/military/render_flyby.py
uv run tools/room_anim/bake_layer.py --base military --all
uv run tools/room_anim/military/bake_sky.py --layers-only
uv run tools/room_anim/composite_preview.py --base military --seconds 38 \
    --out build/room_anim/military/preview.mp4
```

- **Camera.** One-point, level and unyawed (the window lattice is square to
  the frame). The walkway's kerb lamps and the lane's orange dashes
  (least-squares fits) meet at (465, 414), on the window's lower transom,
  and lens shift puts it there. The middle emblem's ring foreshortens to
  ~325 x 68 px at y 808, so f = (808 - 414) * 325 / 68 ~ 1880 px; f = 1860
  px. `--check` draws the kerb, dashes, ramp foot, the emblem's ring and
  the train's route over the plate.
- **Scale.** A floor line at X satisfies (x - 465)/(y - 414) = X / eye:
  dashes 1.256, the lamps along the ramp's foot 1.565. An 8 m eye makes the
  lane right of the dashes 2.5 m: room for the 2.2 m hopper, whose missile
  rack clears the ramp-foot gutter 1.1 m right of the dashes.
- **Round the corner.** The train enters from behind the ramp's far end (a
  holdout box whose face is the ramp's foot). Tug and hopper follow one
  filleted `Route` by arc length, so the hopper tracks the tug through the
  bend; headings are unwrapped, or motion blur spins the tug at the +-180 deg
  flip.
- **`trailer.obj` is a wheelless pod.** On the lane it floated like a
  capsule; the wheeled hopper with a procedural missile rack reads as the
  legacy munitions flatbed.
- **Sky.** As on New Con, the drifting tiles draw from the painting's own
  stars (`sky.PaintedStars`). The mask tests the star-removed plate: tested
  raw, every painted star next to the lattice punched a notch into it.
- **Fighters** are a straight-alpha pass from the same plate camera, baked
  by `bake_sky.py --layers-only` (timing in `sky_layers.json`, since
  `bake_layer.py --all` reads `layers.json`) as an `under` layer: the
  lattice and pillar occlude them. The pair flies through the middle row of
  panes and leaves off the right edge. The pass holds its last pose, so a
  pair stopping on the plate would park the trailing wingman in the slot
  beside the pillar.

| `military/` file | runs in | what |
|---|---|---|
| `scene.py` | Blender | camera match, lane deck, ramp holdout, lights |
| `actors.py` | Blender | tug + missile hopper, `Route` (filleted path by arc length) |
| `render_layers.py` | Blender | the munitions-train layer; `--check` overlay |
| `render_flyby.py` | Blender | the Stilettos beyond the window (straight alpha) |
| `bake_sky.py` | uv | window mask, fill, star tiles; `--layers-only` bakes the flyby |
| `layers.json` / `sky_layers.json` | - | loop timing (plate-aware / sky layers) |

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
subdirectory is treated as a base (archetype walkers).

## New Detroit landing pad (#591)

The landing pad is a platform high over a night city, one composite per hull
(`landing_ships/<hull>.png`). Two aircars, the original game's New Detroit
aircar (`ships_wcnews/nd_airca.obj`, drive glow emitting), fly past it:

* `aircar_low` crosses right to left over the drop in front of the pad's
  near rim, close and big (~150 px);
* `aircar_high` crosses left to right above the far side and the hangar.

| `newdetroit/` file | runs in | what |
|---|---|---|
| `render_landing.py` | Blender | camera, lights, the aircar passes (`--layer`, `--frames`) |
| `bake_landing.py` | uv | registers every composite to tarsus -> `anchors.json`; bakes the passes |
| `landing_layers.json` | - | loop period and phase |

```sh
blender --background --factory-startup \
    --python tools/room_anim/newdetroit/render_landing.py -- --layer all
uv run tools/room_anim/newdetroit/bake_landing.py --debug build/room_anim/newdetroit/reg.png
uv run tools/room_anim/composite_preview.py --base newdetroit --room landing --plate tarsus \
    --seconds 23 --out build/room_anim/newdetroit/landing.mp4
```

- **In front of everything.** The composites aren't one painting reframed:
  the pad was rendered from a slightly different camera for each hull and
  the city painted afresh. So nothing goes *behind* anything painted: the
  cars fly between the camera and the pad, 20-25 m above its deck, and
  cover the plate (`under` is false). One pass fits every hull.
- **Anchored by registration.** Each composite frames the pad up to ~8%
  smaller or bigger and ~50 px off tarsus's. `bake_landing.py` finds each
  one's zoom + shift from the hangar block and the near rim only (the
  parked ship and the city differ on every composite): for every zoom, a
  phase correlation of the edges gives the shift, and the best-correlating
  zoom wins. `--debug` shows tarsus red, the composite cyan: grey where they
  agree. Stiletto's camera also moved (the pad's far-left rim still doubles
  by ~50 px), which a zoom + shift can't express; its cars are near enough.
- **Paths from screen targets.** `render_landing.py` places each pass by
  plate pixel and depth along the view (`_screen_point`), so the numbers in
  `PASSES` are where the car is seen, not world metres.
- **Orientation.** `nd_airca`'s canopy is at -Y and its fins and drive glow
  at +Y, so `ships.FIX_EULER` turns it 180 degrees, like the Galaxy.

## Military landing pad (#589)

The military landing pad is a closed hangar bay: no sky, no mouth, nothing
for stars or traffic to show through. What the original game animated here
was its landing lights (the legacy `landing_lbl` overlay blinked the red
lamps set into the bay floor's edge). They're rebuilt as a "rabbit" chase:
each lamp flares in turn from the far end toward the camera, with a red
glow and a spill along the floor, then rests as painted.

```sh
uv run tools/room_anim/military/bake_landing.py --debug build/room_anim/military/lamps.png
uv run tools/room_anim/composite_preview.py --base military --room landing --plate tarsus \
    --crop 300 620 1000 400 --seconds 6 --out build/room_anim/military/lamps.mp4
```

- **No Blender.** A lamp's flare is a point glow, so it's made in 2D: added
  in linear light over the composite and encoded with `bake_layer.encode()`.
  The engine's plain alpha-over reproduces it exactly.
- **One sheet per composite.** The 17 composites are painted separately:
  the 3-5 lamps sit somewhere different in each, and the spacing differs,
  so no single anchor maps them onto each other. Each composite gets
  `anim/landing/<hull>_lights.json`, found and encoded from its own paint.
  The room names it once, as `{plate}_lights.json` (`for_plate()`
  substitutes layer paths too).
- **Finding the lamps.** Hulls carry red too (strakha is red all over), so a
  lamp is a small red blob below the deck line that sits on a line of 3+
  blobs sloping like the bay's edge (dy/dx 0.25-0.6), 140-380 px apart.
  Gothri's lamps are painted dim and orange, so if the strict colour test
  finds no line, a looser one tries again.
- **Sizes.** The painted lamps range from specks to 10 px smudges
  (drayman's include their painted bloom), so flares scale with the lamp's
  radius, clamped to 2.5-5 px. The fade is quick (0.14 s): with a slower
  one every lamp glowed at once, and each frame's sprite spanned the whole
  row.

## Refinery landing pad (#585)

The refinery's hangar opens through one big door under a truss gantry onto
a dark city under the stars, in 18 per-hull composites framed differently.
Stars drift slowly through the door (the legacy `lst` overlay's drifting
points of light), a transport crosses high and far, and a heavy tug
(`ships_wcnews/dd_tug.obj`) lifts off from behind the skyline and climbs
away, all under the plate and anchored to each composite's door.

```sh
uv run tools/room_anim/refinery/bake_landing.py --debug build/room_anim/refinery/doors.png
# (bake the doors first: render_landing.py reads tarsus's door from anchors.json)
blender --background --factory-startup \
    --python tools/room_anim/refinery/render_landing.py -- --layer all
uv run tools/room_anim/refinery/bake_landing.py --layers-only
uv run tools/room_anim/composite_preview.py --base refinery --room landing --plate tarsus \
    --crop 300 120 1000 560 --seconds 44 --out build/room_anim/refinery/landing.mp4
```

- **The door, by its lamps.** Threshold scans leak: the wall above the
  gantry is nearly as dark as the door, and on some hulls the city through
  it is lit. But five lamps hang under the truss's bottom chord in every
  composite, the brightest pinpoints in the upper middle. The anchor comes
  from the largest subset of peaks on one row forming a regular grid (~175
  px pitch, at most five slots; side-column lights on the same row fall
  off it): `[middle lamp x, row y, two pitches]`. If an end lamp is lost,
  the grid is placed so the door's centre is nearest the usual x ~810.
  The composites differ mainly in height (the row sits at y 162-312); the
  door's half width is 290-370 px.
- **The sky inside it** is `bake_crater.find_sky()`'s rim scan on the
  door's crop, from the lamps down: sky until the first sustained bright
  run. That's the pad's edge, a big hull, or the city's lit skyline. The
  crop is 1.12 half-widths wide, past the end lamps; a lit side beam is
  "rock" from its first row and the scan's opening drops the lattice's
  narrow gaps. The painted pinpoints (city lights, the lamps) are cut back
  out, so they stay painted. Everything else is `bake_crater.py`'s: the door
  finder plugs into `bake_skies(where, debug, find=door_sky)`, and the star
  tiles (`_star_seed` 585) and layer bake are the crater pads' own.
- **Ships** are `flyover.run()` passes (its level f = 1024 px camera and
  canonical tarsus, with night lights through `lights=` and
  `flyover.hull_nav_lights()`). Paths are in door units (u across, -1 to +1 lamp to
  lamp; v down, in half-widths), so they land in the doorway on every
  composite. The tug starts below the skyline (v ~1.05) and rises into view
  from behind the buildings; `transprt` gets a 180 deg `FIX_EULER` (engine
  pods at +Y).

| `refinery/` file | runs in | what |
|---|---|---|
| `bake_landing.py` | uv | door anchors from the lamp row, door skies, star tiles, ship sheets |
| `render_landing.py` | Blender | the transport and the tug, straight alpha, canonical door |
| `landing_layers.json` | - | the ships' loop timing |

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
4. A landing pad (per-hull composites) reuses the shared pieces rather than
   copying another base's (#627): `bake_crater.py` outright for a black
   crater sky; otherwise a `<base>/bake_landing.py` that finds its own sky
   and anchor, fills a smooth painted sky with `sky.half_fill()`, draws its
   `--debug` grid with `sky.contact_sheet()` and packs the passes with
   `bake_layer.bake_passes()`; its `render_landing.py` builds ships on
   `flyover.py`'s stage (`lights=` for its own light).

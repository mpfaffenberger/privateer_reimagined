# Original Privateer gun sprites — extraction + mapping (bead np-52d)

Goal was to extract the original **gun projectile bolt** SHAPES to PNG and map
each `GunType` → its bolt. What we actually found changes the conclusion — read
the "Verdict" first.

## Verdict (LOUD): there are no per-gun bolt sprites in the data

The original in-space, colored **blaster-bolt sprites do not exist as discrete,
decodable sprites** in the Privateer data set, and the clean-room format
authority **dpjudas/WCPrivateer has no bolt/projectile decoder at all** — its
space combat is unimplemented (grep for `blast|bolt|projectile|tracer` in
`Sources/` → nothing; `shipArt`/`WCGun` are parsed but never rendered as fire).
The forward-view bolts in the original engine are drawn **procedurally** (scaled
colored points/streaks) — which is exactly what our `src/projectile.cpp` +
`gun.cpp` `tracer_color` pipeline already does. So our current approach is the
correct model, not a stopgap.

Corollary on np-zy9's "*TYPE.IFF carries a SHAPES id": the projectile type files
(`MASSTYPE.IFF`, `MESNTYPE.IFF`, …) carry a 4-byte `PROJ/INFO` = `[u16, u16]`.
The first word is **4 for every energy gun** (3 for `TORPTYPE`) and the second is
a speed-ish value (e.g. Mass `0x03e8`=1000, Meson `0x044c`=1100). A field that is
identical across all energy guns **cannot** select a per-gun bolt sprite — it is a
projectile *class* id (energy vs torpedo) + speed, **not** a SHAPES reference.
`GUNS.IFF` UNIT records are fully consumed by name+ballistics (abuse, velocity,
lifetime, refire, energy, damage) with **no colour or shape field**. So neither
file points at a bolt sprite.

## What IS cleanly decodable per GunType: SHIPART.IFF gun-hardware art

`DATA\OPTIONS\SHIPART.IFF` (`FORM SHAR` → `FORM <class>` → `FORM GUNS` →
`FORM GUNT` → `INFO`(u8 gun index) + `DESC` + `SHAP`) holds, per ship class
(`FIGH`/`MRCH`/`TUG`/`CLNK`), a SHP image **per gun type** — but these are the
**gun device models** (gray-metal cannon, 4-frame: 2 perspective + 2 muzzle-on
views), i.e. the weapon-as-an-object art used by dealer/outfitting/mount displays.
Confirmed by eye and by palette: every gun's frames use the SPACE.PAL gray ramp
(indices ~172–188) — no per-gun colour, so they are NOT bolts. We extract them
anyway because they are the only per-`GunType` decoded sprite set and are useful
for an outfitting/gun-dealer screen later.

## Format (authority: dpjudas/WCPrivateer)

* SHP frames — `Sources/FileFormat/WCImage.cpp` (segment + run + RLE, 1-bit mask;
  `width>500`/`height>500` guard). Mirrored in `tools/extract_gun_sprites.py:decode_shape`.
* VGA palette — `Sources/FileFormat/WCPalette.cpp` (start/count u16, 6-bit RGB
  `<<2`). Mirrored in `load_palette`. Palette used: `DATA\PALETTE\SPACE.PAL`
  (the palette dpjudas uses for space SHAP/VSHP in `ExportCommandlet::ExportIffImages`).
* IFF nav (BE chunk sizes, 16-bit align) — `Sources/FileFormat/FileEntryReader.h`.
* SHIPART layout — `Sources/FileFormat/WCGameData.cpp::LoadShipArt`.

## GunType → gun index → extracted art (FIGH class)

Gun index comes from `GUNS.IFF` UNIT order and is the `GUNT/INFO` byte:

| GunType (ours)    | GUNS idx | short | *TYPE.IFF  | FIGH frames | size      | art colour |
|-------------------|----------|-------|------------|-------------|-----------|------------|
| Laser             | 5        | Lasr  | LASRTYPE   | 4           | 320×152*  | gray-metal |
| MassDriver        | 3        | Mass  | MASSTYPE   | 4           | 320×152*  | gray-metal |
| MesonBlaster      | 1        | Mesn  | MESNTYPE   | 4           | 320×152*  | gray-metal |
| NeutronGun        | 0        | Neut  | NTRNTYPE   | 4           | 320×152*  | gray-metal |
| ParticleCannon    | 4        | Ptcl  | PARTTYPE   | 4           | 320×152*  | gray-metal |
| TachyonCannon     | 7        | Tach  | TCHNTYPE   | 4           | 320×152*  | gray-metal |
| IonicPulseCannon  | 2        | Ionc  | IONCTYPE   | 4           | 320×152*  | gray-metal |
| PlasmaGun         | 6        | Plas  | PLSMTYPE   | 4           | 320×152*  | gray-metal |
| SteltekGun        | 8        | Stel  | (PLSMTYPE/ALIEN) | 4     | 320×200*  | gray-metal |

`*` Frames are full-canvas; the device occupies a ~30–60 px box (cropped in the
contact sheet). "art colour" is gray-metal for ALL guns → **cannot** be used to
sanity-check `gun.cpp` `tracer_color` (there is no per-gun bolt colour in the
data; the tracer colours stay a hand-picked canonical-feel choice, which the data
neither confirms nor contradicts).

## Outputs (all LOCAL-ONLY, gitignored under `/gog_extracted/`)

* `gog_extracted/gun_sprites/<CLASS>_<KIND>_<idx>_<gun>_<frame>.png` — RGBA,
  transparent where the SHP mask is 0.
* `gog_extracted/gun_sprites/manifest.csv` — per-PNG class/kind/idx/gun/size/frames/dom-colour.
* `gog_extracted/gun_sprites/_contact_sheet.png` — 9-gun × frames eyeball preview.

Re-create with: `python3 tools/extract_gun_sprites.py`. The script is code-only
and committed; the pixels are never committed (verified with `git check-ignore`).

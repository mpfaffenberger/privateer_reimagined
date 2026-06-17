# SFX mapping (SOUNDFX.PAK → gameplay events) — USER-GROUND-TRUTH

How every gun + combat/flight/UI event picks its sample. **This mapping is now
derived from the user's own ear**, not an acoustic heuristic: the in-game F7
sound labeler (np-fnl) was used to play and name every extracted clip, and the
labels are committed as the ground truth in **`docs/sound_labels.json`**. The
previous acoustic-crest-factor guesses (np-0ex) are retired.

`sfx.cpp` loads `assets/sfx/original/<name>.wav` (LOCAL-ONLY, gitignored —
regenerated from the user's OWN `SOUNDFX.PAK`) and falls back to the committed
procedural placeholder (or, for guns, the generic `laser_fire`) when a file is
absent. Filenames are the contract; the tables below record which extracted PAK
clip each file was cut from.

## Source pipeline

`tools/extract_soundfx_pak.py` dumps every VOC in PAK order to
`gog_extracted/sfx_voc/sfx_NN.voc` (NN = PAK index); each is converted to
`gog_extracted/sfx_wav/sfx_NN.wav` (44100/mono/s16). **`tools/remap_sfx_originals.sh`**
then re-encodes the chosen clips into `assets/sfx/original/<name>.wav` per the
tables below. Re-run that script whenever `sound_labels.json` changes — it is
the committable, audio-free record of WHAT maps to WHAT.

## Per-gun firing — loud = PLAYER (2D), quiet twin = NPC

The labels show `sfx_09..17` are the *quieter versions* of `sfx_00..08`. We use
the **loud** clip for the player's own guns (played 2D, always audible) and the
**quiet** twin for NPC guns (positional + coalesced, np-3va). `is_player` in
`sfx::gun_fired` picks both the sample and the playback path.

| GunType            | short_name           | player (loud) | NPC (quiet) | user label |
|--------------------|----------------------|---------------|-------------|------------|
| Laser              | laser                | **sfx_05**    | sfx_14      | "laser cannon" |
| MassDriver         | mass_driver          | **sfx_03**    | sfx_12      | "mass driver" |
| MesonBlaster       | meson_blaster        | **sfx_01**    | sfx_10      | "meson blaster" |
| NeutronGun         | neutron_gun          | **sfx_02**    | sfx_11      | "neutron gun" |
| ParticleCannon     | particle_cannon      | **sfx_04**    | sfx_13      | "particle cannon" |
| TachyonCannon      | tachyon_cannon       | **sfx_07**  | sfx_16      | user-confirmed Tachyon (np-4dr) |
| IonicPulseCannon   | ionic_pulse_cannon   | **sfx_00**    | sfx_09      | "ionic pulse cannon" |
| PlasmaGun          | plasma_gun           | **sfx_06**    | sfx_15      | "plasma cannon" |
| SteltekGun         | steltek_gun          | **sfx_08**    | sfx_17      | "steltek gun" |

### sfx_07 -> Tachyon is USER-CONFIRMED (np-4dr)

Resolved. The user confirmed by ear that **sfx_07 IS the Tachyon Cannon**. The
background: sfx_07 was auto-labeled "particle cannon" by the F7 pass, but
**sfx_04** is already the particle cannon and **no clip was labeled "tachyon"**;
`sfx_07` sits *exactly* at Tachyon's slot in gun order (8th gun clip, right
after particle). The user confirmed the inference, so the ambiguity flag is
removed from `src/sfx.cpp` and the boot log. No mapping change — it was already
bound to sfx_07; this only de-flags it. Locked in.

## Combat events — player-vs-NPC variants

The labels distinguish who took the hit, and the damage pass knows the victim's
`is_player`, so `sfx::impact` now carries a `victim_is_player` flag.

| Event           | player victim | NPC victim | user label / note |
|-----------------|---------------|------------|-------------------|
| impact_armor    | **sfx_23**    | sfx_24     | "armor damage taken by player/NPC" |
| impact_shield   | **sfx_25**    | sfx_26     | "shield damage taken by player/NPC" |
| explosion_big   | sfx_27 (both) |            | "ship destroyed" (unchanged) |
| explosion_small | sfx_28 (both) |            | "missile explosion" (was wrongly sfx_30 "asteroid flying by") |
| missile_fire    | sfx_18 (both) |            | "missile launch" (was wrongly sfx_40 "unknown") |

The global impact rate-limiter (~8/sec) and the NPC-gunfire coalescer (np-3va)
are both preserved.

## Flight / UI

| Event         | source     | note |
|---------------|------------|------|
| afterburner   | **sfx_22** | "afterburner" — now a HELD LOOP while TAB is down (brief windup stab on engage, looping voice sustained until release / Flight-mode exit; np-4dr). Was a one-shot on cruise engage; before that, the engine-hum loop. |
| jump          | **sfx_41** → **sfx_42** | two-clip sequence: "41 plays immediately followed by 42 when you press j inside a jump gate". `sfx::jump()` plays sfx_41 then frame-pumps sfx_42 ~3.3s later |
| ui_click      | sfx_34     | "nav cycling" (unchanged) |
| lock_acquired | **sfx_31** | "target locked" (was wrongly sfx_20 "tractor beam") |
| lock_seeking  | *procedural* | no clean "seeking beep" clip is labeled — KEEP the placeholder |
| engine_hum    | *procedural* | SOUNDFX.PAK has **no** idle-engine loop (sfx_22 is the afterburner, now used for cruise) — KEEP the placeholder |

The previous jump/cruise mix-up is now untangled: cruise and jump no longer
share a sample.

## Identified-but-unused clips → FUTURE event hooks

The F7 labels identified several sounds with no current event. Tracked as
discovered-from beads for future wiring:

| Clip   | label | candidate future event |
|--------|-------|------------------------|
| sfx_19 | "Steltek Drone shooting its guns" | Steltek drone enemy gunfire |
| sfx_20 | "tractor beam" | tractor-beam / cargo scoop |
| sfx_21 | "ship flying by" | near-pass whoosh |
| sfx_29 | "missile explosion" (dup of 28) | alt missile boom |
| sfx_30 | "asteroid flying by" | asteroid near-pass |
| sfx_33 | "radar damaged flicker" | radar-system-damaged ambience |
| sfx_36 | "unknown" | — |
| sfx_37 | "send comms (to NPC)" | comms-send blip |
| sfx_38 | "internal system damage taken" | component-damage cue |
| sfx_39 | "dumping cargo" | cargo-jettison |
| sfx_40 | "unknown" | — |

## Canon status

**RESOLVED via human labeling.** `GUNS.IFF` carries no sound-effect index field
(decoded) and the clean-room reference **dpjudas/WCPrivateer** has no gun→sound
table either (`WCGun` has no sound field; gameplay never plays a firing sound),
so canon was unreachable from references — see np-0ex notes. The user's F7
labels are the authoritative source of truth that closes the gap. The single
remaining open question is the **sfx_07 → Tachyon** inference above (plus the
deliberate procedural-fallback choices for `engine_hum` / `lock_seeking`).

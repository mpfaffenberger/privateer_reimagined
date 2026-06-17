# Privateer Ship-AI Data Model (derived facts)

**Source:** Origin's `PRIV.TRE/DATA/AIDS/*.IFF` (clean-room extraction).
**Tool:** `tools/import_privateer_db/aids.py` (re-run: `python3 tools/import_privateer_db/aids.py --summary`).
**Raw dump:** `gog_extracted/ai_dump/privateer_ai.json` — *gitignored derivative*, local only.
This document holds only **derived facts and tables** (same policy precedent as
`docs/privateer_ship_data.json`); no raw game bytes are committed.

Recon confirmed: the original combat brain is **data-driven**. Every NPC pilot
— generic faction mooks *and* named aces — is a tiny IFF blob of tuning numbers
plus a maneuver bytecode. There is no per-pilot C code. Our job is to replace
our hand-rolled magic constants (`src/ship_ai.cpp`) with these real values.

> **Confirmed vs Inferred.** Anything tagged **[C]** is a structural fact read
> directly from the bytes. Anything tagged **[I]** is an *inference* from
> cross-pilot variance / naming and needs `PRCD.EXE` disasm or playtest to
> confirm. Don't hardcode an **[I]** as gospel.

---

## 0. CONFIRMED AI MODEL (live playtest + GameFAQs) — supersedes stale labels

**Read this first.** Earlier sections (§7.9, §9.4-9.8) carry some *wrong field
labels* that were inferred from static disasm and later REFUTED by live
experimentation (debugger/patch-and-relaunch on the user's own copy) and by the
community FAQ (gamefaqs WC:Privateer FAQ, §5.1 "Morale and Experience"). Where
this section disagrees with a later one, **this section wins**. The stale labels
are being purged; until every inline mention is fixed, trust §0.

### 0.1 The range stack (all in raw Privateer world units, `magnitude>>8`)

| Tier | Value | Meaning | Evidence |
|------|-------|---------|----------|
| Sensor / **awareness** | **15000** (`0x3a98`) | contact appears on radar; **this is what wakes the AI and triggers engage** | static cull loop `0x03d5b2` + live (NPC detected & closed at ~10k) |
| Comms chatter (**`f1`**) | **1500** | **taunt/hail range** — NPC barks when target is within this. **NOT detection.** | live: raising `f1` made taunts fire earlier; lowering it did NOT change when it noticed/engaged. Consumer `0x1ce8c` is the comms-bark check, mislabeled "detect" |
| Pursue → attack-run | **1000** | APPROACH switches from lead-pursuit to a timed firing run | APPROACH leaf `0x397c8` (§9.4.2) |
| **Break-off (`f0`)** | **600 + both hull radii** | **too close → DISENGAGE: turn away, afterburn out, loop back for another pass.** NOT gun range. | live: `f0=5000` ⇒ peeled off at ~5 km. Consumer `0x1d1f8` compare `dist<(f0+radii)` fires the break-off, not the guns |
| Gun fire | per-gun `range_m` + firing arc | actual shooting, during the attack-run window | gun stats + attack-run ticks 76→153 (§9.4.2) |

Scale: engine works in `2^8` fixed-point; values are **raw world-unit radii** —
there is **no ×10** anywhere (§7.10). We adopt these **1:1** in `new_privateer`
(our ship speeds are already Privateer-canonical).

### 0.2 The two-axis pilot model (GameFAQs §5.1, confirmed against CNST variance)

Privateer rates every AI pilot on **two independent axes**:

| Axis | FAQ range | Effect (FAQ wording) | CNST field(s) | Direction |
|------|-----------|----------------------|---------------|-----------|
| **Experience** | rookie / pro / ace | *"evade more attacks, better accuracy"* | **`f3`** (evade-jink amplitude) + **`f2`** (skill/accuracy tier) | higher = more evasion + accuracy. pirate/ace `f3=75`, merchant `f3=30` |
| **Morale** | timid / confident / fanatical | *"higher morale = less likely to retreat, attacks more; fanatical may kamikaze / head-on"* | **`f6`** | **LOW = fanatical** (Kilrathi `f6=64`, fight-to-death), **HIGH = timid** (merchant `f6=128`, flees). `f6` = *caution* |

**`f6` direction is now resolved by lore + faction values** (Kilrathi fanatical =
64, merchant timid = 128). The earlier live `f6` pokes that felt random WERE
noise — `f6` has no traceable resident consumer (§7.13.5), so poking it in the
debugger did nothing; the model comes from the FAQ + the canonical per-faction
values, not from those pokes.

### 0.3 Corrected CNST field roles

| Field | Role (CORRECTED) | Default | Was wrongly called |
|-------|------------------|---------|--------------------|
| `f0` | **break-off / disengage range** (+ hull radii) | 600 | "engage / gun range" |
| `f1` | **comms chatter / taunt range** | 1500 | "detection / awareness range" |
| `f2` | experience/accuracy skill tier | 45 (elite 60, Kilrathi 40) | — |
| `f3` | **maneuver-jitter / evasion gain** (experience) | 75 (merchant 30) | "pursuit tenacity" |
| `f6` | **morale / caution** (low=fanatical, high=timid) | 76 (Kilrathi 64, merchant 128) | "flee threshold" (right idea, wrong direction) |

`f4`/`f5`/`f7`: still `[I]`/unknown — no confirmed consumer. Awareness/engage is
gated by **sensor range (15000) + faction stance**, NOT by any CNST field.

---

## 1. File taxonomy (DATA/AIDS/, 59 IFF files)

`aids.py` dispatches on the root FORM type:

| Root FORM | Meaning | Count | Handled |
|-----------|---------|-------|---------|
| `AIDS`    | a combat pilot (INFO + CNST + MNVR) | 47 | **decoded** |
| `ATTD`    | `ATTITUDE.IFF` — galaxy reaction tables | 1 | **decoded** |
| `COND`    | `MANEUVER.IFF` — morale-gated default scripts | 1 | **decoded** |
| `BUSR`    | base comm/mission chatter (`BASE_*.IFF`) | 10 | skipped (not AI) |
| `PUSR`    | special-pilot dialogue (`SSCOUT.IFF` = Steltek) | 1 | skipped (not AI) |
| —         | `MFDFACES.IFF` — MFD comm-face atlas | 1 | skipped (not AI) |

The `BUSR`/`PUSR`/face files live in this directory but are comm/dialogue
assets, not flight AI. They belong to a future comms importer.

---

## 2. Combat pilot — `FORM AIDS`

```
FORM AIDS
  INFO  <7 bytes>     personality / affiliation / refs   [layout partly I]
  CNST  <16 bytes>    8 × LE u16 skill-tuning vector      [C: structure]
  MNVR  <N bytes>     maneuver bytecode — UNDECODED        [C: preserved verbatim]
```

- **[C]** `CNST` is **optional**. Four pilots ship INFO+MNVR with *no* CNST
  chunk (`CON_DP`, `KIL_DA`, `MER_AP`, `REIS`). These **inherit the default
  vector from `MANEUVER.IFF`** (`COND`/`CNST` = `[600,1500,45,60,2,2,76,0]`).
  This is real engine fallback behaviour, not a parse miss.

> ###  CORRECTED MODEL SUMMARY (read this first — live-test ground truth, §11)
>
> **[CORRECTED, see §0 — source of truth.]** Several CNST labels were corrected
> by live testing and the GameFAQs WC:Privateer FAQ §5.1. The disassembly
> evidence (§7.9–§7.13) is all still valid — only the *purpose* labels changed.
> The correct picture:
>
> **The range stack (distinct same-scale constants, no ×10 anywhere):**
> | range | value (world units) | what it actually gates | tag |
> |-------|---------------------|------------------------|-----|
> | **SENSOR / DETECTION / AWARENESS** | **15000** (`0x3a98`) + faction stance | the AI *waking up* and committing to engage; this is what the player sees as "it noticed me at ~10k" | **[L]** |
> | **COMMS / TAUNT chatter** (`f1`) | 1500 (drone 1800, Confed 2100) | *when the NPC barks a taunt/comm line*. **NOT** detection, **NOT** a combat gate | **[L]** |
> | **BREAK-OFF / DISENGAGE** (`f0`) | 600 + selfR + targetR | too close → turn away, afterburn out, loop back. **NOT** gun range | **[C]+[L]** |
> | **Pursue → attack-run switch** | 1000 | lead-pursuit beyond, timed attack-run within (§9.4.2) | **[C]** |
> | **Gun fire** | per-gun `range_m` + arc | actual shooting, during attack-run ticks 76→153 | **[C]** |
>
> **Other dials (two-axis pilot model, FAQ §5.1):** *experience* (rookie/pro/ace)
> = `f3` evade-jink gain (pirates 75 jerky, merchants 30 smooth) **[C]** + `f2`
> skill/accuracy tier; *morale* (timid/confident/fanatical) = **`f6` caution**:
> **LOW = fanatical** (Kilrathi 64, fight-to-death), **HIGH = timid** (merchant
> 128, flees early). `f6` maps to a flee tendency **[I tuning]** — the old
> "aggression / attack-commitment, do-not-wire-to-flee" reading (from noisy live
> pokes) is itself **superseded by §0**; `f6` has no traceable resident consumer
> (§7.13.5), so the model comes from the FAQ + per-faction values, not the pokes.
>
> **Do NOT** call `f1` "detection/awareness/combat-commit" — that is the sensor
> sphere (15000), not `f1` (§0).

### 2.1 INFO (7 bytes) — partly inferred

Raw bytes preserved as `info_raw`/`info_hex`. Observed layout:

| Offset | Values seen | Reading |
|--------|-------------|---------|
| `[0]` | 0,1,2,3,6,8 | **[I]** faction / affiliation index |
| `[1]` | 0,1,2 | **[I]** sub-class / role |
| `[2]` | 0,1,2 | **[I]** variant |
| `[3]` | `0x80`(128) default, `0x33`(51) on aces, `0x4c`(76) | **[I]** base morale / personality |
| `[4..6]` | almost always `00` (rare `01` at `[4]`) | **[I]** refs / flags, mostly padding |

Marked **[I]** wholesale — the disasm pass should re-label these. Bytes are
preserved verbatim so re-labelling needs no re-extraction.

### 2.2 CNST — the skill vector (8 × LE u16)

This is the payload. Cross-pilot table (mooks vs aces), variance is the key:

```
pilot      f0   f1    f2  f3  f4 f5  f6   f7
BOU_*      600 1500   45  75   3  3  102   0     bounty hunters
KROIZ      600 1500   45  75   3  3  102   0     ace (Kilrathi mercenary)
GARVICK    600 1500   45  75   3  3  102   0     ace
RIORD1     600 1500   45  75   3  3  102   0     ace (Roman Lynch's man)
CONFED1    600 1500   45  40   3  3  102   0     Confed patrol
CON_AA     600 2100   45  60   2  2   76   0     Confed escort (long sensor)
PIR_*      600 1500   45  75   2  2   76   0     pirates
MIL_*      600 1500   45  40   2  2   76   0     militia
MIGGS      600 1500   45  55   2  2   76   0     named pirate
SEELIG     600 1500   45  50   2  2   76   0     named
KIL_*      600 1500  40-45 40-60 2 2  64   0     Kilrathi
RETRO1/2   600 1500   45  60   2  2   64   0     Retros
COM_*      600 1500   45  60   2  2   64   0     (Confed militia variant)
SDRONE     600 1800   45  40   2  2   64   0     scout drone (long sensor)
MER_*      600 1500   60 30-60 3  3  128   0     merchants
FORGE/TOTH 600 1500   60  30   3  3  128   0     named (heavy/cap escorts)
RHOMBUS    600 1500   60  50   3  3  128   0     named
```

**Field variance (the labelling key):**

| Field | Status | Range | Reading |
|-------|--------|-------|---------|
| `f0` | **[C] CONSTANT** = 600 | **world units** | **[C]+[L] BREAK-OFF / DISENGAGE radius** — [CORRECTED, see §0: this is NOT gun range]. disasm-confirmed (§7.9): `dist < (f0 + shipR + targetR)<<8` in a 2^8 fixed-point world space; **live-confirmed (§11): `f0=5000` made the NPC break off at ~5 klicks, small `f0` made it loiter in close**. Too close → turn away, afterburn out, loop back. Gun fire is gated separately by per-gun `range_m` + arc during the attack-run |
| `f1` | **[C] VARY** 1500 / 1800 / 2100 | **world units** | **[L] COMMS / TAUNT-CHATTER RANGE — NOT detection.** disasm-confirmed compare `dist <= f1` is real **[C]** (func `0x1ce8c`) but its **purpose is comms**: live test (§11) showed raising `f1` to 8000 made the NPC fire its taunt/comm bark much earlier while still closing, while `f1`=500/12000 did **not** change when it detected or engaged. Detection/awareness is the **sensor sphere (15000)**, not `f1`. Drone 1800 / Confed escort 2100 = longer taunt range |
| `f2` | **[I] VARY** 40 / 45 / 60 | — | **[I]** weapon/maneuver skill tier — correlated with `f4`/`f5` (elites=60, Kilrathi=40, baseline=45). No independent PRCD.EXE consumer found in this trace; see §7.13 |
| `f3` | **[C] VARY** 30…75 | — | **[C] MANEUVER JITTER / EVADE-GAIN (experience axis, FAQ §5.1)** — read in the per-tick steering/maneuver update (§7.13). Higher `f3` → larger RNG perturbation applied to maneuver amplitude (object+0x18). Pirate/ace 75 = erratic/evasive, merchant 30 = tame. Pairs with `f2` as the experience (rookie/pro/ace) axis. **Not** a pursuit-tenacity dial |
| `f4` | **[I] VARY** 2 / 3 | — | **[I]** paired with `f5`. "burst/evasion skill tier": elites = 3, rank-and-file = 2. Used by the same subsystem as `f2`; no standalone consumer found (§7.13) |
| `f5` | **[I] VARY** 2 / 3 | — | **[C] always == `f4`** — paired count / burst tier |
| `f6` | **[C] VARY** 64 / 76 / 102 / 128 | — | **MORALE / CAUTION (morale axis, FAQ §5.1)** — [CORRECTED, see §0]. **LOW = fanatical** (Kilrathi 64, fight-to-death), **HIGH = timid** (merchant 128, flees early). The per-faction ordering IS a clean monotone caution ladder. **No code consumer of CNST+0x0C was found** (resident + carved overlays exhausted, §7.13.5), so the *mechanism* is **[PENDING]** a dynamic trace, but the *role* is resolved by the FAQ + canonical faction values. The earlier live-poke "aggression/attack-commitment" reading was **noise** (no consumer to poke) and is superseded. Maps to a flee HP threshold **[I tuning]** |
| `f7` | **[C] CONSTANT** = 0 | — | reserved / padding |

The standout fact (**CORRECTED, see §0**): `f6` is the **morale / caution** axis
of the two-axis pilot model (GameFAQs WC:Privateer FAQ §5.1). **LOW = fanatical**
(Kilrathi 64 fight-to-death), **HIGH = timid** (merchant 128 flees early) — a
clean monotone per-faction caution ladder. We DO wire it to a flee tendency
(§0.2). The intermediate "aggression/attack-commitment, do-not-flee" reading
(from noisy debugger pokes against a field with no resident consumer) is
superseded. The baseline numbers 64/76/102/128 are real **[C]**; the flee
mapping itself is **[I]** tuning since `f6` has no traceable consumer (§7.13.5).

### 2.3 MNVR — UNDECODED (needs PRCD.EXE disasm)

Preserved **verbatim** (hex + sha1) — **not** interpreted. Instruction lengths
are unknown until `PRCD.EXE` is disassembled, so any "opcode" split now would
be a guess. What we *can* say without decoding:

- **35 unique** MNVR scripts across 47 pilots + 3 morale tiers (many pilots
  share a script verbatim — see `scripts_by_sha1` in the JSON).
- **Lead byte** (probable entry opcode) frequency:
  `0x0e`×32, `0x0d`×7, `0x0f`×5, `0x0b`×2, `0x09`×1.
  The `0x0d / 0x0e / 0x0f` cluster strongly suggests a small family of
  related entry instructions (script length differs: 176B vs 191B vs 199B).
- **Global byte-value frequency** (head-start operand/opcode census; *not*
  instruction-aligned): `0x00` dominates (operands/padding), then
  `0x04, 0x02, 0x03, 0x11, 0x20, 0x06, 0x12, 0x80, 0x19, 0x13, 0x0a`.

→ **Track 2 (Ghidra on `PRCD.EXE`)** consumes this: find the bytecode
interpreter, map `0x0d/0x0e/0x0f/0x0b/0x09` and operand widths, then the
verbatim hex in the JSON decodes 1:1.

---

## 3. `ATTITUDE.IFF` — `FORM ATTD`

The galaxy-wide reaction tables.

```
FORM ATTD
  AROW × 9   <9 bytes each>   9×9 reaction matrix       [C]
  DISP       <27 bytes>       disposition table          [C bytes / I meaning]
  FLNG       <8 bytes>        4 × LE i16 feeling deltas   [C]
  THRD       <6 bytes>        3 × LE u16 thresholds       [C]
  CNST       <7 bytes>        reaction tuning bytes       [C bytes / I meaning]
```

### 3.1 AROW reaction matrix (9×9)

Codes **[I]**: `8` = ally (always the diagonal/self), `7` = neutral, `6` = hostile.

```
      c0 c1 c2 c3 c4 c5 c6 c7 c8
 r0:   8  8  8  7  8  7  7  7  7
 r1:   8  8  8  6  8  6  6  6  6
 r2:   8  8  8  6  8  6  6  6  6
 r3:   6  6  6  8  6  6  6  6  6
 r4:   8  8  8  6  8  6  6  6  6
 r5:   6  6  7  6  6  8  7  7  6
 r6:   6  6  6  6  6  6  8  6  6
 r7:   7  7  7  7  7  7  7  8  7
 r8:   6  6  6  6  6  6  6  6  8
```

**[I] Behavioural reading of each row:**
- `r0`: friendly with the lawful cluster `{0,1,2,4}`, **neutral** (not hostile)
  to everyone else → likely **Civilian**.
- `r1,r2,r4`: identical — mutually allied lawful cluster, hostile to all
  outlaws → **Confed / Militia / Merchant** (order TBD).
- `r3,r6,r8`: hostile to *everyone* but self → loner aggressors
  → **Retro / Kilrathi** (and one extra, see mismatch below).
- `r5`: mostly hostile, neutral to `{2,6,7}` → **Pirate**.
- `r7`: neutral to all, allied only to self → **Hunter / Mercenary**.

### 3.2  Faction-count mismatch vs `src/faction.h`

`ATTD` carries **9 factions**; our `enum class Faction` has **8**
(`Civilian, Merchant, Confed, Militia, Hunter, Pirate, Retro, Kilrathi`).
Origin splits one faction we merged (most likely a Confed-vs-Militia or a
distinct 9th "loner" row — possibly Steltek/drone or an unaligned slot).

**Action:** do **not** reorder `faction.h` to match AROW until the index→name
map is EXE-confirmed. The tentative mapping above is **[I]** only. The
extractor stores the raw 9×9 so a confirmed map needs no re-extraction.

### 3.3 FLNG / THRD / CNST

- **[C]** `FLNG` = `[-77, -26, +25, +76]` (LE i16) — signed reputation/feeling
  deltas. Symmetric-ish pairs (`-77/+76`, `-26/+25`) **[I]** = the
  reward/penalty applied to a faction's opinion on a friendly/hostile act.
  Compare our `faction::apply_player_kill` swing magnitudes.
- **[C]** `THRD` = `[50, 150, 100]` (LE u16) — reaction thresholds **[I]**
  (e.g. neutral→hostile / hostile→attack gates).
- **[C]** `CNST` = `[6, 10, 40, 10, 20, 30, 50]` (7 bytes) — reaction tuning
  **[I]** (cooldowns / decay / aggro radius). Distinct from a pilot CNST.

---

## 4. `MANEUVER.IFF` — `FORM COND`

The default + morale-gated maneuver scripts every pilot falls back to.

```
FORM COND
  CNST  <16 bytes>   default skill vector = [600,1500,45,60,2,2,76,0]   [C]
  FORM MORL  (×3)    one per morale tier:
    INFO <1 byte>    morale tier index (0, 1, 2)                        [C]
    MNVR <191 bytes> that tier's maneuver bytecode (verbatim)           [C]
```

**[C]** Three morale tiers (`0`, `1`, `2`), each with its **own** MNVR script.
This is the morale-gating mechanism: a pilot's current morale selects which of
the three scripts drives its maneuvering. All three scripts here lead with
`0x0e` (same family as most pilot MNVRs). Script semantics deferred to Track 2.

**[I]** Tier `0`/`1`/`2` ≈ low / normal / high morale (or routed / steady /
confident). Direction of the ordering is an inference until decode.

---

## 5. Mapping to `src/ship_ai.h` / `ship_ai.cpp`

Which of our **hand-rolled magic constants** get **real Privateer values**:

| Our constant (current value) | Replace with | Confidence |
|------------------------------|--------------|------------|
| `k_flee_threshold = 0.30f` (global) | **derive from `f6`** (morale/caution axis, §0 / FAQ §5.1): LOW `f6`=fanatical (Kilrathi 64 → flees ~0.05 hp), HIGH `f6`=timid (merchant 128 → flees ~0.5 hp). Clean monotone map. | **[I tuning]** — role resolved by FAQ + faction values; no resident consumer found (§7.13.5), so the HP-threshold formula is our tuning |
| `k_engage_cos_threshold = 0.966f` (cos15°, global firing arc) | derive accuracy/arc from CNST `f2`/`f3` skill tier | **[I] weak** — needs MNVR/EXE confirm |
| `k_break_distance_m = 150` | **CNST `f0` (600) + selfR + targetR** = break-off / disengage radius (§0). NOT gun range. | **[C]+[L]** — disasm + live confirmed (§7.9/§11) |
| `k_break_duration_min/max`, `k_firing_max_min/max` (3–8s / 3–6s) | MNVR bytecode timings (per morale tier) | **deferred** to Track 2 (MNVR decode) |
| comms / taunt-chatter range | CNST `f1` (1500 base; drone 1800; Confed escort 2100) **world units** | **[L]** — `f1` is the **comms/taunt range, NOT detection** (§11); the `dist <= f1` compare is real (§7.9) but gates the comm bark |
| perception / detection / awareness range | **sensor sphere = 15000** (`0x3a98`) + faction stance — **NOT `f1`** | **[L]** — live test (§11): NPC detected & closed from ~10,000u, far beyond `f1`=1500. Awareness = being a contact inside the 15000 sensor sphere |
| break-off / disengage radius (NOT gun range) | CNST `f0` (600) + hull radii, **world units** | **[C]+[L]** — disasm-confirmed (§7.9, `dist < (f0+R1+R2)`) and live-confirmed (§11, `f0=5000` → break off at ~5 klicks). Gun fire is separate: per-gun `range_m` + arc (§0) |
| `AIPersonality::Coward` boolean (binary) | **make it a scalar from `f6`** (§0): low=fanatical → never flees, high=timid → flees early. Drop the binary enum, read the number. | **[I tuning]** — morale role resolved by FAQ (§5.1); no `f6` consumer found (§7.13) |
| `faction.h` stance matrix (8×8, symmetric) | `ATTD` `AROW` (9×9) once index map confirmed; also `FLNG`/`THRD` for rep deltas/gates | **[C] structure / [I] index map** |

**Cheap-but-huge win (CORRECTED, see §0):** CNST `f0` (break-off / disengage,
600 + radii) is both disasm- and **live-confirmed** and ready to wire — it is
**not** gun range. `f1` (1500) is the **comms/taunt range** — wire it to
comm-bark distance, **not** perception. Detection/awareness is the **sensor
sphere (15000)** gated by stance, independent of `f1`. `f3` is the
**maneuver-jitter / evade-gain** dial (experience axis, FAQ §5.1) — not a
break-off or pursuit switch. `f6` is the **morale / caution** axis (FAQ §5.1):
LOW=fanatical (Kilrathi 64), HIGH=timid (merchant 128), mapped to a flee HP
threshold **[I tuning]**. **No code consumer of `f6` was found** in the resident
image or the carved VROOMM overlay pool (§7.13.5: 415 raw u16 occurrences, none
in a plausible CNST block, no surviving immediate ref, no `+0x0C` indirect
read), so the *mechanism* stays **[PENDING]** a dynamic trace while the *role* is
resolved by the FAQ + faction values. MNVR bytecode timings/
maneuvers are the separate Track-2 follow-up.

---

## 6. Track 2 hand-off — MNVR opcode decode (Ghidra)

Deferred deliberately (we do **not** guess opcodes). Starting facts for the
disasm:

- 35 unique scripts; lengths 120 / 176 / 191 / 199 bytes.
- Entry opcodes cluster `0x0d / 0x0e / 0x0f` (+ rare `0x0b`, `0x09`).
- `MANEUVER.IFF` gives a clean 3-script morale-tier set to diff against.
- Full verbatim hex for every script lives in the (gitignored) JSON dump.

Find the bytecode interpreter in `PRCD.EXE`, recover opcode→operand-width, and
the verbatim hex decodes 1:1 into readable maneuver scripts.

---

## 7. MNVR Bytecode — DECODED (Track 2, `PRCD.EXE` disasm)

**Status: PARTIAL.** The interpreter subsystem, the loaders, the in-memory
struct and the *runtime maneuver model* are **located and confirmed [C]**. The
per-opcode operand widths and exact maneuver semantics are **only partially**
resolved — the format is **not** the flat opcode-VM we assumed in §6; it is an
**object-serialization stream** (Borland C++), which is a deeper decode. This
section records what is solid, what is inferred, and exactly where the next
person picks up. Nothing here is guessed-as-gospel.

> **Provenance / legality.** Everything below is a *derived fact* read from the
> user's own legal GOG `PRCD.EXE`. The Ghidra project, the EXE copy, the carved
> overlay blobs and all decompiler/listing output are **gitignored** under
> `re/` (verified with `git check-ignore`) and were never committed. Only this
> derived documentation (and the code-only helpers in `tools/ghidra/`) is
> tracked. Same OpenMW-style policy as the rest of the repo.

### 7.1 Tooling (reproducible)

- **Ghidra 12.1.2** (`brew install ghidra` — it is a *formula* now, not a cask;
  pulls `openjdk@21`). Headless: `…/libexec/support/analyzeHeadless`.
- Supplementary 16-bit disassembly via **capstone** (`CS_MODE_16`) for quick
  reads of overlay code Ghidra's MZ loader does not map.
- Helpers committed under `tools/ghidra/` (code only): `carve_overlay.py`
  (extract + capstone-disassemble a file-offset window), `DecompDump.java`
  (Ghidra script: disassemble + decompile a list of function offsets).

### 7.2 The Borland overlay problem (SOLVED enough to read the code)

`PRCD.EXE` is a 16-bit real-mode MZ with **Borland VROOMM overlays**
(`"Runtime overlay error"` string @ file `0xb14a`; INT 3F overlay-manager
stubs). MZ header math:

| Field | Value | Meaning |
|-------|-------|---------|
| `e_cparhdr` | 2784 para | header = **44544 B** (load module starts here) |
| `e_cp`/`e_cblp` | 1024 / 352 | resident image ends at file **524128** |
| `e_crlc` | **9062** | relocations @ `0x3e` |
| file size | 937760 | **overlay pool = bytes 524128 … 937760** (413632 B) |

**Key finding:** Ghidra's `MzLoader` maps only the **resident** image, so the
entire AI subsystem (IFF loaders + maneuver runtime) is **invisible** by
default — it lives in the **overlay pool**. The IFF chunk tags appear there as
`push dword 'AIDS'/'INFO'/'CNST'/'MNVR'` immediates (`66 68 ··`), i.e. they are
**code**, not embedded data.

**Reproduction that works:** carve a window of the overlay pool to a flat file
and import it raw as `x86:LE:16:Real Mode` at base 0 (intra-overlay near
jumps/calls are base-independent, so they resolve correctly; only far
`9a seg:off` calls into resident helpers stay symbolic). Two windows used:

- `re/ovr_ai.bin`  = file `0x80000…0x88000` — the **maneuver runtime**.
- `re/ovr_load.bin` = file `0xb0000…0xc8000` — the **IFF/AIDS loaders**.

### 7.3 Loaders & the on-disk → in-memory path  **[C]**

Exact file offsets (xref via the `push dword <tag>` immediates):

| Loader | FORM | file off | reads |
|--------|------|----------|-------|
| AIDS pilot | `FORM AIDS` | `0xb36d4` (wrapper `0xb362b`) | `INFO`, `CNST` @`0xb37f1`, `MNVR` @`0xb38f6` |
| Attitude | `FORM ATTD` | `0xb56c8` | `AROW`×2, `DISP`, `THRD`, `FLNG`, `CNST` |
| Maneuver | `FORM COND` | `0xc3640` | `CNST`, `MNVR` @`0xc3736`, `MORL` @`0xc379a` |

The **MNVR loader** (`0xb38f6`, confirmed by disasm) does:
`IFF_find('MNVR') → heap-alloc → copy chunk verbatim → store far ptr` into the
pilot-definition struct:

```
pilotdef + 0x0f : MNVR script far pointer (offset)   <- raw bytecode, VERBATIM
pilotdef + 0x11 : MNVR script far pointer (segment)
pilotdef + 0x13 : built MNVR maneuver-object ptr (low tier)   (zeroed @0xb363f)
pilotdef + 0x15 : built MNVR maneuver-object ptr (high tier)  (zeroed @0xb3644)
pilotdef + 0x17 : CNST skill-vector block near-ptr           (zeroed @0xb3649)
```

**CNST loader** (`0xb37f1`, confirmed by disasm, this session): `IFF_find('CNST')
→ malloc(0x10) → store near-ptr at pilotdef+0x17 → read 6×u16 (f0..f5) into
block[0,2,4,6,8,0xa] and a final dword (f6|f7<<16) into block[0xc]`. So the
8×u16 CNST lands in a **16-byte heap block** with **`f0 @ block+0x00`,
`f1 @ block+0x02`** … `f5 @ block+0x0a`, `f6 @ block+0x0c`. Values are stored
**raw/unscaled** at load (no ×N applied here). If the `CNST` chunk is absent the
block-ptr stays 0 and consumers fall back to the global default vector
(§7.9). This corrects an earlier note that called `+0x17` an "AI runtime field".

**Consequence (important):** the engine consumes the MNVR bytes **byte-for-byte
unchanged** — so the verbatim hex in `privateer_ai.json` *is* exactly what the
interpreter sees. Our JSON is a faithful oracle.

### 7.4 The runtime maneuver model  **[C] structure / [I] method names**

Decompiled the maneuver dispatch (overlay `0x83xxx`). Privateer's combat brain
is **object-oriented**, not a flat VM:

- An AI actor holds a **3-slot pointer array** (`new ptr[3]`, builder
  `FUN_336b`) — **three slots == the three morale tiers** (this *matches*
  `MANEUVER.IFF`'s three `FORM MORL`). The current morale selects the slot.
- Each maneuver is a **15-byte C++ object** (`operator new(0xf)`) with a vtable.
  Observed virtual slots (offsets into the vtable):
  `+0x04` init, `+0x08` ctor, **`+0x0c` condition-test → bool**,
  **`+0x14` update/tick**, `+0x18` (state), `+0x1c` suspend/leave,
  `+0x20` activate, `+0x24` destroy. (Method *names* are **[I]** from call
  sites; the slot offsets are **[C]**.)
- The per-tick dispatch loops the ≤3 slots, runs `+0x0c` (does this maneuver's
  trigger fire?) and on match runs `+0x14` (drive the ship this frame). Base
  vtable `0x20d`, a derived class `0x235` (ctor chaining observed).

**Dispatch is registry-based, NOT a switch.** Verified there is **no** indexed
jump-table (`jmp [bx+tbl]`) and **no** `cmp al,<opcode>` chain anywhere in the
AI overlays. The lead byte of a MNVR script is therefore a **maneuver class
ID** consumed by a streamable-class factory (the bytecode is a *serialized
object tree*), which is why §6's "opcode → fixed operand width" framing did not
pan out — operand layout is per-class deserialization.

### 7.5 Opcode / class-ID table (what we can state honestly)

From the 35 verbatim scripts (clean-room, byte-level) cross-checked against the
structure above. **[C]** = byte-level fact, **[I]** = inference, **[?]** = open.

| Byte | Role | Evidence | Behaviour |
|------|------|----------|-----------|
| `0x0e` | **root maneuver-list class** (most common entry) | **[C]** lead of 32/47 pilots; opens the shared 32-byte prologue | top-level "combat behaviour" container **[I]** |
| `0x0d` | root class (variant) | **[C]** lead of 7 | sibling container, shorter scripts **[I]** |
| `0x0f` | root class (variant) + appears inline | **[C]** lead of 5; also mid-script | a maneuver class used both as root and child **[I]** |
| `0x0b` | root class (rare) | **[C]** lead of 2 | container variant **[?]** |
| `0x09` | root class (rare) | **[C]** lead of 1 (`SDRONE`, simplest 120-B script) | minimal container — best decode target next **[I]** |
| `0x06`,`0x07` | child maneuver class(es) | **[C]** always cluster mid-script (`… 06 07 07 06 06 06 …`) | a paired "approach/attack" sub-block **[I]** |
| `0x0a` | child class | **[C]** frequent | sub-maneuver **[?]** |
| `0x0c` | child class | **[C]** Kilrathi-only in sample | aggressive sub-maneuver **[I weak]** |
| `0x11`,`0x12`,`0x13` | child classes (most frequent non-zero) | **[C]** dominate the body | the actual steer/throttle/fire maneuvers **[I]** |
| `0x19` | **list terminator / loop** | **[C]** every combat script ends `19 19 19` | end-of-tier / restart **[I strong]** |
| `0x00` | padding / zero operand | **[C]** 4224 occurrences | operand fill **[C]** |
| `0x01`,`0x02`,`0x03`,`0x04`,`0x08` | small scalar operands | **[C]** | counts / tier values **[I]** |
| `0x10`,`0x20`,`0x40`,`0x80` | bit-flag operands | **[C]** single-bit values | maneuver flags **[I]** |

Recurring 3-byte operand groups seen at class boundaries: `03 03 03`,
`04 04 04`, `06 06 06`, `0f 04 04`, `19 19 19` — **[I]** these look like
`<class-id> <param> <param>` triples (a child record header), consistent with
the serialized-tree model, but the exact field meaning is **[?]**.

### 7.6 Validation against the dumped scripts (honest result)

We *did* run the validation the task asked for, and report it straight —
including a hypothesis we **rejected**:

- **Structure validates [C].** Every combat pilot script parses as
  `<root 0x0e/0x0d/…> <shared 32-B prologue> <body of 0x06/0x07/0x11/0x12/0x13
  child blocks> <0x19 0x19 0x19 terminator>`. `KIL_AF` (199 B) vs `GARVICK`
  (191 B) differ by exactly one inserted ~8-byte child record — i.e. the
  Kilrathi runs one *extra* maneuver, which fits "presses the attack harder".
- **Rejected hypothesis [—].** "`0x0f` count tracks aggression" looked good on
  one merchant (MER_SP had zero) but **fails across the set**: `f6=64`
  (Kilrathi) averages 1.0 `0x0f`, while `f6=128` (merchant) averages 0.5 —
  overlapping, not monotone. So `0x0f` is **not** a clean aggression dial.
  Logged so nobody re-walks this dead end.
- **CORRECTION (see §0 — source of truth):** `f6` IS a **monotone per-faction
  caution ladder** (Kilrathi 64 = fanatical → merchant 128 = timid), the morale
  axis of the FAQ §5.1 two-axis model. The intermediate "aggression /
  attack-commitment, do-not-flee" reading (from noisy debugger pokes against a
  field that has **no resident consumer** to poke) is superseded. Wire `f6` to a
  flee tendency **[I tuning]**. (The separate `0x0f`-count-as-aggression
  hypothesis above remains rejected — that was about MNVR bytecode, not `f6`.)

### 7.7 Next steps (where to resume — filed as beads)

1. Read the resident **DGROUP vtables** `0x20d`/`0x235` (from the *resident*
   Ghidra project, not the carve) → recover each maneuver class's `+0x0c`
   condition and `+0x14` update method, decompile them. That yields the real
   semantics (turn-to-target / throttle / evade / afterburner / fire).
2. Find the **deserializer** that reads `pilotdef+0x0f` byte-stream and `new`s
   the class tree (the streamable factory / class registry). That nails
   class-ID → C++ class and operand layout per class.
3. Decode `SDRONE` first (lead `0x09`, 120 B, fewest child blocks).

### 7.8 Mapping to `src/ship_ai.{h,cpp}`

Our current AI is a flat `AIState` machine
(`Idle/Patrol/Engage/BreakOff/Flee`) with global magic constants. Privateer's
is a **3-morale-tier set of maneuver-object trees**. The bridge:

| Privateer (decoded) | Our layer | Action |
|---------------------|-----------|--------|
| 3 morale-tier slots (low/normal/high) | single `ShipAIState` | **[next]** promote to a small per-tier behaviour set, morale-selected. **`f6` seeds the morale tier** (§0: low=fanatical, high=timid); HP/incoming-fire drive transitions between tiers |
| maneuver class `+0x0c` condition-test | our `next = AIState::…` transition block (`ship_ai.cpp` ~L134-177) | each class ≈ one guarded transition; keep our enum as the reduced form |
| maneuver class `+0x14` update/tick | our per-state `act` blocks (`Engage`/`BreakOff`/`Flee`, ~L257-430) | maps 1:1 onto our behaviour writes once class semantics land |
| `0x19` terminator/loop | our implicit "stay in state" | the scripts loop their tier — matches our sticky states |
| child-record scalar/flag operands (`0x01-0x08`, `0x10/0x20/0x40/0x80`) | `k_break_distance_m`, `k_break_duration_*`, `k_firing_max_*`, `k_engage_cos_threshold` | **[deferred]** these magic numbers are the operands we have not yet decoded; do **not** hardcode from guesses |
| CNST `f6` morale / caution | flee HP threshold + morale-tier seed (§0: low=fanatical, high=timid) | **[I tuning]** role resolved by FAQ §5.1 + faction values; no EXE consumer found (§7.13) so the HP formula is ours |
| CNST `f1` comms range / `f3` maneuver-jitter | **comm-bark distance** / steering RNG scale | **[L/do now]** `f1` = comms range, **not** perception (§11); detection = sensor 15000; `f3` = maneuver-jitter dial (§7.13) — both independent of MNVR decode |

**Bottom line for the reimplementation:** the *architecture* is mapped (3 tiers
→ behaviour sets; condition/update virtuals → our transition/act split). `f0`
(break-off / disengage, 600 + radii — NOT gun range) and `f3` (maneuver-jitter)
are ready to wire today, both live-confirmed. `f1` wires to **comms-bark range**
(1500), **not** perception; perception is the **sensor sphere 15000**. `f6` is
the **morale / caution** axis (§0 / FAQ §5.1: low=fanatical, high=timid) with no
located consumer — wire it to a flee HP threshold **[I tuning]**. The remaining
MNVR work is recovering the per-maneuver-class operand semantics (7.7), which
turns our hand-tuned `k_*` timings into Origin's real values.

---

## 7.9 CNST distance fields — scale CONFIRMED (`PRCD.EXE` disasm)

**Status: [C] scale confirmed; [L] PURPOSE of `f1` corrected.** This session
traced CNST `f0`/`f1` from storage to their per-tick distance comparisons in the
**resident** image. The *scale* facts below are solid **[C]**: a **`2^8` (×256)
fixed-point**, `f0`/`f1` are **raw world-unit radii**, distances are **linear**
(Euclidean), `×10` is refuted. **BUT the live test (§11) corrects what `f1`
is used for:** the `dist <= f1` compare at `0x1ce8c` is real, but it gates
**comms / taunt chatter**, **not** detection/awareness. Detection is the
**sensor sphere (15000)**, not `f1`. The labels below are updated accordingly;
the instruction-level facts are unchanged.

### Storage → consumption chain (all [C])

```
ship/actor + 0x02  -> pilotdef (AIDS record)          (gated by flag ship+0x0b bit0)
pilotdef   + 0x17  -> CNST 16-byte block              (loader 0xb37f1, §7.3)
CNST block + 0x00  -> f0  (engage/break-off radius, default 600 @ global DS:0x0e50)
CNST block + 0x02  -> f1  (comms/taunt range, default 1500 @ global DS:0x0e52)
```

The two global words `DS:0x0e50`/`0x0e52` are the **f0/f1 fallback** used when a
pilot has no `CNST` chunk — they equal the `MANEUVER.IFF` default `[600,1500,…]`
(§2.1/§4).

### f1 — COMMS / TAUNT-CHATTER range (func @ `0x1ce8c`)  **[L purpose / C compare]**

A 3-axis delta `(dx,dy,dz)` is fed to the magnitude helper and the **linear**
distance is divided by 256, then compared to **raw f1**. The compare is real
**[C]**; the live test (§11) shows it fires the **comm/taunt bark**, not
detection:

```
0x1cec5  sub  eax,[bx+8]                ; dz = target.z - self.z   (and dx,dy)
0x1ced7  lcall 0x4c5b:0x296            ; -> 32-bit Euclidean |delta|  (×256 fixed-pt)
0x1cedc  shl  eax,0x10 / shrd eax,edx  ; Borland long-return fixup
…
0x1cf05  sar  eax,8                     ; distance >> 8   (÷256 -> world units)
0x1cf09  mov  [0x65f0],eax              ; stash distance-in-world-units
0x1cf1a  mov  bx,[bx+0x17]              ; CNST block (else default below)
0x1cf1d  mov  ax,[bx+2]                 ; f1   (1500)
0x1cf22  mov  ax,[0x0e52]              ; …or default f1
0x1cf28  movsx eax,[bp-0xc]            ; eax = f1
0x1cf2d  cmp  eax,[0x65f0]             ; f1  ?  (distance>>8)
0x1cf32  jg   in_range                 ; comms-bark fires when  f1 > dist/256   (dist < f1·256)
```

→ **Comms / taunt-chatter range = f1 world units** (1500), compared in the `>>8`
world space. **[L]** (§11) Raising `f1` to 8000 made the NPC bark its taunt much
earlier while still closing; `f1`=500/12000 did not change *when* it detected or
engaged — so this is the comms gate, not awareness.

### f0 — BREAK-OFF / DISENGAGE radius (func @ `0x1d1f8`)  [CORRECTED, see §0 — NOT gun range]

Same distance helper; here **f0 is scaled UP** (`<<8`) and the two object radii
are added (also `<<8`), then compared to the distance:

```
0x1d267  lcall 0x4c5b:0x296            ; linear |delta| (×256 fixed-pt) -> [bp-6]
0x1d2a5  mov  ax,[bx]                   ; f0  (600)   (0x1d2a9: default [0x0e50])
0x1d2af  movsx eax,[bp-0x12]
0x1d2b4  shl  eax,8                     ; f0  ×256      <<<< the scale
0x1d2b8  mov  [bp-0x10],eax
0x1d2bc  …les bx,[bx+0x10]; mov ax,es:[bx+4]; movsx; shl eax,8; add [bp-0x10],eax  ; + self radius ·256
0x1d2d5  …(same for target via si+0x12)                                            ; + target radius ·256
0x1d2f3  cmp  eax,[bp-0x10]            ; distance  ?  (f0 + selfR + targetR)·256
0x1d2f7  jge  out_of_range             ; break-off when  dist < (f0+R1+R2)·256
```

→ **Break-off / disengage radius = (f0 + selfR + targetR) world units** (≈600 +
hull sizes). When the target gets this close the NPC turns away, afterburns out
and loops back for another pass (live-confirmed §11). This is **NOT** the gun
range — gun fire is gated separately by each gun's `range_m` + firing arc during
the attack-run window (§0, §9.4.2).

(Other consumers seen in the same pass: f1-setup `0x1c18d` pre-loads f1 into the
`0x65f0` scratch; an f0 user at `0x37b2f`. Same `+0x17 → +0/+2` access, same
gate flag, same `[0x0e50]/[0x0e52]` defaults — corroborating the layout.)

### The scale constant (the answer)

| Quantity | Hypothesis | **Reality (disasm)** |
|----------|-----------|----------------------|
| f0/f1 → distance | `×10` | **`×256` (`<<8` / `>>8`) fixed-point** |
| distance metric | (unstated) | **linear** Euclidean (not squared; no ×100) |
| f0 vs f1 ordering | comms > engage | **CONFIRMED** 1500 > 600 (same world units) |

**`×10` is REFUTED.** There is **no `imul …,10`, no shift-add ×10, and no
load-time scaling** anywhere in the AI loader or the runtime; the only scale on a
CNST distance field is the power-of-two `2^8` fixed-point conversion. `f0`/`f1`
are **raw world-unit radii** (600 / 1500); comms(f1) cleanly exceeds engage(f0)
at the real scale. **Detection/awareness is a *separate* range — the sensor
sphere 15000 (§7.10), not `f1`.**

### Reconciling the user's ~15000 cull observation  **(CORRECTED, §7.10/§11)**

The earlier "~10:1 unit ratio so 1500→15000" guess is **wrong**. The ~15000 is
not `f1×10` — it is a **distinct engine constant**, the sensor/contact range
`0x3a98` = 15000 (§7.10/§7.11), at the **same** `2^8` world scale as `f1`. So:
`f1`=1500 is the **comms/taunt** range (§11), and **15000 is the sensor sphere**
where detection/awareness actually happens. Two different constants, same scale,
different jobs — no ×10 anywhere. Engine fact stands: **raw 1500/600/15000,
`2^8` fixed-point, linear.**

### Wiring implication (concrete world-unit values)

- Treat `f1` as the **comms / taunt-chatter radius** (1500; drone 1800, Confed
  escort 2100) — **NOT** perception. `f0` is the **engage / break-off / gun-range
  radius** (600 + hull radii). Detection/awareness = the **sensor sphere 15000**.
  All three are in **Privateer world units** at the same `2^8` scale.
- **Do not apply a ×10** to map detection — detection is natively 15000 (§7.10),
  not `f1×10`. Keep `f0`/`f1`/sensor as the raw `600 / 1500 / 15000` stack in our
  units (the engine worked in 256-fixed-point throughout).
- Engage should include the two hull radii (`engage = f0 + selfR + targetR`),
  matching `0x1d1f8`.
- Range stack: **sensor 15000 (detection) > comms `f1` 1500 > break-off `f0` 600**
  (§7.10/§11).

> Evidence is a derived fact from the user's own legal `PRCD.EXE`; the carves,
> Ghidra projects and decompiler output stay gitignored under `re/` (verified
> `git check-ignore`). Only this derived summary is committed.

---

## 7.10 Targeting-VDU distance & the 1500↔15000 puzzle — RESOLVED (`PRCD.EXE` disasm)

**Status: [C] CONFIRMED — and this CORRECTS the earlier refutation in this file's
history.** Ground truth from the user playing *vanilla* Privateer: the cockpit
targeting readout shows **≈15000** at the moment an enemy is *first detected* —
and the live test (§11) confirms detection/awareness happens at this **sensor
sphere (15000)**, *not* at `f1`. The `magnitude>>8 == f1 == 1500` compare in
`§7.9` is the **comms/taunt-bark** gate (§11), not detection.
A prior pass concluded "no ×10" by sampling a **secondary, sub-5000 readout**
(`0x3329f`/`0x02f8b0`) — that was the wrong callsite and an incomplete story.

The corrected finding: **there is no ×10 anywhere — and there does not need to
be.** `15000` and `1500` are **two different ranges measured in the *same*
`magnitude>>8` world-unit scale**, not one value scaled by ten:

| Number the user/AI sees | What it actually is | Where (resident) | Scale |
|-------------------------|---------------------|------------------|-------|
| **15000** (`0x3a98`) | **sensor / radar contact range == DETECTION / awareness** — the NPC acquires a contact (and it drops once beyond 15000) **[L]** | cull loop `0x03d4xx`, compare `0x03d5b2` | `magnitude>>8` world units |
| **1500** (`f1`) | **comms / taunt-chatter range** — the NPC fires its comm bark **[L]** (§11); **NOT** detection | comms func `0x1ce8c`, compare `0x1cf2d` | `magnitude>>8` world units |
| **600** (`f0`) | **AI break-off / disengage** (+ hull radii) — NOT gun range **[L]** | func `0x1d1f8`, compare `0x1d2f3` | `magnitude>>8` world units |

So as a target approaches, the **same** displayed distance number ticks **down**:
it materialises on the VDU at ~15000 (crosses the sensor boundary) **and that is
where the AI detects/wakes** (live, §11: NPC closed from ~10,000u, far beyond
`f1`), it starts comms/taunt chatter at 1500 (`f1`), and it is in gun range /
breaks off near 600 (`f0`). The user reads "15000 at first detection" because
**first detection == entering sensor range == 15000 world units** — the genuine
acquisition boundary. The old "×10 of the AI's 1500" was wrong on two counts: it
is a separate constant, **and** 1500 was never the detection range to begin with
(it is comms).

### The real targeting-VDU distance readout (addresses + disasm)  **[C]**

The cockpit target-distance number is emitted by the recurring resident idiom
(four instances, all identical in scale): `0x020cd8`, `0x043737` (uncapped
target MFD), and `0x02f8b0`/`0x3329f` (a secondary, *only-drawn-when-<5000*
variant). Coordinate source and scale, from `0x020cd8`'s function:

```
0x020c6f  ...di+0x14 -> [bp-0x18]        ; target.position vec (obj+0x14)
0x020c77  lcall 0x348e:0x1d; +0x14       ; self/player.position vec (obj+0x14)
0x020c82  mov eax,[target+0]; sub eax,[self+0]   ; dx  -> [bp-0x38]
0x020c95  mov eax,[target+4]; sub eax,[self+4]   ; dy  -> [bp-0x34]
0x020ca7  mov eax,[target+8]; sub eax,[self+8]   ; dz  -> [bp-0x30]
0x020cd8  lcall 0x4c5b:0x296             ; |delta| (alpha-max-beta-gamma, §7.11)
0x020cdd  shl eax,0x10 / shrd eax,edx,0x10        ; Borland dx:ax -> eax
0x020cf2  (clamp >= 0; default DS:0x33e6)
0x020d0e  sar eax,8                       ; >>8  ->  WORLD UNITS  (NO x10)
0x020d1a  push dword [bp-0x16]            ; the value displayed == magnitude>>8
0x020d25  lcall 0x6793:0x89d             ; VDU text-field primitive (file 0x72fcd)
```

`0x6793:0x89d` (file `0x72fcd`) was verified this pass to be a **text-field
device primitive**: it reads only its byte-mode/column args (`[bp+0x10]`,
`[bp+0xa]`) and the buffer ptr; it **does not multiply** the numeric value. So
the number drawn is exactly `magnitude>>8`. (The earlier note that this proves
"display == 1500" was *locally* right but drew the wrong global conclusion: it
never found the 15000 *sensor* constant, so it mis-explained the user's reading
as a unit coincidence.)

### The 15000 sensor-range constant (the missing piece)  **[C — new]**

Resident per-frame contact scan, relative to the player ship (`DS:0x66b4`):

```
0x03d503  loop over contact array es:[bx+0x3b], count es:[bx+0x53], index si
0x03d525  contact.position = contact_obj+0x14 ; -> di
0x03d52c  ref.position      = [0x66b4]+0x14    ; player ship
0x03d535  eax = contact[0] - ref[0]            ; dx (and dy,dz)
0x03d57c  lcall 0x4c5b:0x296                   ; |delta| (same helper)
0x03d5aa  sar eax,8                            ; >>8 -> world units -> [bp-8]
0x03d5b2  cmp dword [bp-8], 0x3a98             ; vs 15000
0x03d5ba  jl  keep                             ; < 15000: contact stays
0x03d5c3  call 0x3d719                         ; >=15000: drop contact
```

`0x3d719 → 0x332f:0xad` (file `0x3e19d`) is a **contact-release**: it decrements
the active-contact count (`dec [si+6]`) and frees the contact object
(`0x1554:0x18`, then `0x1558:0x27b`). So **15000 = the maximum sensor/contact
range = the detection/awareness boundary** (live-confirmed, §11), in the very
same `magnitude>>8` world units the AI and the VDU use. This is the number the
user sees a fresh contact pop in at — and where the NPC AI wakes.

### The answer (corrected)

| Quantity | Earlier claim | **Corrected reality (disasm)** |
|----------|---------------|--------------------------------|
| display vs AI distance space | "identical `>>8`, so display==1500" | **identical `>>8` IS correct** — but the user's 15000 is the *detection/sensor* range, a *different* constant from `f1` |
| 1500 → 15000 factor | "×10 unit coincidence" | **NOT a factor at all** — `1500` (`f1`, **comms**) and `15000` (sensor cull `0x3a98`, **detection**) are *distinct constants* at the *same* scale |
| any ×10 on a distance | refuted | **still refuted** — no `imul …,10`/`lea ×5`/shift-add ×10 on any distance; sensor (15000), comms (1500) and engage (600) are all raw `>>8` world units |
| what `f1`=1500 gates | "AI detection/awareness" | **CORRECTED [L] (§11): comms / taunt-chatter range, NOT detection** |

**Net:** the previous §7.10 "refutation" reached the right micro-fact (no engine
×10) but mis-labelled the ranges. The *correct* stack (live-confirmed, §11) is:
**detection/awareness = sensor 15000 ⊃ comms `f1` 1500 ⊃ break-off `f0` 600**, with
the pursue→attack-run switch at 1000 (§9.4).

---

## 7.11 The complete distance pipeline — coords → magnitude → internal → display

End-to-end, every step first-hand disasm-verified this session. All **[C]**
unless noted.

### 1. Coordinate storage  **[C]**
Ship/world position lives at **`object + 0x14` = {x, y, z}, three 32-bit signed
words** (`+0x14`, `+0x18`, `+0x1c`). They are **8-fractional-bit fixed point**:
**1 world unit = 256 stored**. (Proven by every consumer dividing the magnitude
by 256 via `sar …,8` to get the comparison/display integer — `§7.9`, `§7.10`.)

### 2. Magnitude — `0x4c5b:0x296` (file `0x57646`)  **[C, decompiled this pass]**
Borland `long`-returning helper, args = the three 32-bit deltas. It is an
**alpha-max-beta-gamma 3-D magnitude approximation**, not a true sqrt:

```
abs(dx),abs(dy),abs(dz); sort so A=max, B=mid, C=min
result = A + C>>2 + B>>2 + B>>3 - B>>5
       = max + (11/32)·mid + (1/4)·min        ; 11/32 = 0.34375
return packed as dx:ax  (shld edx,eax,0x10)
```

Linear in the inputs, so the output is in the **same 256-fixed-point** as the
positions. Approximation error vs true Euclidean is bounded ≈ **±8 %** with an
angular skew (worst near the body diagonal) — relevant if we want bug-for-bug
parity in our own range checks; a real `sqrt` will read ~8 % shorter on
diagonals than Privateer did.

### 3. To internal world units  **[C]**
Callers do `sar eax,8` (`÷256`) → an **integer world-unit distance**. This is
the canonical "distance to target" the AI stashes at `DS:0x65f0` (`0x1cf09`).

### 4. AI compare scale  **[C compare / L purpose]** (restated from §7.9, §11)
- **Detection / awareness:** `magnitude>>8 < 15000` (sensor sphere `0x3a98`) —
  `0x03d5b2`. **This is detection, not `f1`** (§11).
- **Comms / taunt:** `magnitude>>8 < f1` (default **1500**) — `0x1cf2d`. The
  compare is `[C]`; its purpose is the comm bark **[L]** (§11), not awareness.
- **Break-off / disengage (NOT gun range):** `magnitude>>8 < (f0 + selfR + targetR)`
  with `f0` default **600**, all `<<8` for the compare — `0x1d2f3`.
- `f0`/`f1` are stored **raw/unscaled** at load (`§7.3`), no ×N.

### 5. Sensor / radar contact range  **[C — new]**
`magnitude>>8 < 15000` (`0x3a98`, `0x03d5b2`) keeps a contact; beyond it the
contact is released. This is the outermost tier.

### 6. Display scale  **[C]**
Targeting VDU draws **`magnitude>>8` verbatim** (`0x020cd8`/`0x043737`) — `×1`,
no scaling. So the on-screen number IS the internal world-unit distance.

**One coherent chain:**
```
stored coords (256-fp, obj+0x14)
   └─ alpha-max-beta-gamma magnitude  (256-fp, ~±8% vs Euclid)
        └─ >>8  ──►  integer world-unit distance  D
                      ├─ D < 15000  → on sensors / NPC AI DETECTS you  (sensor sphere 0x3a98) [L]
                      ├─ D < 1500   → NPC fires comms / taunt bark     (f1) [L]  (NOT detection)
                      └─ D < 600+R  → NPC AI engages / break-off       (f0) [L]
```
No multiply anywhere; just one `÷256` and three thresholds in one unit. (The VDU
draws `D` ×1 regardless; first contact appears at the 15000 sensor edge.)

---

## 7.12 Reconciling with `new_privateer` units — "there's more going on"

The user is right that **our world is not a flat ×10 of Privateer's**. Two facts
make the mapping non-trivial:

1. **The ×10 never existed.** It was an illusion from reading Privateer's
   *sensor/detection* range (15000) as if it were ×10 of `f1` (1500). `f1` is
   the **comms range**, not detection (§11); both 15000 and 1500 are already in
   the same Privateer world unit (`§7.10`/`§7.11`).
2. **Our engine's ranges were hand-set, not derived from one conversion
   factor.** Current `new_privateer` values (meters):

   | Our knob | Value (m) | Privateer analogue | Privateer value (world units) | implied ratio |
   |----------|-----------|--------------------|-------------------------------|---------------|
   | `perception::k_player_radar_m` | 35000 | — (player HUD, no direct analogue) | (sensor/detection 15000) | ~2.3× |
   | `ShipClass::radar_range` | 25000 | **sensor/contact = detection range** | **15000** (`0x3a98`) | ~1.7× |
   | `ShipClass::weapons_range` | 3000 | gun range (stays gun range) | per-gun `range_m` | — |
   | `ship_ai k_break_distance_m` | 150 | break-off `f0` 600 (+radii) | 600 | ~0.25× |
   | `docking k_dock_range_m` | 2000 | — | — | — |
   | system nav coords (`troy.json`) | 50k–200k | inter-nav distances | (coarser nav space) | — |

   These ratios are **all over the place** (0.25× … 5×), which is the concrete
   meaning of "there's more going on": **no single Privateer→ours factor fits.**

### What the mapping SHOULD be (recommendation)  **[I, evidence-backed]**

The clean, faithful conversion is **≈1:1** between *Privateer's `magnitude>>8`
world unit* and *our meter* — i.e. adopt Privateer's three-tier range stack
directly rather than inventing per-knob numbers:

| Tier | Privateer (world units) | Suggested `new_privateer` (m) | Note |
|------|-------------------------|-------------------------------|------|
| sensor / radar contact = **detection / awareness** | **15000** | **15000** (was 25000–35000) | matches `0x3a98`; **this is the detection range** (§11), our radar is currently ~1.7–2.3× too generous |
| comms / taunt range (`f1`) | **1500** (drone 1800, Confed escort 2100) | **1500** (per-pilot from CNST `f1`) | **comms-bark distance, NOT perception** (§11) |
| AI engage / break-off / in-gun (`f0`+radii) | **600** + hull radii | **600** + hull radii | currently split awkwardly across `weapons_range`=3000 / `k_break_distance_m`=150 |

If we deliberately keep a "player HUD is enhanced" bump (our `35000` vs sensor
`15000`), document it as **our** choice (≈2.3×), **not** a Privateer behaviour —
Privateer used one 15000 sensor sphere for everyone. The 8 %-short Euclidean vs
Privateer's alpha-max-beta-gamma (`§7.11`) is well within these tolerances and
can be ignored for gameplay (note it only if exact parity is ever wanted).

> **Honest scope.** The base-independent facts here (positions are 256-fp at
> `obj+0x14`; one magnitude helper; `>>8` to world units; sensor/detect 15000 /
> comms `f1` 1500 / break-off `f0` 600 all in that one unit; VDU draws distance ×1) are
> first-hand certain from disasm; the *purpose* labels (15000 = detection,
> `f1`=1500 = comms, `f0`=600 = engage/break-off) are **[L]** live-confirmed
> (§11). The exact *physical* meaning of one Privateer
> world unit (km? "klick"?) is **[I]** — we only have the ratios, which is all
> the reimplementation needs. If a byte-perfect on-screen glyph trace is ever
> wanted, the definitive route is **dynamic**: run vanilla in the dosbox-x
> debugger, breakpoint the magnitude helper `0x4c5b:0x296`, and watch the
> returned value alongside the cockpit number through a real intercept — that
> pins the absolute unit (the 15000/1500/600 tiers and their roles are already
> live-confirmed, §11).

> Evidence is a derived fact from the user's own legal `PRCD.EXE`; the carves,
> Ghidra projects, capstone scans and decompiler output stay gitignored under
> `re/` (verified `git check-ignore`). Only this derived summary is committed.

---

## 7.13 CNST field consumer trace — `f3` resolved; `f6` consumer still unlocated (behaviour live-corrected, §11) (`PRCD.EXE` static + runtime-prep)

**Status:** `[C]` for `f0`/`f1` (already §7.9); `[C]` for the single `f3`
consumer; `[PENDING]` for `f6`; `[I]` for `f2`/`f4`/`f5`/`f7`.

This trace searched the loaded resident image of `PRCD.EXE` for every
instruction of the form `MOV reg, [base + 0x17]` (`base` in `BX/BP/SI/DI`).
That three-byte encoding is the compiler's canonical form for loading the
`CNST` block pointer from `pilotdef + 0x17`.  All 30 occurrences were found and
checked for subsequent reads of `CNST` offsets `+0x00/+0x02/+0x04/+0x06/+0x08
/+0x0A/+0x0C/+0x0E` within the same function.

### Summary table: functions that load the CNST pointer

| Function file offset | Program seg:off | Base | Offsets read | Usage summary |
|----------------------|-----------------|------|--------------|---------------|
| `0x1ce8c` | `0x1208:000c` | BX | `+0x02` | **comms / taunt-chatter range (`f1`)** — compare confirmed §7.9; purpose live-corrected §11 (NOT detection) |
| `0x1d1f8` | `0x123f:0008` | BX | `+0x00` | engage / in-range (`f0`) — confirmed §7.9 |
| `0x39b01` | `0x2ed0:0001` | BX (via `LES BX,[SI+2]` -> `ES:[BX]`) | `+0x06` | **maneuver-jitter / aggression scaling (`f3`)** — confirmed §7.13.2 |
| *(others)* | — | BP/SI/DI/BX | `+0x02` only (or none) | stack locals / unrelated structs; no CNST field evidence |

**Key finding:** `f3` is **not** read in the detect/engage functions.  It is
read in an entirely separate AI subsystem — the per-tick steering/maneuver
update — while `f6` has **no direct consumer** in the resident image traced
here.

### 7.13.1 VROOMM overlay manager found (`PRCD.EXE` overlay pool disasm)

The Borland VROOMM overlay manager lives at the **start of the overlay pool**
(file `0x7ff60..0xe4f20`).  The pool begins with the `FBOV` header (4 bytes:
`46 42 4f 56`) followed by manager metadata and then a small relocation/fixup
table at `0x7f74f` (six-byte records ending with the partial record `08 b8 66`).

The resident-image boundary is `0x7ff60`; the zero padding `0x7f7d0..0x7ff60`
is unused by the engine and provides a convenient code-cave region for the
runtime logger described in §7.13.3.

| Symbol (inferred) | File offset | Program seg:off | Role |
|-------------------|-------------|-----------------|------|
| `OvrInit` | `0x7ff70` | `0x7517:0000` | Allocate overlay-manager context (calls malloc @ `0x0000:0x861`) |
| `OvrClose` | `0x7ff9b` | `0x7519:000b` | Free context + optional file close |
| `OvrFind` | `0x7ffc6` | `0x751c:0006` | Search overlay directory; returns table index or `0xFFFF` |
| `OvrLoad` | `0x8011f` | `0x7531:000f` | **Main overlay load entry** — calls `OvrFind`, then opens `FILE`/`STAT` resources and reads the overlay directory entry |

**Overlay directory layout** (from `OvrFind` / `OvrLoad` disasm):

* Entries are **10 bytes**.
* Offset `+0x00` = overlay number (u16).
* Offsets `+0x02` and `+0x06` = two dwords read by `OvrLoad` (likely file offset
  / size pair for the overlay data).
* The directory ends with a `0xFFFF` sentinel.
* The directory is **heap-allocated** at runtime (`[si+0x50]` in the manager
  context); no static table address was found in the file.

> **Honest note on runtime segments.** The program seg:off values above are
> file-relative (`file_offset - 0xae00` in paragraph form).  VROOMM overlays are
> loaded into emulated memory at a segment chosen by the manager (UMB/EMS/conventional);
> the actual runtime CS/DS for overlay-pool code is therefore not fixed.  Break
> and patch recipes use file offsets so they can be applied to a copy before
> launch.

### 7.13.2 `f3` (+0x06) — maneuver-jitter / evade-gain (experience axis, FAQ §5.1)  **[C]**

**Consumer:** function @ file `0x399a3` (program `0x2e9a:0003`), read at
`0x39b01` (`0x2ed0:0001`).

**Access path (disasm):**

```
039aef: 66ff7402                 push dword ptr [si + 2]
039af3: c45c02                   les bx, ptr [si + 2]
039af6: 268b1f                   mov bx, word ptr es:[bx]
039af9: ff5f08                   lcall [bx + 8]
039afc: 83c404                   add sp, 4
039aff: 8bd8                     mov bx, ax
039b01: 8b5f17                   mov bx, word ptr [bx + 0x17]      ; pilotdef -> CNST
039b04: 8b4706                   mov ax, word ptr [bx + 6]         ; f3
```

**Usage context:** the value is stored to `[bp - 0x3e]`.  A few instructions
later the function calls the RNG helper `0x0000:0x96f`, scales the result by
`0x1e` (30), divides by `0x8000` (signed), and **adds** it to the `f3` value:

```
039b3b: 660fbfc0                 movsx eax, ax
039b3f: 666bc01e                 imul eax, eax, 0x1e
039b43: 66bb00800000             mov ebx, 0x8000
039b49: 6699                     cdq
039b4b: 66f7fb                   idiv ebx
039b4e: 8b56c2                   mov dx, word ptr [bp - 0x3e]      ; dx = f3
039b51: 03d0                     add dx, ax                          ; f3 + rng·30/32768
```

The sum is then used in a fixed-point divide against a case-derived magnitude
threshold (`0x3200`) and the final quotient is written to the ship/AI state
field at `[si + 0x18]` (and mirrored to `[si + 0x14]`).

**Semantics:** `f3` is a per-pilot **aggression scaling factor for maneuver
randomness**.  A high `f3` (pirates/aces = 75) makes the NPC's steering update
noisier / more evasive; a low `f3` (merchants = 30) makes it tame.  The default
fallback (taken when `[si + 0x2b] & 1` is clear) is the global word at
`DS:0x2492`, which corresponds to the `MANEUVER.IFF` default `f3` = 60.

This refutes the earlier "pursuit tenacity" hypothesis: the field is not
compared against a distance and does not gate break-off.  It controls **how
much RNG jitter is mixed into the maneuver amplitude**.

### 7.13.3 Runtime overlay-load logger patch  **[PENDING]**

Because the macOS DOSBox-X heavy debugger crashes during interactive flight, a
small **file-patch** logger was built.  It patches a *copy* of `PRCD.EXE`, logs
in the emulated DGROUP scratch area, and can be restored with `--reset`.

**Patch targets:**

| Site | File offset | Original 5 bytes | Patched 5 bytes | Cave |
|------|-------------|------------------|-----------------|------|
| `OvrLoad` prologue | `0x80122` | `81 ec b0 00 56` | `e9 d9 fc 90 90` | `0x7f800` |
| `OvrClose` prologue | `0x7ff9b` | `55 8b ec 56 57` | `e9 80 fe 90 90` | `0x7f820` |

**Log scratch area:**

| Address | Meaning |
|---------|---------|
| `DS:0x66c0` | Last overlay **number** loaded (u16) |
| `DS:0x66c2` | **Flag**: `0` = load, `1` = unload |

**Read recipe in DOSBox-X debugger:**

```
D DS:66C0
D DS:66C2
```

**Apply / reset:**

```sh
python3 re/overlay_trace.py --apply
python3 re/overlay_trace.py --reset
```

The patched game tree is written to `re/dosbox/game_patched/` and launched with
`re/dosbox/overlay-trace.conf` (mounts the patched C: tree).  The patch is
RAM-equivalent: it never modifies the original `PRCD.EXE`; `--reset` deletes the
patched tree.

**What the cave does (load path):**

1. Execute the original `sub sp, 0xb0` / `push si` prologue bytes.
2. Save `BX` and `DS`, set `DS = SS` (DGROUP).
3. `mov bx, [bp+8]` — the overlay number argument to `OvrLoad`.
4. `mov [0x66c0], bx` — write it to the scratch word.
5. Restore `DS`, `BX`, and jump back into `OvrLoad` at `0x80127`.

The unload cave is analogous but writes `1` to `DS:0x66c2`; the unload entry
receives a context handle and a flag rather than an overlay number, so only the
load/unload flag is recorded for unloads.

> **Safety note.** All modified registers are saved/restored; the prologue is
> re-executed verbatim so the function's stack frame is identical to the vanilla
> build.  The cave lives in the zero padding before the FBOV header and does not
> overlap any original code or data.

### 7.13.4 Test scenario and candidate overlays  **[PENDING]**

The logger patch was applied and the patched binary prepared
(`re/dosbox/game_patched/PRCD.EXE`), but the actual in-cockpit scenarios
(pirate encounter vs. merchant flee/comms/docking) require interactive flight
and manual debugger/memory inspection.  No automated capture was run in this
pass.

**Planned scenario:**

1. `python3 re/overlay_trace.py --apply`
2. `bash re/dosbox/overlay-trace_launch.sh`
3. At the `C:\>` prompt: `PRIV.EXE`
4. Fly to a nav point, get a normal pirate encounter, then `D DS:66C0` in the
   debugger (if stable) to dump the load log.
5. Reset, re-apply, find a merchant encounter, trigger flee/comms/docking, and
   dump the log again.
6. Compare the two logs; any overlay numbers seen only in the merchant/f6-relevant
   scenario are candidate overlays for f6 consumers.

**Candidate overlay list:** none identified yet — pending dynamic run.

### 7.13.5 `f6` (+0x0C) — overlay scan + indirect trace complete, still no consumer
**[PENDING]**

The resident-image trace found **zero** instructions that read `CNST + 0x0C`
via the `pilotdef + 0x17` pointer (§7.13 header table).  A follow-up scan of the
raw VROOMM overlay pool (file `0x7ff60..0xe4f20`, 413632 bytes) produced:

| Scan | Result |
|------|--------|
| Raw u16 occurrences of the four known `f6` values (`0x4c`, `0x80`, `0x40`, `0x66`) | **415** total: 0x4c=126, 0x80=69, 0x40=188, 0x66=32 |
| Occurrences in a plausible 16-byte CNST-block pattern | **0** |
| Full 16-byte faction CNST signatures (pirate/merchant/kilrathi/bounty/militia/default) | **0** hits |
| Instructions that load/compare an immediate equal to an `f6` value | **0** (after filtering far-call seg:off addresses) |
| Indirect `LES BX,[SI+2]` -> `[BX+0x0C]` reads | **0** in overlays; resident trace also found none tied to `pilotdef+0x17` |
| Resident f3/f1/f0 consumer byte-signatures duplicated in overlays | **0** hits |

The 415 raw byte-pair hits are almost all small immediates/displacements inside
unrelated x86 code (e.g. `0x40` is a common displacement/sign-extend byte); none
survived the 16-byte CNST-block structural heuristic (f0=600, f1=1500, f7=0).

**Conclusion (CORRECTED, see §0):** `f6` has no observable consumer in either the
resident image or the carved VROOMM overlay pool via static pattern matching —
that static result still stands. Because there is **no consumer to poke**, the
earlier debugger-poke reading ("aggression / attack-commitment") was **noise** and
is withdrawn. The resolved role (GameFAQs FAQ §5.1 + canonical faction values):
`f6` = **morale / caution** — LOW=fanatical (Kilrathi 64), HIGH=timid (merchant
128). The *role* is settled; the *code path* is still unlocated. Three
possibilities remain for the missing consumer:

1. `f6` is consumed from a **cached copy** in the ship/AI state object reached
   by `LES BX,[SI+2]`, but not at offsets `+0x0A`/`+0x0C` (no such read was
   found in the indirect trace).
2. `f6` is dead / unused in this build.
3. `f6` is consumed via a value transformation not caught by these signatures
   (e.g. loaded as a byte, doubled, table-indexed), or from an overlay loaded
   during a merchant/flee scenario that has not yet been captured dynamically.

**Next step:** run the overlay-load logger scenario (§7.13.4) to capture the
actual overlay numbers loaded during combat (now targeting **attack-commitment**
behaviour, not "flee" — e.g. compare `f6=0` vs `f6=255` patched runs), then carve
and scan only those candidate overlays for the `f6` reader.  Until that dynamic
run pins the code path, `f6`'s **consumer** remains **[PENDING]** (its behaviour
is live-confirmed, §11).

**Tooling note:** the overlay carve, scan artifacts and the patched binary live
under `re/` (gitignored).  Only the derived facts in this paragraph are
committed.  Verify with `git check-ignore re/dosbox/game_patched/PRCD.EXE`.

### 7.13.6 `f2`/`f4`/`f5`/`f7` — no standalone consumers found  **[I]**

- `f2` and `f4` do **not** have independent readers in the detect/engage
  functions; earlier reports of `+0x04`/`+0x08` reads there were grouping
  artefacts from unrelated `[bp+…]` stack accesses within the same function.
- `f5` remains paired with `f4` structurally (always equal in the data), with
  no independent code consumer.
- `f7` is constant `0` and treated as padding.

`f2`/`f4` may be used by the same maneuver subsystem that consumes `f3` (they
vary with the same elite-vs-rank split), but the static trace did not isolate a
specific instruction for them.

---

## 8. English / mnemonic hints — string-mining pass  **[C]**

A dedicated hunt for any human-readable label of the CNST fields (`f0..f7`) or
the MNVR maneuver-class IDs (`0x06/07/09/0b/0d/0e/0f/0a/11/12/13`), across
`PRCD.EXE` (resident + overlay pool) and the `DATA/AIDS/*.IFF` data.

### 8.1 Headline result — there are **no** field/maneuver labels

**Confirmed negative.** Neither the binary nor the data carries an English or
mnemonic token for `f0..f7` or for any maneuver-class ID. Searched
`PRCD.EXE` for `ATTACK/EVADE/FLEE/BREAK/PURSUE/DISENGAGE/RANGE/DETECT/AGGRESS/
MORALE/MANEUVER/TAUNT/ENGAGE/RETREAT/APPROACH/PATROL/ESCORT/COWARD/…` (whole-
word, case-insensitive, full file). The only matches are **UI/state labels**,
not AI internals:

- `maneuver` (file `0x7ba43`) and `combat` (`0x7ca0d`) — sit in the menu/HUD
  string pool next to `trader`, `Game Paused`, `TEAMS`; they are screen text,
  not field names.
- `Range:` / `%X%Y%JRange:` (`0x7b84e`/`0x7b841`) — the cockpit target-distance
  HUD label (the §7.10 VDU readout).
- `MISSILE/TORPEDO/TRACTOR/GUNFIRE/No Missiles/NO TARGET` — weapon HUD strings.

The AI subsystem references the IFF tags only as `push dword '<tag>'` code
immediates (e.g. `66 68 'INFO'` reads as `fhINFO` in a naive string scan) — i.e.
they are **code, not labels**. This is fully consistent with §7.4: the combat
brain is a **serialized C++ object tree**, addressed by numeric class-ID, with no
string keys to mine. So `f0/f1/f3/f6` semantics must be pinned **empirically**
(see `docs/ai_debug_playground.md`) — there is no shortcut hiding in the strings.

### 8.2 The faction / pilot-type name table  **[C]**

`PRCD.EXE` `0x79d34` holds the pilot-type prefix table, contiguous:
`PIR_`, `KIL_`, `BOU_`, `MER_`, `COM_`, `MIL_`, `CON_`, then `PLAYER`
(`0x79d61`), then `typenams`/`rand_npc`/`randcu_`. These are the `DATA/AIDS/`
file prefixes (§1) used to pick a pilot template for a spawned NPC — useful for
the playground (which faction signature to search for), not for field labels.

### 8.3 Weak corroborations from AIDS comm chatter  **[C bytes / I reading]**

The `AIDS/*.IFF` pilots carry, besides the AI core (`AIDS/INFO/CNST/MNVR`), a
**comm-chatter block** keyed by *attitude*. Full 8-char tag census across the 47
pilots: `FRNDFORM`, `NEUTFORM`, `HOSTFORM`, `PLOTFORM`, `SENDFORM`, `CMBTMNUM`,
`RECVMNUM`, `SENDMNUM`, `LISTMNUM`, `MSGS`, `TUSRFNAM`/`PUSRFNAM`/`BUSRFNAM`,
`XCHG`, `TIME`. None label `f0..f7`, but two facts corroborate earlier `[I]`
readings:

- **`FRNDFORM`/`NEUTFORM`/`HOSTFORM`** (friendly/neutral/hostile message forms)
  match the `ATTD` `AROW` code reading (§3.1): `8`=ally, `7`=neutral, `6`=hostile
  are the three reaction tiers. Independent confirmation of the matrix semantics.
- **`SDRONE`** carries *"This attitude not possible"* for both `FRNDFORM` and
  `NEUTFORM` (only `HOSTFORM` has real lines) — i.e. the scout drone is
  hostile-only, matching its `f6=64` "fight-to-the-death" tier and the
  loner-aggressor `AROW` row.
- Combat barks (`CMBTMNUM`) flavour the modes: a neutral pilot's
  *"We are tracking you, keep your distance"* (awareness), merchant
  *"I'm but a lowly coward, spare me!"* / *"Please don't trash me, man!"*
  (early-flee, `f6=128`). Flavour only — **not** field evidence.

### 8.4 Cross-ref: dpjudas `/tmp/WCPrivateer`  **[C]**

The dpjudas reimplementation is an **asset/renderer** project: it lists the
`DATA.AIDS.*.IFF` paths as a file manifest (`Sources/FileFormat/WCGameData.cpp`)
but contains **no combat-AI code and no CNST/MNVR field naming** (its only
"maneuver" token is the cockpit damage label "Maneuvering Jets"). No hints there.

### 8.5 Other readable tokens noted (for the debugger, not the AI)

`PRCD.EXE` ships Borland memory-manager debug printfs — `handler %d attached
with %ld bytes available`, far/near/EMS heap dumps (`%s block at DS:%04X, size =
%u`), `Far Heap Allocation error #%04X` — confirming the **near-heap in DGROUP**
(where the malloc'd CNST block lives, §7.3) and giving optional landmarks for the
live debugger. No bearing on field meaning.

> Same provenance/legality as the rest of this file: facts derived from the
> user's own legal `PRCD.EXE` + `DATA`. Raw bytes/strings dumps stay gitignored
> under `re/`; only this derived summary is committed.

---

## 9. MNVR maneuver system — full decode (§7.7 follow-up, `PRCD.EXE` disasm)

**Status:** the §7.7 deep-decode. This section recovers the **runtime object
model**, the **collection (container) class**, **three fully-decoded leaf
maneuvers with real numeric parameters**, the **per-tick dispatch**, the
**3-morale-tier array**, and a structural decode of **SDRONE / GARVICK / KIL_AF**.
The **byte→class deserializer registry** is *located in model* but its exact
class-ID→vtable map and per-class operand widths remain **[?] open** (resume
notes in §9.9). Everything is tagged **[C]** byte/disasm fact, **[I]** inference,
**[?]** open.

> **Provenance / legality.** Derived facts from the user's own legal GOG
> `PRCD.EXE`. Ghidra projects, the EXE copy, overlay carves, capstone scans and
> all decompiler output stay **gitignored** under `re/` (verified
> `git check-ignore re/ PRCD.EXE`; `GAME.GOG` lives under `re/dosbox/` and
> `gog_extracted/`, both ignored). Only this derived doc + the code-only
> `tools/ghidra/` helpers are tracked. OpenMW-style clean-room policy.

### 9.0 Tooling note (reproducible)

Resident AI code is in Ghidra project `re/ghidra_proj/PRCD` (gitignored). Headless
needs `JAVA_HOME=/opt/homebrew/opt/openjdk@21/...`. Helper scripts live in
`re/gs/` (local). Capstone (`re/.venv`) reads the raw file directly and is the
**ground truth** — Ghidra's auto-analysis is mis-aligned in several resident AI
regions, so all addresses below were verified with capstone on `PRCD.EXE`.
Address map (all **[C]**):

- resident: `runtime_linear = file_off − 0xAE00`; Ghidra DGROUP segment = `0x7e4d`
  (i.e. `DS:off` → Ghidra `7e4d:off`).
- AI overlay window `ovr_big.bin` = file `0x80000…0x98000`; its full Ghidra
  decompile is `re/decomp_big.txt` (function offsets are *carve-relative*, so
  `func @ 0xNNNN` = file `0x80000+0xNNNN`).

### 9.1 The runtime object model — it is a Collection of maneuvers  **[C]**

The combat brain is **object-oriented Borland C++**, confirmed by decompiling the
vtable methods. There are **two kinds of objects**:

**(a) The maneuver-list / "Collection" class** — DGROUP vtables **base `0x20d`**,
**derived `0x235`** (adjacent in DGROUP; `0x235 = 0x20d + 0x28`, i.e. 10 far-ptr
slots each). Both vtables' method bodies live in the **AI overlay** (base→overlay
seg `0x7c73`, derived→overlay seg `0x7a04`). Decompiled from `re/decomp_big.txt`
the derived methods are a **singly-linked list collection** (Borland
`TNSCollection`-style):

| Overlay fn (carve) | Role | Body (decompiled) |
|--------------------|------|-------------------|
| `FUN_4e63` | **add-at-head / insert** | `*node = list[+2]; list[+2]=node; if list[+4]==0 list[+4]=node` |
| `FUN_4e04` | append-at-tail | links `node` after `list[+4]` (tail) |
| `FUN_4e2b` | addElement(head) | `cell=allocCell(item); insert(cell)` |
| `FUN_4dcc` | addElement(tail) | `cell=allocCell(item); append(cell)` |
| `FUN_4e86`/`FUN_4eb5` | **at(index)** | walk `index` links via iterator `FUN_4b04`, return `cell[+2]` (payload) |
| `FUN_4f18` / `FUN_4f08` | head `[+2]` / tail `[+4]` | accessors |
| `FUN_4f57` | alloc list-cell | `*cell=0(vtable); cell[+2]=item` (cell: `+0` next, `+2` data) |
| `FUN_4f28` | alloc node, vtable `0x2d2` | a second small class (list element header) |

So **a Collection object** has: `+0x00` vtable, `+0x02` head ptr, `+0x04` tail
ptr (and is itself ≥6 bytes; the 15-byte `operator new(0xf)` figure from §7.4 is
the *leaf-maneuver* node, not the collection — corrects §7.4). The **maneuver
tree = nested Collections** whose payloads are leaf-maneuver objects.

**(b) The leaf-maneuver objects** — the actual behaviours. Their **update/tick
methods are RESIDENT** (file `0x396xx…0x39axx`), reached through a different,
leaf-class vtable. Each leaf object instance carries its own working state
(timer, phase flags, heading deltas). Decoded layout of a leaf-maneuver instance
(`si` in the disasm; **[C]** offsets, **[I]** names):

```
leaf + 0x00 : far-ptr-ish to the controlling SHIP object's accessor vtable
leaf + 0x02 : far ptr (dword) to the SHIP object   (es:[bx] used for heading/orient)
leaf + 0x06 : word  (computed heading word, written to ship es:[bx+3])
leaf + 0x08 : dword desired X-axis rotation delta   (256-fixed-pt; ±0x100 = ±1.0)
leaf + 0x0c : dword desired Y-axis rotation delta   (256-fixed-pt)
leaf + 0x10 : dword desired Z/roll rotation delta   (256-fixed-pt)
leaf + 0x14 : dword TIMER / accumulator
leaf + 0x18 : dword computed turn-rate target
leaf + 0x1c : byte  phase flags  (bit0 = "wind-up done / executing", bit1 = "finished")
leaf + 0x29 : word  computed range-derived parameter (see ORIENT maneuver)
leaf + 0x2b : byte  flag bit0 = "this pilot has a CNST block" (gate, §7.13)
```

### 9.2 The factory + constructor chain  **[C]**

`re/decomp_big.txt FUN_363d` (overlay) is the maneuver-object constructor. It is
the classic Borland two-stage vtable ctor:

```
node = operator_new(...)            ; FUN_0861 (near-heap malloc, DGROUP)
node->vtable = 0x20d                ; base
(*node->vtable[+0x08])(node)        ; base ctor
node->vtable = 0x235                ; upgrade to derived
(*node->vtable[+0x08])(node)        ; derived ctor
slot[idx] = node
(*node->vtable[+0x04])(node, arg)   ; +0x04 init  (arg = builder context)
(*node->vtable[+0x1c])(node)        ; +0x1c suspend/leave (start inactive)
```

The **3-slot tier array** is allocated by `FUN_336b(brain, 3)` (zeros 3 words);
`FUN_3613` / `FUN_3700` allocate it lazily on first use and store the far ptr at
**brain+0x0f** (matching §7.3's "+0x13/+0x15 built-object ptrs", here seen as the
runtime brain's `+0x0f` child array).

### 9.3 The per-tick dispatch loop  **[C]**

`re/decomp_out.txt FUN_3493` (overlay) is the per-tick condition dispatch:

```
bool tick(brain, target_key):
    if (brain[+0x02]==target_key[0] && brain[+0x06]==target_key[1])
        return (*brain->vtable[+0x0c])()        ; brain's own condition
    if (brain[+0x0f] == 0) return 0             ; no tier array -> nothing
    for idx in 0..2:                            ; <= 3 morale-tier slots  [C]
        slot = brain[+0x0f][idx]
        if slot != 0 && slot[+0x02]==target_key[0] && slot[+0x06]==target_key[1]:
            return (*slot->vtable[+0x0c])()      ; run that tier's condition
    return 0
```

So the engine: matches a maneuver to the active target by an **8-byte key**
(`+0x02`,`+0x06` = the ship/target object handle, set by `FUN_2a2c` from the
ship's `+2/+6`), then runs vtable **`+0x0c` condition**; the activation path
(`FUN_363d`/`FUN_3700`) runs **`+0x04` init** and **`+0x14` update**. **Exactly
3 slots** is hard-confirmed (`if (2 < idx) return`). **[C]**

**Tick rate / `dt`.** Every leaf maneuver advances its timer by the global
`DS:0x2768` (`add [leaf+0x14], [0x2768]` to count up, `sub` to count down). This
is the per-frame **AI time-delta**. Its absolute value is **[?]** (BSS, set at
runtime) — the "~10 Hz" figure from lore is **not** confirmed by static disasm.
The duration constants below (76, 153, 512) are in **these `dt` units**.

### 9.4 Three fully-decoded leaf maneuvers (the real numbers)  **[C]**

All three are resident, share the leaf layout (§9.1b), take `(self=si, target)`,
return 1=finished / 0=continue, and drive the ship via two primitives:
`lcall 0x4c5b:0x296` = the **alpha-max-beta-gamma magnitude** helper (§7.11) and
`lcall 0x1dfe:0x1cf` = a **"rotate ship toward stored heading"** primitive
(writes ship `es:[bx+3]` orientation from `es:[bx+0x1f]`).

#### 9.4.1 ORIENT / turn-to-target — fn @ file `0x3966f`  **[C]**

```
delta = target.pos(+0x14) - self.pos(+0x14)
if !(leaf[0x1c] & 1):                       ; PHASE 1 (wind-up)
    leaf[0x14] = 0x200 (=512)               ; timer init  [C]
    rotate_toward_target()                  ; lcall 0x1dfe:0x1cf
    dist = magnitude(delta)                 ; lcall 0x4c5b:0x296 ; clamp>=0 (def DS:0x33e6)
    leaf[0x29] = (dist>>16)*4 ; clamp >= 0x9c4 (=2500)   ; range-scaled param, min 2500 [C]
    leaf[0x1c] |= 1
else:                                        ; PHASE 2 (hold/execute)
    rotate_toward_target()
    leaf[0x14] -= dt(DS:0x2768)             ; countdown
    if leaf[0x14] < 0: clear ship orient; cleanup(0x3883d); leaf[0x1c] |= 2 ; done
```

→ **Turns the ship to face the target, holds for 512 `dt`**, with a
range-derived secondary parameter clamped to a **floor of 2500**. **[I]** name.

#### 9.4.2 APPROACH / ATTACK-RUN — fn @ file `0x397c8`  **[C]**

```
delta = target.pos - self.pos ; dist = magnitude(delta) ; D = dist>>8 (world units)
if D <= 0x3e8 (=1000):                       ; CLOSE  -> attack-run branch  [C]
    leaf[0x14] += dt
    if leaf[0x14] >= 0x4c (=76):  rotate_toward_target()/fire action (lcall 0x1dfe:0x1cf)  [C]
    if leaf[0x14] >= 0x99 (=153): return 1   ; attack run complete  [C]
    return 0
else:                                         ; FAR  -> lead-pursuit branch
    lead = ship.get_lead_vector()            ; lcall ship_vtbl[+0x88]
    intercept = compute_intercept(self, target.pos, target.vel)   ; call 0x36e3a
    leaf[0x10] = intercept ; ship.heading(es:[bx+0xa]) = leaf[0x10] ; copy orient
    return 0
```

→ **Beyond 1000 world units: lead-pursuit** (aims at the target's *future*
position using its velocity — true intercept, not stern-chase). **Within 1000
units: a timed attack run** — acts/fires from tick **76** and ends the run at
tick **153** (so the "guns hot" window ≈ ticks 76→153). The **1000-unit
pursue→attack-run switch** is the key engagement distance. **[I]** name.

#### 9.4.3 EVADE / JINK — fn @ file `0x39981`  **[C]**  (this is the §7.13.2 `f3` consumer)

```
if !(leaf[0x1c] & 1):                         ; PHASE 1 (pick a jink)
    r = rng(0:0x96f); axis = (r*4)/0x8000     ; uniform 0..3  [C]
    switch(axis):                             ; jmp word cs:[bx+0x3d21]  (4-entry table)
      0: leaf[0x08] = +0x100   ; +X (yaw/roll one way)
      1: leaf[0x08] = -0x100   ; -X
      2: leaf[0x0c] = +0x100   ; +Y (pitch up)
      3: leaf[0x0c] = -0x100   ; -Y
    r = rng(); roll = (r*3)/0x8000 - 1        ; roll in {-1,0,1} ; leaf[0x10] = roll<<8
    f3 = (leaf[0x2b]&1) ? CNST.f3 : DS:0x2492(=60)         ; per-pilot or default 60
    mag = current angular magnitude
    if mag < 0x3200 (=12800 = 50.0 in 256-fp):  leaf[0x18] = 0x100 (full turn)   [C]
    else: jit = f3 + (rng()*0x1e)/0x8000        ; f3 + up to +30 jitter  [C]
          leaf[0x18] = (jit<<16) / mag          ; turn-rate = jitter / magnitude
    leaf[0x14] = leaf[0x18]                      ; load timer from turn target
    apply leaf[0x08..0x10] to ship heading vector (es:[bx+6/0xa/0xe]) ; leaf[0x1c] |= 1
else:                                          ; PHASE 2 (execute jink)
    leaf[0x14] -= dt
    if leaf[0x14] > 0: keep applying the rotation deltas to the ship
    else: clear flag -> finished
```

→ **Random evasive jink**: pick one of 4 cardinal rotation directions uniformly,
add a random roll (−1/0/+1), and set turn-rate to **full (1.0)** when the ship's
angular magnitude is below **50.0**, else **`f3`-scaled with up to +30 RNG
jitter** divided by magnitude. This is exactly the per-pilot **`f3`
maneuver-jitter** of §7.13.2, now seen in its home maneuver. High `f3`
(pirates/aces 75) → noisier, jerkier evasion; low `f3` (merchants 30/40) →
smoother. **[I]** name.

### 9.5 The deserializer / bytecode grammar  **[C structure / I fields / ? widths]**

The bytecode at `pilotdef+0x0f` is a **serialized object tree** (§7.4 confirmed:
no switch, no jump-table in dispatch). The builder that walks the bytes and
calls the Collection `addElement` ops (`FUN_4e2b`/`FUN_4dcc`, §9.1) is **not in
the AI overlay window** (`ovr_big.bin` has *no* callers of them) — it lives in
the **loader overlay** (`ovr_load.bin`, file `0xb0000+`, near the MNVR loader
`0xb38f6`) or is resident, invoked at AI spawn. **Locating it is the remaining
keystone (`np-l6u`)** — see §9.9.

What the bytes themselves prove **[C]** (cross-checked over all 47 scripts and
the three dumped here):

```
script := <root-class-id> <root operand block>
          <record>*                         ; child maneuver records
          <terminator: 19 19 19>            ; every combat script (SDRONE omits)
record := <hdr byte(s)> <operand block>     ; operand block 8 or 12 bytes [C], width per-class [?]
```

- **Root class id** = lead byte: `0x0e` (32/47, main), `0x0d` (7), `0x0f` (5),
  `0x0b` (2), `0x09` (1 = SDRONE). **[C]**
- Roots open with a fixed **prologue**: `… 03 03 03 <12B> 04 04 04 <12B> …`
  (visible identically in GARVICK/KIL_AF). **[C]**
- Child records carry **3-byte headers** that *triple up class IDs*, e.g.
  `12 11 0a`, `0d 11 12`, `0a 0e 0b`, `11 13 12`, `06 06 07`, `06 06 06`,
  `12 0b 0a`, `0d 12 11` — **[I]** read as `<container/parent> <child-A> <child-B>`
  (a maneuver that nests two sub-maneuvers), consistent with the nested-Collection
  model. **[C]** they exist; **[I]** the field meaning; **[?]** exact widths.
- `0x11`/`0x12`/`0x13` dominate the bodies → the **steer / throttle / fire** leaf
  maneuvers; `0x06`/`0x07` always pair → the **approach + attack** pair
  (matches the APPROACH/ATTACK-RUN leaf, §9.4.2). **[I]**
- `0x19 0x19 0x19` = list terminator/loop. `0x00` = operand fill. `0x01`–`0x08`
  scalar operands; `0x10/0x20/0x40/0x80` single-bit flags. **[C]**

**Mapping recovered leaf functions → class IDs (best current inference):**

| Leaf fn (file) | Behaviour (real numbers) | Likely class ID(s) |
|----------------|--------------------------|--------------------|
| `0x3966f` ORIENT/turn-to-target | face target, hold 512 dt, range param min 2500 | `0x11` or `0x13` **[I]** |
| `0x397c8` APPROACH/ATTACK-RUN | lead-pursuit >1000u; attack-run 76→153 dt ≤1000u | `0x06`/`0x07` pair **[I]** |
| `0x39981` EVADE/JINK | 4-way random jink, roll±1, full turn <50.0, `f3`+30 jitter | `0x12` **[I]** |

The exact ID↔function binding and operand-field semantics are **[?]** until the
deserializer registry (§9.9) is read.

### 9.6 Annotated example pilots

#### 9.6.1 SDRONE (lead `0x09`, 120 B) — the simplest  **[C bytes / I structure]**

```
09                                    root container (variant, SDRONE-only)
04 00 00 00 00 00 00 00 00            root operand (9B; 04 = a count/tier? [I])
0b 0a 11   02 00 00 00 00 00 00 00    record: container 0x0b {0x0a, 0x11}  + op(8)
0d 02 02   01 00 00 00 00 00 00       record: 0x0d {0x02,0x02} + op
11 06 11   00 10 00 00 00 00 00 00    record: steer 0x11 {0x06,0x11}, flag 0x10 + op
06 06 06   00 02 00 00 00 00 00 00    record: approach/attack 0x06 triple, scalar 0x02
0a 12 11   00 04 00 00 00 00 00 00    record: 0x0a {throttle 0x12, steer 0x11}, scalar 0x04
03 03 03   08 00 00 00 00 04 00 80 00 record: prologue-style triple + op(12, flag 0x80)
08 00 00 00 00 00 ...                 (continuation/operand fill)
04 04 04   08 00 00 00 00 04 00 80 00 record: triple + op(12)
0f 04 04   00 01 00 00 00 40 00 00 02 00 00 00 00 00 00 00   record: 0x0f {0x04,0x04} flags 0x01/0x40/0x02
0f 04 04                              trailing 0x0f record (truncated tail; no 19 19 19)
```

→ A **minimal hostile-only brain**: one approach/attack block (`06 06 06`), one
steer+throttle pair (`0a 12 11`), guarded by `0x0f` flag records. Matches the
drone's data: `f6=64` (aggression/commitment baseline, §11), comms/taunt range
`f1=1800` (long bark range, **not** detection — §11), `f3=40` (low jitter — flies
straight, easy kill). The drone has **no flee tier**, consistent with §8.3's
"hostile-only, *This attitude not possible*". Detection/awareness is the sensor
sphere (15000), not `f1`. Operand
field meanings are **[I]/[?]** (need the deserializer).

#### 9.6.2 GARVICK (pirate ace, lead `0x0e`, 191 B)  **[C bytes / I structure]**

```
0e <12B>  03 03 03 <12B>  04 04 04 <12B>      root + standard prologue
12 11 0a  <8B>                                 throttle+steer+container
0d 11 12  <12B, flag 0x20>                      steer/fire block
0a 0e 0b  <12B, flag 0x80>                      nested container
11 13 12  <8B>                                  steer+fire+throttle  (the core combat triad)
13 11 0e  <8B>                                  fire+steer+container
06 06 07  <8B, flag 0x10>                        APPROACH + ATTACK pair
06 06 06  <8B, scalar 0x02, flag 0x20>           second approach/attack
12 0b 0a  <8B>                                   throttle+containers
0d 12 11  <12B>                                  steer/throttle
03 03 03  <12B, flag 0x80>                       trailing prologue-style
0f 04 04  <op, flags 0x01/0x40/0x02>             guarded tail
19 19 19                                         terminator
```

→ Aggressive multi-maneuver brain: **two** approach/attack blocks (`06 06 07`,
`06 06 06`) plus the steer/fire/throttle triad (`11 13 12`). Data: `f3=75` (high
jitter → jinky), `f6=102` (stands and fights). **[I]** beyond token level.

#### 9.6.3 KIL_AF (Kilrathi, lead `0x0e`, 199 B)  **[C bytes / I structure]**

KIL_AF is **GARVICK + exactly one extra child record** (`§7.6`): note the
Kilrathi-only `0d 0c 0a` block and an extra `06 11 0f` record vs GARVICK's
`12 0b 0a`. Same prologue, same `0f 04 04` tail, same `19 19 19`. The extra
`0x0c` (Kilrathi-only sub-maneuver, §7.5) + a third combat record = "presses the
attack harder". Data: `f3=40` but `f6=64` (fight-to-death, never flees). The
aggression here is **structural** (more attack records) and via `f6`, not `f3`.
**[I]** beyond token level.

### 9.7 The 3-morale-tier model  **[C array / I selection]**

- The brain holds a **3-slot maneuver-tree array** at `brain+0x0f`
  (`FUN_336b(brain,3)`), hard-confirmed by the `idx<3` dispatch bound (§9.3) and
  matching `MANEUVER.IFF`'s three `FORM MORL`. **[C]**
- Dispatch (§9.3) iterates all ≤3 slots and runs the first whose **target key**
  matches; it is **not** a morale-indexed `array[morale]` lookup in this path.
  So morale most plausibly governs **which tier(s) are populated/active**, with
  the matching slot's condition (`+0x0c`) gating execution. **[I]**
- **Morale transitions / `f6`.** No resident or carved-overlay consumer of CNST
  `f6` (+0x0C) was found (§7.13.5, exhaustive). The strong, still-unproven model:
  `f6` seeds the starting morale tier (Kilrathi 64 = top "fight" tier always;
  merchant 128 = bottom "flee" tier early), with HP/incoming-fire driving
  transitions. This remains **[PENDING]** — confirm via the dynamic overlay-load
  trace (§7.13.4), which is the only route left after the static dead-ends.

### 9.8 Reimplementation mapping → `src/ship_ai.{h,cpp}`

Concrete bridge from the decode to our code. **Bold = ready to wire now**;
*italic = needs the deserializer/dynamic trace first.*

| Privateer (decoded) | Our knob / state | Action |
|---------------------|------------------|--------|
| ORIENT maneuver: face target, hold **512 dt** | `Engage` turn-to-target + dwell | replace ad-hoc turn with face-then-hold; expose a `k_orient_hold` ≈ 512 dt **[I]** |
| APPROACH switch **D=1000 world units** (pursue↔attack-run) | `k_engage_distance` / state split | **adopt 1000 u as the pursue→attack-run boundary** (we currently conflate `weapons_range`=3000 / `k_break_distance_m`=150) |
| Attack-run window **ticks 76→153** (guns hot 76, end 153) | `k_firing_min/max` (we use 3–6 s) | replace our seconds with the 76→153 dt window once `dt` is pinned **[I dt]** |
| Lead-pursuit (`compute_intercept` at `0x36e3a`, uses target velocity) | our stern-chase Engage steering | **switch Engage to true lead-pursuit** (aim at target future pos) — biggest feel win |
| EVADE: 4-way random jink + roll±1; **full turn when angular mag <50.0** | `BreakOff`/evade steering | model break-off as random-axis jink, not a fixed vector |
| EVADE jitter **`f3` + RNG·30/32768**, turn = jitter/mag | steering RNG amplitude | **wire `f3` (per-pilot) as evade-jitter gain** (pirates 75, merchants 30) — confirmed §7.13.2 |
| `magnitude` = alpha-max-beta-gamma, coords **256-fixed-pt**, distances `>>8` | our float meters | keep floats; note ~±8 % vs Euclid (§7.11) — ignore unless byte-parity wanted |
| comms/taunt **f1=1500**, break-off **f0=600+radii**, sensor **15000** | comm-bark range / break-off / radar-detection | **adopt the stack** (§0, §7.12): detection = sensor **15000**; comms bark = **f1** (NOT detection); break-off/disengage = **f0** (NOT gun range — guns use per-gun `range_m`) |
| 3 morale tiers (`brain+0x0f[3]`) | single `ShipAIState` | *promote to a 3-tier behaviour set; **`f6` = morale/caution** (§0: low=fanatical, high=timid) seeds the tier + flee HP threshold **[I tuning]*** |
| condition (`+0x0c`) / update (`+0x14`) split | our transition / act split | the split already matches; bind each leaf maneuver to one act block |
| class-ID → leaf class + operand fields | the per-record numbers | *blocked on the deserializer (`np-l6u`)* |

**Recovered constants to hardcode now (real, [C] unless noted):**
`break-off/disengage f0=600 (+hull radii)` (**NOT gun range**, §0), `comms/taunt
f1=1500` (drone 1800, Confed 2100 — **comms bark range, NOT detection**),
`sensor/detection=15000`,
`pursue→attack-run = 1000 world units`, `attack-run window =
76→153 dt`, `orient hold = 512 dt`, `evade "full turn" angular threshold = 50.0`,
`evade jitter = f3 + RNG·30/32768`, `ORIENT range-floor = 2500`. The `dt`→seconds
factor is **[?]** (pin it with the §7.13.4 dynamic trace before converting the
76/153/512 timers to real time).

### 9.9 What is still open (resume here)  **[?]**

1. **The deserializer registry (`np-l6u`) — keystone.** Find the byte→leaf-class
   builder. It calls the Collection `addElement` (`FUN_4e2b`/`FUN_4dcc` in
   `ovr_big.bin`) but lives elsewhere: scan `ovr_load.bin` (file `0xb0000+`, near
   MNVR loader `0xb38f6`) and the resident spawn path for **far-calls into the
   AI-overlay add functions**, then read how it maps the lead byte / 3-byte
   header to a leaf vtable and how many operand bytes each class consumes. That
   closes the grammar (operand widths `[?]`) and the ID↔function map (§9.5).
2. **Leaf-class vtable enumeration.** Confirm the ID↔function bindings in §9.5 by
   reading each leaf class's vtable `+0x0c`/`+0x14` (the resident `0x396xx`
   family has more functions at `0x38f20`, plus the ship-accessor vtable slots
   `+0x78/+0x88/+0x38` used for orient/lead).
3. **`dt` (DS:0x2768) absolute value + AI Hz** — dynamic (DOSBox-X), to convert
   76/153/512 timers to seconds.
4. **`f6` morale model** — dynamic overlay-load trace (§7.13.4); static is fully
   exhausted (§7.13.5).

> Evidence is derived facts from the user's own legal `PRCD.EXE`. Carves, Ghidra
> projects, capstone scans and decompiler output stay gitignored under `re/`
> (verified `git check-ignore`). Only this derived summary is committed.

## 10. MNVR maneuver system — decode pass 1 (the deserializer grammar, keystone)

**Status:** this is the §7.7 / §9.9-#1 keystone (bead `np-l6u`). §9 already
recovered the runtime object model, the three leaf maneuvers with real numbers
(GOAL 2) and a structural read of SDRONE/GARVICK/KIL_AF (GOAL 3); what it left
**[?] open** was the **byte→object deserializer grammar** — the per-class operand
widths and child nesting. This pass nails the *record framing* and recovers
**exact, validated record boundaries** for SDRONE/GARVICK/KIL_AF, proves the
grammar is a **recursive streamable tree** (operand width is *not* a function of
the class id alone), and rules the deserializer **out** of the resident maneuver
cluster. The remaining open piece is the per-class width *rule* (needs the
load-overlay deserializer body). Tags: **[C]** byte/disasm fact, **[I]**
inference, **[?]** open.

> **Provenance / legality.** Derived facts from the user's own legal GOG
> `PRCD.EXE`. Ghidra projects, the EXE copy, overlay carves, capstone scans and
> all decompiler output stay **gitignored** under `re/` (re-verified this pass:
> `git check-ignore re/ re/ovr_ai.bin re/ovr_load.bin re/PRCD.EXE` and the new
> `re/grammar_*.py` probes all return ignored). Only this derived doc + the
> code-only `tools/ghidra/` helpers are tracked. OpenMW-style clean-room policy.

### 10.0 Method (reproducible, clean-room)

The engine consumes the MNVR bytes **byte-for-byte unchanged** (§7.3 — the JSON
oracle is faithful), so the deserializer grammar must be **self-consistent across
all 47 verbatim scripts**. Local probes under `re/` (gitignored):
`grammar_probe.py` (stride search), `grammar_diff.py` (prefix/suffix alignment of
near-identical scripts), `grammar_widths.py` (per-class width table test),
`grammar_final.py` (greedy-to-next-header record dump). Cross-checked against the
resident maneuver-cluster decompile (`re/decomp_out.txt`) and the AI-overlay
decompile (`re/decomp_big.txt`).

### 10.1 The record framing  **[C]**

```
script := <root>  <record>*  <terminator>
root   := class_id(1)  operand(W_root)
record := header(3)    operand(W_rec)        ; header = three class-id bytes
term   := 0x19 {0x19}                         ; run of 0x19, normally "19 19 19"
```

Hard facts established this pass:

- **Root** = 1 class-id byte + an operand. `W_root = 12` for the common `0x0e`
  / `0x0d` roots; **`W_root = 8` for SDRONE's `0x09` root**. **[C]**
- The **0x0e prologue is a fixed 43-byte run** across all 32 `0x0e`-lead pilots
  (longest common prefix): `0e` + 12B, then `03 03 03` + 12B, then `04 04 04` +
  12B. Identical bytes in every `0x0e` pilot. **[C]**
- A record header is **three consecutive class-id bytes** (a "class triple"),
  e.g. `03 03 03`, `04 04 04`, `12 11 0a`, `0d 11 12`, `0f 04 04`. **[C]**
- **46/47 scripts terminate with `19 19 19`.** Only SDRONE is **truncated** (no
  terminator — ends mid-`0f 04 04`), matching §9.6.1. **[C]**
- **15-byte records (`3 header + 12 operand`) == `operator new(0xf)`** from §7.4.
  The container/prologue records (`03`/`04`) are exactly 15 bytes, tying the
  on-disk record size to the in-memory maneuver-object size. **[C — new, strong]**
- **Leaf records are 11 bytes (`3 header + 8 operand`).** **[C]**

### 10.2 Operand width is context-dependent — it is a recursive tree  **[C]**

A flat fixed-stride parse fails on **every** script (`grammar_probe.py`: 0/47).
A per-`header[0]` width table **also fails**, and the counterexample is decisive:
in GARVICK, class `0x0d` appears **both** as a 12-byte record (`0d 11 12` @off 54)
**and** as an 8-byte record (`0d 12 11` @off 143). Same leading class id, two
different operand widths in one script. **[C]**

→ Operand width therefore depends on the record's **content / nesting**, not on a
static `id→width` map. This *confirms* the §7.4/§9.5 model: the bytecode is a
**serialized object tree**, the deserializer is a recursive streamable factory
(read class, read that class's fixed fields, recurse for children), and there is
**no flat `opcode→width` table** to recover — the "width" is whatever that
class's `read()` consumes, including nested child records. The widths seen are
**8, 12, and 16 bytes** (16 for the `0f 04 04` guarded-tail and some `03 03 03`
container variants). **[C]** observed; the **exact per-class field layout is [?]**
until the deserializer body is read.

### 10.3 Where the deserializer is NOT — resident cluster ruled out  **[C, negative]**

The resident maneuver-management cluster (`re/decomp_out.txt`, overlay `0x83xxx`)
was read in full:

| fn | role (decompiled) |
|----|-------------------|
| `FUN_3493` | per-tick condition dispatch (§9.3): match target key, run vtable `+0x0c` |
| `FUN_37e7` | target-key match + run destroy `+0x24` on a tier slot |
| `FUN_38d9` | per-tick: for each of 3 slots run `+0x0c`→`+0x14`→`+0x18` (the update pump) |
| `FUN_3a50` | build a 4-word snapshot of the 3 slots (`FUN_336b(brain,4)`) |
| `FUN_3b3e` | **clone/rebuild** the 3-slot tree from a context (`func 0x83b4`), bitfield copy into `brain+0x0c..0x0e` |
| `FUN_3d04` | per-slot run `+0x10` then `+0x14` |
| `FUN_3d72` | count empty slots (`return #slots==0`) |
| `FUN_3da4` | advance to next non-empty slot (iterator) |
| `FUN_3613`/`FUN_3700` | lazy-alloc 3-slot array (`FUN_336b(brain,3)`) + `new` a Collection (vtable `0x20d`→`0x235`) into the first free slot |

**Every one of these operates on an *already-built* 3-slot object tree at
`brain+0x0f`. None reads the raw `pilotdef+0x0f` byte-stream.** The tier-factory
(`FUN_363d`/`FUN_3700`) always instantiates the **Collection** class
(`0x20d`→`0x235`) regardless of any class id — so the per-class *leaf* factory
(the registry that switches on the lead byte) is a **separate** function that is
**not** in this cluster. **[C]**

→ The deserializer runs at **AIDS-load / AI-spawn time in the load overlay**
(`re/ovr_load.bin`, file `0xb0000+`, near the MNVR loader `0xb38f6`), reading the
verbatim stream and `new`-ing the leaf objects. Its exact address is still
**[?]** — see resume list.

### 10.4 Annotated record dumps (greedy-to-next-header; GARVICK boundaries hand-verified)  **[C bytes / I record meaning]**

Operand widths below are recovered by running each operand up to the next valid
class-triple header (or the `0x19` terminator). This **exactly reproduces the
hand-verified GARVICK boundaries** (every record lands on a real triple header,
the script ends cleanly on `19 19 19`), which validates the method.

**SDRONE** (lead `0x09`, 120 B, simplest — GOAL 3):
```
ROOT   id=09  op[ 8]=04 00 00 00 00 00 00 00          minimal root container (W_root=8)
REC    0b 0a 11  op[ 8]=02 00 00 00 00 00 00 00         container{0a,11}, param=02
REC    0d 02 02  op[ 8]=01 00 00 00 00 00 00 00         leaf 0d{02,02}, param=01
REC    11 06 11  op[ 8]=00 10 00 00 00 00 00 00         steer 0x11{06,11}, flag 0x10
REC    06 06 06  op[ 8]=00 02 00 00 00 00 00 00         approach/attack triple, scalar 02
REC    0a 12 11  op[ 8]=00 04 00 00 00 00 00 00         0a{throttle 12, steer 11}, scalar 04
REC    03 03 03  op[16]=08 00 00 00 00 04 00 80 00 08 …  container, wide operand (flags 04/80)
REC    04 04 04  op[12]=08 00 00 00 00 04 00 80 00 00 …  container (flag 04/80)
REC    0f 04 04  op[16]=00 01 00 00 00 40 00 00 02 00 …  guarded tail (flags 01/40/02)
REC    0f 04 04  op[ 0]= (truncated — no 19 19 19)
```
→ A minimal **hostile-only** brain: one approach/attack block (`06 06 06`), one
steer+throttle pair (`0a 12 11`), guarded by `0f 04 04` records. Matches the
drone data (`f6=64` fight-to-death, `f1=1800` long sensor, `f3=40` low jitter —
flies straight, easy kill) and §8.3's "hostile-only". The leaf operands read as
`byte0 = scalar param, byte1 = flag bits (0x10/0x02/0x04), rest 0` **[I]**; exact
field meaning **[?]** (needs the deserializer).

**GARVICK** (pirate ace, lead `0x0e`, 191 B — *fully clean parse, ends 19 19 19*):
```
ROOT   id=0e  op[12]=20 00 00 00 00 04 00 00 00 00 00 00
REC    03 03 03  op[12]=20 00 00 00 00 04 00 80 00 00 00 00   prologue container
REC    04 04 04  op[12]=02 00 00 00 00 20 00 00 00 00 00 00   prologue container
REC    12 11 0a  op[ 8]=02 00 00 00 00 00 00 00              throttle{steer,container}
REC    0d 11 12  op[12]=04 00 00 00 00 20 00 00 00 00 00 00   steer/fire container
REC    0a 0e 0b  op[12]=04 00 00 00 80 00 00 00 00 00 00 00   nested container (flag 80)
REC    11 13 12  op[ 8]=04 00 00 00 00 00 00 00              steer+fire+throttle triad
REC    13 11 0e  op[ 8]=01 00 00 00 00 00 00 00              fire+steer+container
REC    06 06 07  op[ 8]=00 10 00 00 00 00 00 00              APPROACH+ATTACK pair (flag 10)
REC    06 06 06  op[12]=00 02 00 00 00 20 00 00 00 00 00 00   second approach/attack
REC    12 0b 0a  op[ 8]=00 02 00 00 00 00 00 00              throttle+containers
REC    0d 12 11  op[ 8]=00 04 00 00 00 00 00 00              steer/throttle
REC    03 03 03  op[12]=08 00 00 00 00 04 00 80 00 00 00 00   trailing container
REC    0f 04 04  op[16]=00 01 00 00 00 40 00 00 02 00 00 00…  guarded tail
TERM   19 19 19
```

**KIL_AF** (Kilrathi, lead `0x0e`, 199 B — *fully clean parse, ends 19 19 19*):
```
ROOT   0e op[12] …                              (same prologue as GARVICK)
03 03 03 op[12] / 04 04 04 op[12]               (shared prologue)
0d 0c 0a op[ 8]=02 …    <- Kilrathi-only 0x0c sub-maneuver (§7.5), an extra record
0d 11 12 op[16]=04 00 00 00 00 20 00 00 00 02 … <- 16B here vs GARVICK's 12B (the +bytes)
11 0e 12 op[12] / 0a 0b 11 op[8] / 11 13 11 op[8]
06 07 06 op[8] / 06 06 06 op[8] / 06 11 0f op[8]   <- THREE 06-cluster records (vs GARVICK 2)
03 03 03 op[16] / 04 04 04 op[12] / 0f 04 04 op[16]
TERM   19 19 19
```
→ KIL_AF = GARVICK's skeleton **+ the Kilrathi-only `0d 0c 0a` record + a third
`06`-cluster (attack) record + a wider `0d 11 12`** (16 B vs 12 B). The +8 bytes
of §7.6 are these structural additions = "presses the attack harder" — confirmed
at the record level this pass. **[C]** bytes; **[I]** behaviour reading.

### 10.5 Width dichotomy summary (corpus-wide)  **[C]**

| operand width | record size | seen on (header[0]) | reading **[I]** |
|---------------|-------------|---------------------|-----------------|
| 8  | 11 B | `06 07 0a 0b 0c 0d 11 12 13` leaf records | a **leaf maneuver**: 1 scalar param + flag byte |
| 12 | 15 B | `03 04` prologue containers; some `0a 0d 06 11` | a **container** maneuver (`new(0xf)`) w/ extra flag dwords |
| 16 | 19 B | `0f 04 04` guarded tail; some `03 03 03` | a container w/ an extra child/flag block |
| run | — | `0x19…` | list terminator / loop |

The 8↔12↔16 choice is **content/nesting-driven** (§10.2), so this table is a
*description of what the corpus contains*, **not** a decoder. The decoder is the
class `read()` body **[?]**.

### 10.6 Reimplementation note

Nothing here changes the §9.8 wiring recommendations (those come from the decoded
leaf maneuvers, which are independent of the deserializer). What §10 adds for the
reimplementation: a **validated parser** for the record framing
(`re/grammar_final.py`) we can use to (a) sanity-check any future per-class width
rule, and (b) drive a data-driven importer once the leaf-class `read()` layouts
are known. The 15-byte-record ↔ `new(0xf)` identity (§10.1) is the bridge: each
on-disk record materialises one maneuver object.

### 10.7 Resume here (pass 2)  **[?]**

In priority order (still all gated on the same keystone):

1. **The leaf-class factory / class registry (np-l6u) — keystone.** It is *not*
   in the resident cluster (§10.3, ruled out). Carve/decompile `re/ovr_load.bin`
   (file `0xb0000+`) around the MNVR loader `0xb38f6` and the AI-spawn path for
   the function that reads `pilotdef+0x0f` byte-by-byte and `new`s a leaf object
   per class id. Recover: (a) the `class_id → ctor/vtable` map (the registry),
   (b) each class's `read()` field layout = the **per-class operand width rule**
   that §10.2/§10.5 could only bound. That closes the grammar.
2. **Bind leaf functions → class ids.** Confirm the §9.5 inferences
   (`0x11/0x13`=ORIENT, `0x06/0x07`=APPROACH/ATTACK-RUN, `0x12`=EVADE) by
   matching each class's `read()`/vtable to the resident leaf bodies
   (`0x3966f`/`0x397c8`/`0x39981`, §9.4) once the registry (step 1) gives the
   id↔vtable map.
3. **Decode a pirate + a Kilrathi end-to-end.** GARVICK and KIL_AF already parse
   cleanly to `19 19 19` (§10.4); once steps 1–2 give field meanings, annotate
   every operand (param/flag) to produce full behaviour trees — then diff
   pirate vs Kilrathi vs merchant for the per-faction maneuver mix.
4. (carried from §9.9) `dt` (`DS:0x2768`) absolute value + AI Hz (dynamic);
   `f6` morale model (dynamic overlay-load trace, §7.13.4).

> Evidence is derived facts from the user's own legal `PRCD.EXE`. Carves, Ghidra
> projects, capstone scans and decompiler output stay gitignored under `re/`
> (verified `git check-ignore`). Only this derived summary is committed.

## 11. Live-test corrections (vanilla Privateer, in-game ground truth)  **[L]**

**Status:** this section records **live in-game testing on vanilla Privateer**
(the user editing CNST values in a running build and observing NPC behaviour).
Live behaviour is **ground truth** and **overrides any static label** where they
disagree. The disassembly in §7.9-§7.13 is *not* wrong about the *mechanics*
(the compares, the scale, the consumers) - it was wrong about some *purpose
labels*, which this section fixes. Tag **[L]** = live-test fact, **[C]** =
disasm fact, **[?]** = open.

### 11.1 `f1` is COMMS / TAUNT-CHATTER range, NOT detection  **[L]**

- **Test:** raise `f1` to 8000 / 12000, lower to 500, watch the NPC.
- **Result:** raising `f1` made the NPC fire its **taunt / comm bark from much
  farther out** while it was still closing; lowering `f1` to 500 did **not**
  change *when it detected* the player or *when it engaged*.
- **Conclusion:** the `dist <= f1` compare at `0x1ce8c` (real, **[C]** §7.9) is
  the **"am I close enough to bark at this target?" comms trigger**, not a
  detection/awareness gate. Per-pilot: drone 1800, Confed escort 2100 = longer
  taunt range. **The "detection / awareness / combat-commit" label on `f1` is
  rejected.**

### 11.2 Detection / awareness / engage-trigger = SENSOR sphere 15000  **[L]**

- **Observation:** the NPC detected the player and began closing from
  **~10,000 world units** - far beyond `f1`=1500. Awareness = being a contact
  inside the **sensor / radar sphere (15000, `0x3a98`)**, gated by **faction
  stance** (already modelled in `perception.cpp`). This is what actually wakes
  the AI and starts the engage, matching what the player sees.
- **Net three-tier range model (corrected, supersedes the old
  "sensor superset detect superset engage" framing):**
  `SENSOR/detection 15000  >  COMMS/taunt f1 1500  >  BREAK-OFF f0 600`,
  with the pursue->attack-run switch at **1000** (gun fire is separate: per-gun
  `range_m` + arc). All are **distinct,
  same-scale** world-unit constants - **there is no x10** between any of them
  (the old "x10" was an illusion from reading the 15000 sensor range as if it
  were 10x the 1500 `f1`; refuted in §7.9/§7.10 and confirmed here).

### 11.3 `f0` is BREAK-OFF / DISENGAGE radius - confirmed  **[L]**  [CORRECTED, see §0: NOT gun range]

- **Test:** `f0`=5000 made the NPC **break off at ~5 klicks**; small `f0` made it
  **loiter in close**. Matches the disasm compare `dist < (f0 + selfR +
  targetR)<<8` (§7.9). `f0`=600 default = the **break-off / disengage** radius:
  too close → turn away, afterburn out, loop back. It is **NOT** gun range — gun
  fire is gated by per-gun `range_m` + arc during the attack-run (§0). **[C]+[L]**

### 11.4 `f6` is MORALE / CAUTION  **[SUPERSEDED by §0 — see correction below]**

> **[CORRECTED, see §0 — source of truth.]** This entry originally concluded
> `f6` was an "aggression / attack-commitment" scalar from the live pokes below.
> That conclusion is **withdrawn.** `f6` has **no traceable resident consumer**
> (§7.13.5, exhaustive) — there is nothing in the running image for a debugger
> poke to affect, so the "more shots" observation was **noise / confirmation
> bias**, not a real effect. The resolved role (GameFAQs WC:Privateer FAQ §5.1
> "Morale and Experience" + the canonical per-faction values) is:
> **`f6` = morale / caution.** **LOW = fanatical** (Kilrathi 64, fight-to-death),
> **HIGH = timid** (merchant 128, flees early) — a clean monotone caution ladder.
> We **do** wire it to a flee HP threshold **[I tuning]** (the formula is ours
> since there is no consumer to copy; the *direction* is FAQ-confirmed).

- **Original (withdrawn) test note:** `f6`=255 *seemed* to keep the NPC on its
  attack vector longer; `f6`=0 was inconclusive. Given no `+0x0C` consumer
  exists, treat both as noise. Mechanism stays **[PENDING]** a dynamic
  overlay-load trace, but the *role* (morale/caution) is settled by §0.

### 11.5 Implications for the reimplementation (what we wired in PART B)

- **Detection/awareness** -> sensor/radar range **15000** + faction stance
  (`perception.cpp`); `f1` is **not** consulted for perception.
- **`f1`** -> hostile **comm/taunt range** in `comm.cpp` (per-pilot).
- **`f0`** -> break-off / disengage radius (`f0 + selfR + targetR`) in the
  Engage/BreakOff states (`ship_ai.cpp`), replacing the old fixed
  `k_break_distance_m`. NOT gun range (guns use per-gun `range_m`).
- **`f3`** -> evade/jink jitter gain in BreakOff (`ship_ai.cpp`).
- **`f6`** -> morale/caution: derive the flee HP threshold (§0: low=fanatical
  fights to death, high=timid flees early) **[I tuning]**, mechanism PENDING but
  role FAQ-confirmed.

> Evidence: live in-game testing on the user's own legal vanilla Privateer plus
> the derived disasm facts (§7). RE artifacts stay gitignored under `re/`.

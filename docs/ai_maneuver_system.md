# Data-driven condition->maneuver AI system

This document describes new_privateer's combat AI: a **data-driven
condition->maneuver table** evaluated each tick, with a priority-interrupt
channel and three morale tiers, driven by our **vanilla-decoded** numbers.

## 0. Clean-room / license note (read first)

This system is a **clean-room reimplementation**. The *architecture* (a per-tick
table of `{metric-in-a-window -> named maneuver for N seconds}`, a normal
selection channel plus a priority-gated interrupt channel, a catalog of named
maneuver routines, per-pilot personality seeding, an aggressivity-scaled firing
cone) is the pattern shared by **two** originals:

- **vanilla Privateer** -- the decoded 3-tier MNVR maneuver-object tree, whose
  per-tick dispatch runs each maneuver's `vtable +0x0c` *condition* and `+0x14`
  *update* (`docs/ai_model.md` s9.3-9.4); and
- **Privateer Gemini Gold / Vega Strike** -- the XML `condition->script`
  `AggressiveAI` (analysed, GPL, in `docs/ai_vegastrike_comparison.md`).

Those two are structurally the same idea wearing different clothes
(`docs/ai_vegastrike_comparison.md` s5). We reimplement the **concepts** from
scratch in our own types for the Apache-2.0 engine. **No GPL code or text was
copied** from Vega Strike / PGG; only ideas/patterns were borrowed, which is
permitted. All **numbers** come from the vanilla decode
(`docs/ai_model.md` s0 -- the source of truth), NOT from PGG's data tree.
Distances are adopted 1:1 through the existing `kPvtScale` knob.

Where the two originals disagree on a value, vanilla wins -- it is *this game's*
AI; Vega Strike is a cousin we borrow structure from, not numbers.

## 1. Data model (`src/ai_maneuver.h`)

Three nested types describe a logic table; a fourth is the per-ship runtime.

```
AICondClause : one metric tested against a [min,max) window (+ optional negate
               = "outside the window"). min/max are AIScalars: a literal OR a
               per-pilot CNST token resolved against the ship at eval time.
AILogicItem  : { clauses (AND), maneuver, priority, duration_s }  -- one rule.
AIMoraleTier : { logic[], interrupt[] }  -- two channels of rules.
AILogicTable : { name, tiers[3] }        -- 3 morale tiers (timid/steady/fanatical).
```

The **static table** is shared and loaded once from `assets/ai/*.ai.json`
(registry in `ai_brain.cpp`). The **per-ship runtime** lives on `ShipAIState`
(`src/ship_ai.h`): `brain` (resolved table), `morale_tier`, `cur_maneuver`,
`cur_priority`, `cur_duration`, `maneuver_started_at`, `personality_seed`,
`fire_solution_at`. Same shared-vs-instance split as `ShipClass` vs `Ship`.

### 1.1 Condition metrics (`enum AICondition`)

Computed per tick against the window. Kept small and ours (a subset of vanilla's
used set plus the PGG metrics our engine can actually compute):

| metric (`json "metric"`) | meaning | units |
|--------------------------|---------|-------|
| `distance`         | observer -> target distance | world units (raw, kPvtScale 1:1) |
| `hull`             | self hull fraction | 0..1 |
| `shield`           | self shield fraction (avg facings) | 0..1 |
| `target_in_front`  | cos(angle nose->target) | -1..1 (1 = dead ahead) |
| `target_facing_me` | cos(angle target-nose->me) | -1..1 (1 = it aims at me) |
| `target_in_range`  | within gun `range_m`? | 0 or 1 |
| `random`           | fresh uniform each eval | 0..1 |
| `closing_rate`     | line-of-sight closing speed | m/s (+ = closing) |
| `time_in_state`    | seconds the current maneuver has run | s |

### 1.2 Maneuver catalog (`enum AIManeuver`)

Clean reimplementations, each writes our `behavior` / `controller` outputs
(`desired_forward` via a far aim point, `speed_scale`, `afterburner`,
`fire_guns`). The first four are ports of the prior working math
(`docs/ai_model.md` s9.4); the rest are new, borrowed (concept-only) from the
PGG/VS catalog (`docs/ai_vegastrike_comparison.md` s2).

| maneuver (`json`)       | behaviour | status |
|-------------------------|-----------|--------|
| `lead_pursuit`          | intercept-aim close, guns cold (vanilla APPROACH far) | live |
| `attack_run`            | close + fire on geometry (in arc + gun range + reaction gate), throttle 0.5 (vanilla 0x397c8) | live |
| `break_off`             | afterburn extend along a cached lateral+away diagonal (vanilla EVADE) | live |
| `evade_jink`            | pure 1-of-4 cardinal jink + roll, f3-scaled, afterburn (vanilla 0x39981) | live |
| `barrel_roll`           | corkscrew approach (spiral the aim), fire when lined up | live |
| `loop_around`           | overshoot-and-curl: face+close if target behind, else curl to an offset point | live |
| `evade_left_right`      | sign-flipping lateral weave, afterburn | live |
| `evade_up_down`         | sign-flipping vertical weave, afterburn | live |
| `face_perpendicular`    | fly perpendicular to target facing (deflection-shot setup) | live |
| `turn_away`             | point/run directly away | live |
| `match_speed`           | hold aim, match target speed (Kickstop-ish) | live |
| `flee_home`             | run away + home-tether + sinusoidal jink (existing Flee) | live |

### 1.3 Per-pilot scalar tokens (`enum AIScalarToken`)

A window edge can be a literal number OR a token resolved per-ship, so one table
fits every pilot's own CNST. JSON: a number -> literal; a string -> token.

`f0`, `f0_radii` (= f0 + selfR + targetR, the real break-off radius), `f1`,
`f2`, `f3`, `f6`, `pursue_switch` (1000), `gun_range` (class `weapons_range`),
`flee_threshold` (f6-derived flee HP fraction), `sensor` (radar_range, 15000).

## 2. The evaluator (`src/ai_brain.cpp`)

`ship_ai::tick` (thin, in `ship_ai.cpp`) handles the non-combat gating
(Idle/Patrol fallthrough, unchanged) and, when perception reports a hostile,
delegates to `ai_brain::run_combat`. Per combat tick:

1. **Resolve** the ship's table (lazy, by faction, fallback `default`) and stamp
   a stable `personality_seed`.
2. **Pick the morale tier** from CNST `f6` (s4).
3. **Interrupt channel** -- `select(interrupt, priority > cur_priority)`: the
   highest-priority *passing* rule strictly above the running maneuver's
   priority preempts (e.g. "hull critical -> flee NOW", "too close -> break").
4. **Else hold** the current maneuver until `(t_now - started) >= cur_duration`.
5. **Else logic channel** -- `select(logic)`: the highest-priority passing rule
   (re)selects the next maneuver and starts its timer.
6. **Execute** the selected maneuver handler (writes `behavior`/`controller`).
7. Map the running maneuver onto the legacy `AIState` enum for HUD/debug.

This is the clean-room twin of vanilla's per-tick `+0x0c`/`+0x14` dispatch and
PGG's two-pass `ProcessLogic` (logic + interrupt). A rule is reachable only
while its metric sits inside its authored window, and is held for its
`duration` before reselection -- exactly the "bounded-time maneuver object with
its own timer" both originals use.

Two deliberate interactions keep combat readable:

- **Random-gated maneuvers re-roll on a decision cadence, not per frame.** The
  `random` metric is quantized to `k_decision_hz` (5 Hz) so a window `[0,p]`
  means ~`p` probability per decision, framerate-independent. (A per-frame roll
  at 60 fps made occasional-evade windows fire ~84%/sec -- ships evaded
  continuously.)
- **Evade does not preempt a firing pass.** The random-evade interrupt rules sit
  at the SAME priority as `attack_run` (5). Because interrupts require strictly
  *higher* priority to preempt, an evade can interrupt `lead_pursuit` (the
  approach, priority 4) but NOT an active `attack_run` -- so a ship in its
  firing pass actually shoots. `break_off` (6) and `flee_home` (8) still preempt
  everything, as they should.

One personality nuance the metrics can't express -- "a hunted Coward flees" --
stays as a code override in `ship_ai.cpp` (`run_forced(FleeHome)`); un-hunted
cowards fall through to the table and fight.

### 2.1 Firing decision (the firing upgrade)

`should_fire` replaces the old fixed 15-degree instant cone. It gates on
GEOMETRY (arc + range + reaction time), NOT on the decoded 76->153 tick window:
the AI tick rate `dt` (`DS:0x2768`) is still `[?]` (s9.3), so that 7.6s..15.3s
seconds-conversion is untrustworthy and previously STARVED the guns (the attack
run was interrupted before its window ever opened). The tick numbers stay `[?]`
until `dt` is pinned. `should_fire`:

- **ITTS lead** -- we aim at the computed intercept point (kept from the prior
  Engage math).
- **Aggressivity-scaled cone** -- the firing half-angle interpolates ~10deg
  (tight) .. ~18deg (wide) by an aggressivity derived from CNST `f3`
  (experience): pirates (`f3=75`) fire at wider angles, merchants (`f3=30`)
  tighter.
- **Reaction-time gate** -- the first shot is held until the firing solution
  (cone aligned + in gun range) has stood for a per-pilot reaction time scaled
  by CNST `f2` (skill): `f2=40 -> 0.45 s`, `f2=60 -> 0.12 s`. No snap-firing.

Both mappings are `[I]` tuning (documented; `docs/ai_model.md` s0 / FAQ s5.1).

## 3. JSON schema (`assets/ai/*.ai.json`)

Loaded at startup by `ai_brain::load_all("assets/ai")` (same pattern as
`ship.json`). One file per table; filename `*.ai.json`. A ship picks the table
named after its faction, else `default`.

```jsonc
{
  "name": "default",
  "tiers": {
    "timid":     { "logic": [ <item>... ], "interrupt": [ <item>... ] },
    "steady":    { "logic": [ ... ],       "interrupt": [ ... ] },
    "fanatical": { "logic": [ ... ],       "interrupt": [ ... ] }
  }
}
```

A `<item>` (a logic rule):

```jsonc
{
  "maneuver": "attack_run",   // one of the AIManeuver names (s1.2)
  "priority": 5,              // higher wins; interrupt needs > running priority
  "duration": 15.3,          // seconds before reselection
  "when": [                  // AND of clauses; empty/absent = always passes
    { "metric": "distance", "min": "f0_radii", "max": "pursue_switch" }
    // "min"/"max": a number (literal) or a token string (s1.3)
    // "not": true  -> pass when the metric is OUTSIDE [min,max)
  ]
}
```

The shipped `default.ai.json` reproduces the decoded vanilla behaviour:
`distance > 1000 -> lead_pursuit`; `[f0_radii, 1000] -> attack_run` (fire in the
fire on arc+range geometry); `< f0_radii -> break_off`; `hull < flee_threshold ->
flee_home`; periodic `random -> evade/barrel-roll` on the interrupt channel.

## 4. The three morale tiers

Each table carries `timid` / `steady` / `fanatical` tiers; the active tier is
selected per-ship from CNST `f6` (morale/caution, `docs/ai_model.md` s0):

| `f6` | tier | feel |
|------|------|------|
| `<= 70`  | **fanatical** | Kilrathi 64, drone 64: presses the attack, barrel-rolls, flees only at ~5% hull |
| 71..109  | **steady**    | baseline 76, pirate/Confed 102: pursue/attack/break, flees at the f6 HP threshold |
| `>= 110` | **timid**     | merchant 128: flees early at its (high) `flee_threshold`, weaves more |

`flee_threshold(f6)` maps `f6=64 -> ~0.05 hull` (fanatical, fights to death) to
`f6=128 -> ~0.50 hull` (timid, bolts when hurt), a clean monotone ladder
(`ai_brain::flee_threshold_for`, `[I]` tuning -- `f6` has no traceable resident
consumer, s7.13.5, so the HP formula is ours; the *direction* is FAQ-confirmed).

## 5. Personality seed

Each pilot gets a stable `personality_seed` (hash of ship id) stamped on its
first combat tick -- the clean-room analog of PGG's `personalityseed`. It seeds
the jink geometry (which of 4 cardinal axes, the roll sign, weave phase) and the
`random` metric, so two same-faction pilots fly distinctly without diverging
from the per-faction CNST baseline. (Next step: let the seed also pick among
2-3 authored tables per faction/tier, unifying PGG file-selection with vanilla's
3-tier array.)

## 6. Build, tests, integration

- Sources: `src/ai_maneuver.h` (data model), `src/ai_brain.{h,cpp}` (registry +
  evaluator + maneuver handlers). `src/ship_ai.{h,cpp}` is now a thin top-level
  tick that delegates combat to `ai_brain`.
- Wired into both CMake targets; `ai_brain::load_all("assets/ai")` runs at
  startup (`main.cpp`) and in the headless harnesses.
- `test_full_loop` (the director/registry soak) stays green and now also
  validates the AI tables parse.
- `test_ai_brain` (new, headless) proves the evaluator selects the right
  maneuver per distance band (`6000 -> lead_pursuit`, `800 -> attack_run`,
  `300 -> break_off`) and drives the controller -- the parity proof.

## 7. Status / deferred

**Live:** the full data model, the two-channel evaluator, all 12 maneuvers, the
JSON table loader + `default.ai.json` (3 tiers), the f2/f3 firing upgrade, and
per-pilot personality seeding. Parity with the prior hand-coded behaviour is
proven by `test_ai_brain`.

**Deferred (next steps):**
- Per-faction / per-role table files (`merchant.ai.json`, `kilrathi.ai.json`,
  ...) -- the loader + faction resolution already support them; only `default`
  is authored today.
- Seed-selected table variants per tier (PGG file-selection + vanilla 3-tier
  array unified, s5).
- Missiles (PGG's per-atom probability + gun-delay) once the weapon system has
  them.
- Pinning the AI `dt` (`DS:0x2768`, `docs/ai_model.md` s9.3) so the 76/153/512
  tick timers convert to real seconds instead of the assumed ~10 Hz.

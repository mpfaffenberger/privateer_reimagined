# Gap Analysis — Privateer Reimagined vs. Wing Commander: Privateer (1993)

_Audit: 2026-09-28 against `origin/main` @ `4c9427bf` (issue [#490]).
Reference: original Privateer + Righteous Fire (`gamefaq`,
`docs/ai_model.md`, `re/` Ghidra work)._

**Method.** Every "Done" row below was checked against source or data in
this commit, not against older docs. All 24 CMake test targets were built
and run on macOS (Release). Where the earlier version of this document
(2026-07-19) was wrong, the correction is marked **(corrected)** so drift
is easy to spot next time.

---

## 1. Executive summary

The **sandbox and the story are both done**. Flight, combat, AI, trading,
outfitting, contracts, guilds, factions, docking and base screens, saves,
audio, the full 69-system Gemini sector, and the complete 23-mission
campaign all ship and are covered by headless tests.

The remaining gaps are:

1. **Build and test health.** The broad integration harness is red
   ([#314]) and there is no CI.
2. **Combat-system fidelity.** Per-system component damage, Friend-or-Foe
   missiles, scanner tiers, and buying turrets for hulls that lack them.
3. **Presentation polish.** HDR/bloom, normal maps, an Orion cockpit, and
   HUD pages that fit inside the cockpit MFDs.
4. **The living world** (Gemini Lives phases 2–6) and **Righteous Fire**.

Rough completion: **sandbox ~92%, story ~100% (base game), combat systems
~85%, base experience ~90%, living world ~15%, Righteous Fire ~3%.**

---

## 2. What's done (verified)

| Area | Status | Evidence |
|---|---|---|
| Universe: 69 systems in 4 quadrants, 170 jump links | Done | `assets/galaxy.json` (69 / 170), `assets/systems/*` (plus ~26 demo/atlas/test systems) |
| 59 landable bases + the Steltek derelict; per-archetype concourse art | Done | `assets/bases/*` (60 dirs incl. `derelict_base`), `assets/concourse/*` |
| Base screens: landing pad, concourse, bar, commodity exchange, ship dealer, equipment bay, contract boards, guilds, cargo hold | Done | `base_screens.*`, `commodity_ui.*`, `dealer_ui.cpp`, `equipment_ui.cpp`, `missions_ui.cpp` |
| Visual **hardpoint equipment bay**: click a hull schematic to fit guns into forward **and turret** hardpoints and to fit launchers | Done | `equipment_hardpoints.*`, `equipment_panels.cpp`, per-ship `equipment_hardpoints.json` |
| 18 ship definitions: 4 player hulls (Tarsus/Orion/Galaxy/Centurion) plus Talon, Demon, Gladius, Stiletto, Broadsword, Paradigm, Drayman, Dralthi, Gothri, Kamekh, Strakha, Drone, Scout, Derelict | Done | `assets/ships/*/ship.json` |
| Ship rendering: multi-view sprite atlases baked from 3D meshes; Demon/Orion/Kamekh/Gladius reskinned; 32-bit mesh indices | Done | `ship_sprite.*`, `assets/ships/*/sprites_3d`, `assets/meshes/`, [#484] |
| Guns: Laser, Mass Driver, Meson, Neutron, Particle, Tachyon, Ionic Pulse, Plasma, Steltek | Done | `GunType` in `gun.h` (9) |
| Missiles: DF / HS / IR / Torpedo, left/right launchers, launcher cycling (`W`) | Done | `MissileType` in `missile.h`, `launcher_modes.h`, `cockpit_armaments.*` |
| Turrets: auto-fire, lead-predicting, arc-gated, for NPCs and the player; guns can be bought into existing turret mounts | Done | `firing.cpp`, `GunMount::is_turret`, `equipment_panels.cpp` |
| Upgrades: shield ladder (capped per hull), engines, Plasteel/Tungsten armor, cargo expansion, jump drive, ECM 1–3, repair droid, advanced repair droid, tractor beam | Done | `outfitting.*`, `equipment_prices.json` |
| Ship dealer with trade-in | Done | `outfitting.cpp` `buy_hull` / `hull_trade_in` |
| Economy: reworked commodity exchange, per-archetype price/stock, contraband only at pirate bases, loot/salvage tables | Done | `economy.*`, `commodity*.{h,cpp}`, `assets/data/*.json` |
| Contracts: 6 mission types, 3 sources, shared contract boards, decoded original briefing grammar, tracker, auto-fail on dock for any incomplete mission | Done | `missions.*`, `mission_tracker.*`, `mission_templates.gen.h` |
| Factions: **9** (incl. **Steltek**, [#146]) stance matrix + per-faction player rep, kill attribution | Done **(corrected)** | `Faction` enum in `faction.h` |
| AI: decoded CNST skill vectors, personalities, maneuver system, per-ship AI tables, pursuit commitment | Done | `ai_brain.*`, `docs/ai_model.md`, `docs/ai_maneuver_system.md` |
| Encounters: dynamic spawner + data-driven scripted director (36 entries) | Done | `encounters.*`, `scripted_encounters.*` |
| Comms: hails, taunts, contraband scans, in-flight rumors, voiced reply menu | Done | `comm.*`, `hailing.*`, `comms_menu.*` |
| Navigation: hostile-gated autopilot, autodock, jumps (incl. escaping under fire), local nav map, **four-quadrant sector star chart** with canonical coordinates, unrevealed Fariss systems hidden | Done **(corrected)** | `autopilot.*`, `docking.*`, `jump.*`, `cockpit_hud.cpp` ([#339]) |
| Flight feel: critically damped fly-by-wire turn response, pilot head lean, rigid cockpit slide | Done | `turn_response.h`, `pilot_head_motion.h`, `test_turn_response` |
| Cockpits: painted overlays with live MFD instruments and lamps for **Tarsus, Galaxy, Centurion** (+ Talon for dev); world markers occluded by cockpit structure | Done **(corrected; the old doc said "deliberately not used")** | `cockpit_overlay*.{h,cpp}`, `assets/cockpits/`, `test_cockpit_overlay`, `test_cockpit_layers` |
| Weapons/ARMAMENTS MFD: live arm mode, mounts, energy, turret markers, interactive schematic | Done | `cockpit_mfd.cpp`, `cockpit_armaments.*`, `test_armament_loadout` |
| Saves: format **v8**, accumulating timestamped autosaves, `--continue` resumes newest, load-menu metadata, progress UI during rebuilds | Done **(corrected; was v6)** | `savegame.h` `k_format_version = 8` |
| Calendar: stardate `2669.135`, +1 day per landing, persisted, shown on base screens | Done, including dev_remote `/state` day/stardate and `POST /advance_day` ([#171]) | `world_clock.*`, `docking.cpp`, `test_world_clock` |
| Audio: SFX, dynamic music, per-scene bar music + DJ tool, extracted original speech, voice bank, per-line voice direction | Done | `audio/music/sfx/voice/*`, `music_dj.*` |
| Hazards, asteroids, loot/tractor, explosions, warp streaks, title scene, death | Done | respective modules |
| Dev infra: headless test seams, loopback `dev_remote` HTTP API (macOS), debug panel, cinematic studio, labelers, sprite/mesh/light editors, ~150 tools | Done | `dev_remote.*`, `tools/` |

### The campaign — **DONE** (epic [#136])

All 23 missions are playable end to end: Sandoval (M01), the Tayla smuggling
arc (M02–M05), Lynch/Miggs (M06–M09), Masterson and the Oxford escorts
(M10–M13), Lynn Murphy/Palan (M14–M16), Dr. Monkhouse (M17), the Cross
frontier surveys (M18–M21), Goodin (M22), and the Terrell/drone finale at
Blockade Point Tango (M23, `campaign_complete`).

- **Plot layer** (`plot.*`): string-keyed flags and plot items on
  `PlayerState`. With the campaign off (the default), nothing changes.
- **Bar and fixers** (`fixers.*`, 37 scene entries in `fixers.json`):
  auto-advancing portrait conversations with Grayson, original-actor voice
  clones, artifact prop shots, refusal epilogues, and per-scene music. The
  same framework runs the Oxford Library and Terrell's office.
- **In-flight cinematics** (`cinematic*`): M03 militia bust, M04 customs
  interception, M05 Riordian ambush, M06 Seelig, M22 Vera Crusader, the Troy
  tours, and dressed offer/debrief scenes for Murphy, Cross, Terrell, and
  Garrovick.
- **Steltek content:** plot-gated frontier jumps, Delta Prime derelict and
  gun pickup, the invulnerable cross-system drone, and weapon-whitelist
  damage gating so only the boosted Steltek gun can kill the drone.
- **Verification:** `test_campaign` walks M01→M23 headlessly (passing).

---

## 3. Gaps — ranked by importance

### P0 — Build and test health

_New tier since the 2026-07-19 audit._

| Gap | Impact | Issue |
|---|---|---|
| `test_full_loop` fails 17 checks (start state, director spawns, dock autosave slot 0, cargo run, respawn) because of stale assumptions after the autosave/economy changes | Main integration net is off | [#314] |
| **No CI.** The Windows Actions workflow was removed on 2026-07-11 after repeated failures. Windows previews are hand-published; Windows-specific bugs are open ([#291], [#239]). | Regressions show up late | — (needs an issue if CI is wanted) |

**Test scoreboard (2026-09-28, macOS Release):** 28/29 wired harnesses pass
on a fresh clone. `test_full_loop` fails.

**Fixed since the audit:**
- Fresh clones build the game and every harness without recovered data
  ([#427]).
- The post-build assets link follows the real build dir, so any `-B` works
  ([#497]).
- Save-touching harnesses (`test_savegame`, `test_missions`,
  `test_full_loop`) run in per-harness temp dirs via `tools/test_sandbox.h`,
  which aborts if isolation fails ([#383]). `test_docking` stubs `savegame`
  and never wrote real saves.
- Shadowed dev keys ([#491]): the gun mount tuner moved to `Shift+F4`, the
  unreachable F8/F9 demo-cinematic handlers were removed (use
  `POST /cinematic/play`), and one `player_trigger_held()` replaced a stale
  `X`/`Tab` trigger check in the gun-mode cycle.
- One radar range ([#492]): perception, target lock/drop, the `T` cycle, and
  the radar MFD scale all use the hull's `radar_range`. It is 15 km, the
  original game's sensor cull (`0x3a98`). April placeholder data had quietly
  set most hulls to 25–30 km, so NPCs spotted the player from farther away
  than they were meant to. `dev_remote`'s default port is now the 47001
  every script uses.
- Five orphaned harnesses are wired into CMake and green ([#493]):
  `test_autopilot`, `test_inventory112`, `test_missile`, `test_reputation`,
  `test_salvage`. Their failures were test rot (API drift, NPC-vs-player
  hull, starter-loadout change), not game bugs. `autopilot` and `reputation`
  gained real assertions. `test_light_rec` was a CLI probe, not a test, and
  is now `tools/light_rec_probe.cpp`.
- Quick fixes (2026-09-29): stale stub comments ([#149]); cinematic and
  trigger writes are atomic via `tools/cinematics/publish.py` ([#368]);
  mesh showroom regenerated with Stiletto ([#474]); dev_remote exposes the
  day ([#171]); [#323] closed as a duplicate of [#322].

### P1 — Combat-system gaps

1. **Per-system component damage + Damage Control MFD** ([#141]). The `R`
   page still draws `"system damage not yet modeled"`
   (`cockpit_hud.cpp:661`). The original damaged guns, engines, radar, and
   the jump drive individually and charged per-system repairs. `repair.*`
   only covers hull and armor.
2. **Friend-or-Foe missiles** ([#144]). `MissileType` is still DF/HS/IR/
   Torpedo. The source data already lists FF on several loadouts
   (`privateer_ship_data.json`), so they're referenced but can't be fired.
3. **Scanner/radar tiers** ([#143]). There are no scanner products. The
   foundation is ready: every ship, player included, now reads one per-hull
   `ShipClass::radar_range` through `perception::radar_range_m()` ([#492]),
   so a tier only has to change that value.
4. **Turrets.** Firing is done, and guns can be bought into *existing*
   turret hardpoints **(corrected)**. Still missing: adding turrets to hulls
   that don't have them ([#145]), and keeping turrets auto-armed across gun
   groups ([#379]). A manual turret view is probably a deliberate non-goal
   (auto-fire plays better).

### P2 — Presentation and art

- **Cockpits:** the Orion has no cockpit overlay, so it falls back to the
  HUD-only view. The ticket to make "every flyable ship" have a cockpit is
  [#168]. Dense HUD pages (TARGET faction line, STATUS Weapons) clip inside
  the cockpit MFD holes ([#430]). The art-direction question ([#153]) is
  settled: commit to per-hull cockpits.
- **Rendering:** HDR scene with highlight roll-off and dithering ([#488],
  branch in progress), a bloom kernel that's 4× taller than wide ([#489]), 3DS
  bump maps fed in as tangent-space normals ([#473]), and the pixel sky-props PR ([#468]/[#425]). Atlas regeneration for the
  reskinned hulls is done ([#482]).
- **Art passes:** equipment dealer room ([#322]), vanilla upgrade-screen
  art ([#230]), refinery landing composites ([#293]), AI art for base
  backgrounds/animated concourses ([#165]), character art for fixers, comms
  pilots, and Grayson ([#166]), and skybox depth ([#167]).

### P3 — Base and UX gaps

- **Bar rumors / bartender talk** ([#164]). The bar and fixers exist, but
  rumors still only come over in-flight comms (`rumor_lines.json` →
  `comms_menu`).
- **Jump through a nearby gate without selecting its nav** ([#380]).
- **Contraband-scan behavior during the M03/M04 smuggling legs** needs a
  design call ([#319]).
- **dev_remote phase 2 write endpoints** for full agentic campaign driving
  ([#154]).

### P4 — Living world ("Gemini Lives", epic [#170])

| Phase | Status | Evidence / issue |
|---|---|---|
| 1. Calendar + save v8 + dock hook + stardate UI | **Done**, including the dev_remote `/state` day field and `/advance_day` test hook | `world_clock.*`, [#171] |
| 2. Character bible expansion | Partial: 24 characters in `characters.json`; new cast pending | [#175]–[#180] |
| 3. NPC schedules + `npc_director` + schedule lint | Not started (no `npc_schedules.json`, no director module) | [#172], [#173] |
| 4. Trigger extensions (`npc_present`, day gates) + Tier-1 barks | Not started | [#174] |
| 5. Tier-2 Grayson cinematics | Not started | [#182] |
| 6. The Defector arc | Not started | [#181] |

Also here: Kilrathi voices are broken ([#179]), and the older Phase-2 Confed
distress scenario tickets ([#65]–[#68]) are still open.

### P5 — Faction and world fidelity

- **Asymmetric stances** are still an acknowledged TODO (`faction.h`:
  "Stance is symmetric for v1").
- **Guild membership fees and pay bands** haven't been checked against
  vanilla ([#147]).
- ~~No Drone/Steltek faction~~ **Done (corrected)**: `Faction::Steltek`
  ([#146]).
- ~~Hidden systems / exploration~~ **Done (corrected)**: frontier jumps are
  plot-gated (`JUMP: UNSURVEYED` until `monkhouse_done`) and unrevealed
  Fariss systems are hidden on the sector chart ([#339]).

### P6 — Righteous Fire ([#148])

Only the advanced repair droid exists. Missing: the RF campaign (Mordecai
Jones cult arc) and RF-only gear (isometal armor, speed/thrust enhancers,
shield levels beyond the base-game ladder, gun cooler). The **secret
compartment already exists** as a campaign plot item (M04), so it isn't an
RF gap **(corrected)**. Fine to defer.

### P7 — Engineering hygiene

- **`src/main.cpp` is 7,661 lines / 400 KB and still growing** ([#151] was
  filed at 337 KB). **17 other files are over the 600-line guideline**,
  led by `cockpit_hud.cpp` (2,482), `dev_remote.cpp` (2,216),
  `base_screens.cpp` (1,451), `missions.cpp` (1,304), and
  `scripted_encounters.cpp` (1,165).
- **Comment debt is low:** 8 TODO/FIXME/HACK markers across `src/`, and the
  known stale "stub" comments are fixed ([#149]).
- **Repo hygiene: done** ([#150] closed). The tracked root is clean. Local
  build products and logs stay untracked.
- **Doc drift:** this audit replaced a gap doc that had drifted for about 60
  PRs. Keep volatile status here, not in the README. Update both in the same
  pull request whenever bindings, CLI flags, save format, or test targets
  change.

### Issue-tracker housekeeping (2026-09-28)

Closed after checking the code: [#142] (Weapons MFD), [#150] (repo cleanup),
[#153] (cockpit decision: commit), [#482] (reskin atlases regenerated). Left
open because they're only partly done: [#171] (dev hooks missing; since fixed), [#65]–[#68]
(one distress scenario exists, not the 10–15 variants or the inspection
scene), and [#166] (fixer portraits exist; generic pilots and Grayson don't).
Art-quality tickets [#293] and [#322] have related commits but need a human
judgment call ([#323] was a duplicate of [#322]).

---

## 4. Suggested attack order

1. **Get `test_full_loop` green ([#314])** and decide whether to bring CI
   back (at least a macOS or Linux headless-test job).
2. **Component damage + Damage Control MFD ([#141]).** It's the biggest
   remaining gameplay-fidelity gap, and the repair economy hooks are ready.
3. **Scanner tiers ([#143]) → FF missiles ([#144])**, then purchasable turrets ([#145]/[#379]). All small and
   data-driven.
4. **Presentation:** HDR/bloom ([#488]/[#489]), normal maps ([#473]), Orion
   cockpit + MFD clipping ([#168]/[#430]).
5. **Gemini Lives phases 3–4** (schedules, director, barks: [#172]–[#174]),
   then the cast and cinematics.
6. **Keep splitting `main.cpp` ([#151])**, starting with the input handler,
   now that its dead handlers are gone.
7. Righteous Fire, someday ([#148]).

[#65]: https://github.com/mpfaffenberger/privateer_reimagined/issues/65
[#68]: https://github.com/mpfaffenberger/privateer_reimagined/issues/68
[#136]: https://github.com/mpfaffenberger/privateer_reimagined/issues/136
[#141]: https://github.com/mpfaffenberger/privateer_reimagined/issues/141
[#142]: https://github.com/mpfaffenberger/privateer_reimagined/issues/142
[#143]: https://github.com/mpfaffenberger/privateer_reimagined/issues/143
[#144]: https://github.com/mpfaffenberger/privateer_reimagined/issues/144
[#145]: https://github.com/mpfaffenberger/privateer_reimagined/issues/145
[#146]: https://github.com/mpfaffenberger/privateer_reimagined/issues/146
[#147]: https://github.com/mpfaffenberger/privateer_reimagined/issues/147
[#148]: https://github.com/mpfaffenberger/privateer_reimagined/issues/148
[#149]: https://github.com/mpfaffenberger/privateer_reimagined/issues/149
[#150]: https://github.com/mpfaffenberger/privateer_reimagined/issues/150
[#151]: https://github.com/mpfaffenberger/privateer_reimagined/issues/151
[#153]: https://github.com/mpfaffenberger/privateer_reimagined/issues/153
[#154]: https://github.com/mpfaffenberger/privateer_reimagined/issues/154
[#164]: https://github.com/mpfaffenberger/privateer_reimagined/issues/164
[#165]: https://github.com/mpfaffenberger/privateer_reimagined/issues/165
[#166]: https://github.com/mpfaffenberger/privateer_reimagined/issues/166
[#167]: https://github.com/mpfaffenberger/privateer_reimagined/issues/167
[#168]: https://github.com/mpfaffenberger/privateer_reimagined/issues/168
[#170]: https://github.com/mpfaffenberger/privateer_reimagined/issues/170
[#171]: https://github.com/mpfaffenberger/privateer_reimagined/issues/171
[#172]: https://github.com/mpfaffenberger/privateer_reimagined/issues/172
[#173]: https://github.com/mpfaffenberger/privateer_reimagined/issues/173
[#174]: https://github.com/mpfaffenberger/privateer_reimagined/issues/174
[#175]: https://github.com/mpfaffenberger/privateer_reimagined/issues/175
[#179]: https://github.com/mpfaffenberger/privateer_reimagined/issues/179
[#180]: https://github.com/mpfaffenberger/privateer_reimagined/issues/180
[#181]: https://github.com/mpfaffenberger/privateer_reimagined/issues/181
[#182]: https://github.com/mpfaffenberger/privateer_reimagined/issues/182
[#230]: https://github.com/mpfaffenberger/privateer_reimagined/issues/230
[#239]: https://github.com/mpfaffenberger/privateer_reimagined/issues/239
[#291]: https://github.com/mpfaffenberger/privateer_reimagined/issues/291
[#293]: https://github.com/mpfaffenberger/privateer_reimagined/issues/293
[#314]: https://github.com/mpfaffenberger/privateer_reimagined/issues/314
[#319]: https://github.com/mpfaffenberger/privateer_reimagined/issues/319
[#322]: https://github.com/mpfaffenberger/privateer_reimagined/issues/322
[#323]: https://github.com/mpfaffenberger/privateer_reimagined/issues/323
[#339]: https://github.com/mpfaffenberger/privateer_reimagined/issues/339
[#368]: https://github.com/mpfaffenberger/privateer_reimagined/issues/368
[#379]: https://github.com/mpfaffenberger/privateer_reimagined/issues/379
[#380]: https://github.com/mpfaffenberger/privateer_reimagined/issues/380
[#383]: https://github.com/mpfaffenberger/privateer_reimagined/issues/383
[#425]: https://github.com/mpfaffenberger/privateer_reimagined/issues/425
[#427]: https://github.com/mpfaffenberger/privateer_reimagined/issues/427
[#430]: https://github.com/mpfaffenberger/privateer_reimagined/issues/430
[#468]: https://github.com/mpfaffenberger/privateer_reimagined/issues/468
[#473]: https://github.com/mpfaffenberger/privateer_reimagined/issues/473
[#474]: https://github.com/mpfaffenberger/privateer_reimagined/issues/474
[#482]: https://github.com/mpfaffenberger/privateer_reimagined/issues/482
[#484]: https://github.com/mpfaffenberger/privateer_reimagined/issues/484
[#488]: https://github.com/mpfaffenberger/privateer_reimagined/issues/488
[#489]: https://github.com/mpfaffenberger/privateer_reimagined/issues/489
[#490]: https://github.com/mpfaffenberger/privateer_reimagined/issues/490
[#491]: https://github.com/mpfaffenberger/privateer_reimagined/issues/491
[#492]: https://github.com/mpfaffenberger/privateer_reimagined/issues/492
[#493]: https://github.com/mpfaffenberger/privateer_reimagined/issues/493
[#497]: https://github.com/mpfaffenberger/privateer_reimagined/issues/497

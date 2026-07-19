# Gap Analysis — new_privateer vs. Wing Commander: Privateer (1993)

_Audit date: repo state as inspected. Reference: original Privateer +
Righteous Fire (gamefaq in repo root, docs/ai_model.md, re/ Ghidra work)._

---

## 1. Executive summary

The sandbox layer is **substantially complete**: flight, combat, AI,
trading, outfitting, generated missions, guilds, factions/reputation,
docking/base screens, saves, audio/music/voice, and a full 69-system
Gemini sector across all four quadrants. What's **missing is the game's
spine**: the scripted story campaign (Sandoval → Tayla → Lynch →
Monkhouse → Palan → Steltek drone), fixers in bars, per-system
component damage, scanner/radar tiers, and Friend-or-Foe missiles.
Righteous Fire exists only as two stray equipment entries.

Rough completion: **sandbox ~90%, story ~0%, combat systems ~85%,
base experience ~85%**.

---

## 2. What's already done (verified in source/assets)

| Area | Status | Evidence |
|---|---|---|
| Universe: 69 systems, 4 quadrants (Humboldt 13 / Fariss 22 / Potter 18 / Clarke 16), 170 jump links | Done | `assets/galaxy.json`, `assets/systems/*` |
| 59 bases with per-base concourse art, 7 concourse archetypes (agri/mining/refinery/pleasure/pirate/military/newcon) | Done | `assets/bases/*`, `assets/concourse/*` |
| Base screens: Landing Pad, Concourse, Commodity Exchange, Ship Dealer, Equipment, Mission Computer, Merc + Merchant Guilds, Cargo Hold | Done | `base_screens.{h,cpp}` + registered hooks |
| 18 ship classes incl. all 4 player hulls (Tarsus/Orion/Galaxy/Centurion), Talon, Demon, Gladius, Stiletto, Broadsword, Paradigm, Drayman, Dralthi, Gothri, Kamekh, Strakha, Drone | Done | `assets/ships/*`, `ship_class.*` |
| Guns: Laser, Mass Driver, Meson, Neutron, Particle, Ionic Pulse, Tachyon, Plasma, Steltek | Done | `gun.cpp` |
| Missiles: DF / HS / IR / Torpedo + left/right launchers, torpedo launcher, ammo economy | Done | `missile.h`, `repair.cpp`, `outfitting.cpp` |
| Turrets: auto-firing, lead-predicting, arc-gated, energy-free; work for NPCs AND the player (rear-turret basis fix in place) | Done | `firing.cpp:171-247`, `GunMount::is_turret` |
| Upgrades: shield levels, engine levels, armor packages, cargo expansion, jump drive, ECM 1–3, repair droid, adv. repair droid (RF), tractor beam | Done | `outfitting.*`, `equipment_prices.json` |
| Ship dealer with trade-in (55%) | Done | `outfitting.cpp buy_hull/hull_trade_in` |
| Economy: commodity catalog, per-archetype price/stock, contraband list, loot/salvage tables | Done | `economy.*`, `assets/data/*.json` |
| Missions: 6 vanilla types (Patrol/Scout/Attack/Defend/Bounty/Cargo), 3 sources (Computer/MercGuild/MerchGuild), decoded original mission-text grammar ($EN/$DB/$DS…), tracker, fail-on-land | Done | `missions.*`, `mission_templates.gen.h`, `mission_tracker.*` |
| Factions: 8-faction stance matrix + per-faction player rep (-100..100) + baselines, kill attribution, rep deltas | Done | `faction.*`, `comm.*` |
| AI: decoded CNST skill vectors from PRCD.EXE (f0/f1/f2/f3/f6), personalities, maneuver system, per-ship AI tables, capital-ship spacing | Done | `ai_brain.*`, `docs/ai_model.md`, `re/` Ghidra project |
| Encounters: dynamic spawner + data-driven scripted encounter director (triggers/dialogue/spawn/reward) | Done | `encounters.*`, `scripted_encounters.*` |
| Comms: hails, taunts, contraband branch, rumors-over-comms, player comm menu with voice | Done | `comm.*`, `hailing.*`, `comms_menu.*` |
| Autopilot (hostile-gated), autodock, jump mechanic, nav map overlay, cross-system route (BFS/Dijkstra) | Done | `autopilot.*`, `docking.*`, `jump.*`, `missions::hops_between` |
| Saves: versioned (v6), autosave-on-dock, timestamped accumulation, load menu metadata | Done | `savegame.*` |
| Audio: SFX, dynamic music, extracted original speech w/ labeling + voice-bank pipeline | Done | `audio/music/sfx/voice/*`, `tools/` |
| Hazards, asteroids, loot/tractor, explosions, warp streaks, title scene, death (Dying mode) | Done | respective modules |
| Dev infra: headless test seams (MISSIONS/ECONOMY/COMM_HEADLESS), dev remote, debug panel, sprite/light editors, huge asset pipeline in `tools/` | Done | — |

That is a *lot* of remake. Now, the gaps.

---

## 3. Gaps — ranked by importance

### P0 — The story campaign — **DONE** (epic #136)

The full 23-mission campaign is implemented and playable end to end:
Sandoval (M01), the Tayla smuggling arc (M02-M05), Roman Lynch/Miggs
(M06-M09), Masterson + the Oxford escorts (M10-M13), Lynn Murphy/Palan
(M14-M16), Dr. Monkhouse (M17), the Cross frontier surveys (M18-M21),
Goodin (M22), and the Terrell/drone finale at Blockade Point Tango
(M23, `campaign_complete`).

What was built (issues #113-#135, infra #137-#140/#146):

- **Plot-flag layer** (`plot.*`, #138): string-keyed flags + plot items
  on `PlayerState`, savegame v7 (v6 saves migrate: plot lists default
  empty = campaign off). Debug panel + `POST /plot` for get/set.
- **Bar screen + fixer framework** (`fixers.*`, #137): data-driven
  registry (`assets/data/fixers.json`) with fixed-base AND
  archetype-predicate placement (Goodin: any mining base except
  Rygannon/Perry), plot-flag gating, portrait conversation UI with
  accept/refuse, reused for the Oxford library and Terrell's office.
- **Scripted-encounter extensions** (#139): `in_system` / `at_nav` /
  `on_launch` triggers, multi-wave kill-alls, named NPCs with
  kill-memory (`killed:<id>`), conditional re-ambush, talk-then-attack,
  wingman + prop spawns — all in `assets/data/scripted_encounters.json`.
- **Escort missions** (`escort.*`, #140), the Palan blockade
  docking-refusal gate, mission-scoped stance overrides (Tayla's pirate
  neutrality), the secret compartment (scan-exempt contraband hold).
- **Steltek content is now gameplay** (#130-#135): plot-gated frontier
  jump links (`JUMP: UNSURVEYED` until `monkhouse_done`), fleshed-out
  delta/beta/gamma/delta_prime systems, the Delta Prime derelict + gun
  pickup, the cross-system invulnerable drone pursuer (`drone.*`), the
  Steltek boost event, and weapon-whitelist damage gating
  (`Ship::immune_bypass_gun`) so ONLY the boosted gun kills the drone.
  The gun is unbuyable/unsellable (no shop price row).
- **Verification**: headless `test_campaign` walks M01→M23 against the
  shipped data; `test_savegame` proves the v6→v7 migration; live smoke
  runs drive every phase over the dev_remote HTTP API (`/fixer`,
  `/plot`, `/damage`, `/ships`, `/events`).

The sandbox remains untouched for players who never talk to Sandoval
— campaign-off is the default and stays invisible.

### P1 — Combat-system gaps

1. **Turret gaps (core is DONE — auto-fire works)** — remaining edges:
   turrets aren't purchasable in outfitting (no turret rows in
   `outfitting.cpp` / `equipment_prices.json`; they exist only as
   `is_turret` mounts in ship.json loadouts), and there's no manual
   turret view (original let you man the Galaxy/Centurion turret
   yourself — arguably auto-fire is the better design, so this may be
   an intentional non-goal). The stale turret comments in `ship_class.h`
   and `gun.h` have been corrected to match `firing.cpp`.
2. **Friend-or-Foe missiles** — missing. `MissileType` = DF/HS/IR/
   Torpedo only. Original had FF as the pirate favorite.
3. **Per-system component damage** — `cockpit_hud.cpp:683`:
   `"DAMAGE CONTROL — system damage not yet modeled"`. Original damaged
   individual systems (guns, engines, radar, jump drive) and made you
   pay per-system repair. `repair.*` covers hull/armor only.
4. **Weapons MFD — DONE.** The STATUS/Weapons page now reads the live ship
   and shows arm mode, armed mount count, energy, fitted gun names, turret
   markers, and per-mount armed state.
5. **Scanner/radar tiers** — no scanner products at all (grep confirms:
   only a coincidental "Skybird Scanners" company name). Original had
   Iris/Hunter/B&S lines with color-coded IFF, ITTS, and lock quality.
   Radar is currently a flat 15 km sphere for everyone.

### P2 — Base/UX gaps

- **Rumors-in-bar** — the Bar screen now exists (fixers, #137) but
  rumor delivery still happens in-flight over comms; porting the rumor
  tables into bartender small-talk is an open nicety.
- **Cockpit art**: deliberately not used — the Tarsus frame exists in
  `assets/cockpits/` but was judged not good enough and is unwired.
  Not a gap so much as an open art-direction question: either commit
  to the clean HUD-only look (fine, Freelancer did it) or budget for
  four per-hull cockpit frames that don't suck. Recording the decision
  here so nobody "helpfully" re-wires the bad one.
- **Quine 4000 personal computer** fiction — mission/manifest/status
  exist as screens, but the original's unified in-flight MFD computer
  (cargo manifest view, quadrant map paging) is partial. Verify the
  navmap covers cross-quadrant browsing.

### P3 — Faction/world fidelity

- **No Drone/Steltek faction** — enum caps at 8; the drone can't have
  its canonical everyone-hostile behavior without borrowing Kilrathi.
- **Asymmetric stances** — acknowledged TODO in `faction.h` (Retro
  hatred was asymmetric in the original).
- **Hidden systems / exploration** — original had unexplored Rygannon
  frontier + Delta Prime gating via plot. All 170 jump links are
  presumably visible/usable from day one.
- **Merchant/Merc guild membership** exists (save v6 bools) — verify
  join fees + member-only pay bands match original (they appear
  implemented via `MissionSource` pay bands; spot-check values).

### P4 — Righteous Fire

Only `adv_repair_droid` and the Steltek gun row exist. Missing: RF
campaign (Mordecai Jones cult arc), RF-only gear (isometal armor, speed
enhancer, thrust enhancer, shield levels 6–7, gun cooler, secret
compartment for contraband). Fine to defer — base game first.

### P5 — Platform & engineering hygiene (not game features, but real gaps)

- **Platform confidence is uneven.** macOS/Metal is the primary development
  path and Windows/D3D11 has a build + rolling-nightly CI workflow. The
  Linux OpenGL/X11 path remains comparatively untested.
- **`src/main.cpp` is roughly 390 KB / 7,500 lines.** The repo's own style
  rules (and mine — woof) say split it by cohesive responsibility: input,
  sim loop, lock-state machine, HUD glue.
- **Repo hygiene — DONE for source control.** Root build products, compile
  databases, binaries, logs, and the accidental literal `~/` directory are
  untracked and covered by `.gitignore`. Local copies may remain for developer
  convenience without polluting commits.
- **README refresh — DONE.** It now documents the actual game, architecture,
  build/test entry points, and current roadmap instead of the old skybox demo.
- **Stale-comment scrub — initial pass DONE.** Turret, threat/autopilot, and
  base-screen integration comments now match the live implementations; keep
  treating comments as code when behavior changes.

---

## 4. Suggested attack order

1. **Bar + fixers screen** (art & speech already extracted; unlocks
   everything narrative).
2. **Plot-flag layer + first campaign chapter (Sandoval)** on top of
   scripted_encounters — proves the pipeline end-to-end.
3. **Component damage + Damage/Weapons MFDs** (repair economy hooks
   already exist).
4. **Scanners/FF missiles + purchasable turrets** (small, data-driven,
   big fidelity win).
5. **Steltek arc wiring** (derelict base + drone into real systems,
   hidden-jump gating, Steltek gun as pickup not purchase).
6. Hygiene pass: continue splitting main.cpp. Ignore coverage, README rewrite,
   and the first stale-comment pass are complete.
7. Righteous Fire, someday.

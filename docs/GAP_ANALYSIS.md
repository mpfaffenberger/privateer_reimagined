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

### P0 — The story campaign (the biggest gap by far)

The original's spine is entirely absent. No trace of: Sandoval,
Tayla, Roman Lynch/Miggs, Dr. Monkhouse, Lynn Murphy/Palan, Masterson,
Admiral Terrell, Goodin, the Steltek artifact/drone arc.

- No campaign/plot-state machine (chapter progression, fixer unlock
  gating, plot flags in `PlayerState` or `savegame` — verified absent).
- **Bar & fixers are a stub**: `base_screens.cpp:505` draws
  `"BAR - fixers TBD"`; no `register_screen(BaseScreen::Bar, …)` exists.
  Ironically the raw material is ready: bar backgrounds + bartender
  sprites are extracted (`assets/concourse/*/bar_*.png`), bar/fixer
  speech is extracted and labeled (`assets/speech/bar/`,
  `docs/bar_speech_vpk_format.md`, `speech_labeler.cpp`).
- Steltek content exists as parts, not as gameplay: the Steltek gun is
  in the gun table, `assets/ships/drone` and
  `assets/bases/derelict_base` exist — but the drone/derelict only
  appear in dev showroom systems (`mesh_showroom.json`,
  `sprite_showroom.json`). `delta_prime.json` is an 831-byte skeleton.
  No hidden-system discovery, no drone hunt, no gun pickup event.
- The scripted-encounter director (`scripted_encounters.*`) is the
  natural substrate for campaign missions but currently only drives
  ambient scenarios.

**Recommendation:** build a small plot-flag layer on `PlayerState`
(saved), a fixer NPC hook in the Bar screen, and author the campaign as
data on top of the existing scripted-encounter + mission systems. Most
of the machinery already exists.

### P1 — Combat-system gaps

1. **Turret gaps (core is DONE — auto-fire works)** — remaining edges:
   turrets aren't purchasable in outfitting (no turret rows in
   `outfitting.cpp` / `equipment_prices.json`; they exist only as
   `is_turret` mounts in ship.json loadouts), and there's no manual
   turret view (original let you man the Galaxy/Centurion turret
   yourself — arguably auto-fire is the better design, so this may be
   an intentional non-goal). Also: `ship_class.h` still says turrets
   are "out of scope / we ignore them" and `gun.h:19` says "NPC-only"
   — both comments are stale and contradict `firing.cpp`.
2. **Friend-or-Foe missiles** — missing. `MissileType` = DF/HS/IR/
   Torpedo only. Original had FF as the pirate favorite.
3. **Per-system component damage** — `cockpit_hud.cpp:683`:
   `"DAMAGE CONTROL — system damage not yet modeled"`. Original damaged
   individual systems (guns, engines, radar, jump drive) and made you
   pay per-system repair. `repair.*` covers hull/armor only.
4. **Weapons MFD** — `"WEAPONS — loadout screen TBD"`
   (`cockpit_hud.cpp:686`).
5. **Scanner/radar tiers** — no scanner products at all (grep confirms:
   only a coincidental "Skybird Scanners" company name). Original had
   Iris/Hunter/B&S lines with color-coded IFF, ITTS, and lock quality.
   Radar is currently a flat 15 km sphere for everyone.

### P2 — Base/UX gaps

- **Bar screen** (see P0) — also blocks rumors-in-bar (rumor data files
  exist and are used in-flight over comms instead).
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

- **macOS/Metal only in practice.** `dev_remote_stub.cpp` hints at
  Linux intent; sokol supports GL/D3D — untested/unwired.
- **`src/main.cpp` is 337 KB.** The repo's own style rules (and mine —
  woof) say split it: input, sim loop, lock-state machine, HUD glue.
- **Repo hygiene:** `tracelog.log` (11 MB!), `tl.log`, `CMakeCache.txt`,
  `Makefile`, `CMakeFiles/`, `compile_commands.json`, a literal `~/`
  directory, and a 2.2 MB `new_privateer` binary are committed at the
  root. `.gitignore` them and purge.
- **README.md is a fossil** — describes a 1,700-line skybox demo;
  undersells the project by roughly two orders of magnitude. The
  roadmap checkboxes claim missions/trade/factions are still open;
  they're done.
- **Stale "stub" comments** — `threat.h`/`autopilot`/`jump` comments
  still say the hostile gate "is a stub today"; verify and refresh
  (`threat.cpp` looks live now). Same for `base_screens.h` header
  claiming shops are placeholders.

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
6. Hygiene pass: split main.cpp, scrub logs/artifacts, rewrite README,
   fix stale comments (turrets "NPC-only", threat "stub", ship_class
   "turrets out of scope").
7. Righteous Fire, someday.

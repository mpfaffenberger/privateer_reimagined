# Plan: Living Voices, Reactive Encounters, Inventory & Salvage

Status: **design / approved — being broken into issues.** This is the source of
truth for the work; each GitHub issue references a section here.

This plan integrates the cloned-cast voice bank (see
`.agents/commands/generate-voice-lines.md`) into the real-time flight loop, adds
reactive + scripted encounters, a rumor/lead objective system, and the
inventory/salvage/rarity systems those rewards need.

## Approved decisions (locked)

1. **Unified hold** — `InventoryItem`s and commodity stacks share one
   `cargo_capacity`; each discrete item = 1 space.
2. **Contraband = local aggro only** — getting caught flips the searching NPC +
   nearby same-faction to hostile for that encounter; **no** galaxy-wide rep hit.
3. **Female Confed voice = repurpose an existing female clip** (`V00`/`V06` or
   `mercgirl`) mapped as `confed_f` — no new cloning.
4. **Build order: Phase 0 (voice plumbing) first.**
5. **Gun mounts become a struct** `{gun_id, rarity, mods}` — rarity is a
   first-class property from drop → inventory → mount → save.
6. **New `BaseScreen::CargoHold` sub-screen** for viewing/selling inventory;
   Equipment fits/installs; Commodity Exchange sells salvage-as-commodities.

## Design principles

- **Ride existing systems, don't fork them.** Voice rides the `comm` event
  path; scripted spawns ride `encounters::SpawnFn`; rewards ride `player` cargo
  + a new inventory peer; objectives extend `mission_tracker`; base UI rides the
  `base_screens::register_screen` seam.
- **Data-driven.** Lines, contraband list, scenarios, loot tables, rarity mods,
  prices all live in `assets/data/*.json`.
- **Headless-testable transactions.** New mutation modules mirror
  `economy`/`outfitting`: pure logic behind a headless guard, UI on top, mutate
  only via `player::` helpers so `savegame` round-trips everything.
- **Phased + independently shippable.** Ordering front-loads high-impact /
  low-risk work.

## What already exists (anchors)

| System | File | Note |
|---|---|---|
| Comm line table + HUD feed | `comm.{h,cpp}` | `assets/data/comm_lines.json`, `pick_line(faction,event)` — **text only** |
| NPC engage bark hook | `ai_brain.cpp:557` | fires when a hostile is in `comms_f1` range, rate-limited by `last_bark_at` |
| Encounter director | `encounters.{h,cpp}` | `populate_on_entry`, `MissionForce`, `SpawnRequest{faction,AIState,civ_role}` |
| Factions / stance / rep | `faction.{h,cpp}` | 8 factions, runtime-mutable stance matrix, `stance_npc_vs_player`, `apply_player_kill` |
| Audio mixer | `audio.{h,cpp}` | 24-voice 3D mixer, `play()`/`play_world()` — **PCM16 WAV only, MP3 rejected** |
| Player state | `player.{h,cpp}` | credits, rep, equipment flags, `cargo` (commodity stacks), missions, `faction_kills` |
| Commodities | `commodity.{h,cpp}` | 50 goods incl. SLAVES/MAGIC — **no contraband flag yet** |
| Nav points | `system_def.h NavPointDef`, `system.nav_points` | static per-system; autopilot/jump/navmap consume them |
| Base screens | `base_screens.{h,cpp}` | concourse hub + `register_screen(BaseScreen, hook)` seam; `BaseContext{player, player_ship}` |
| Shops | `outfitting.{h,cpp}`, `economy.cpp` | headless transactions, `buy/sell_gun`, mutate via `player::` helpers |
| Tractor / equipment | `outfitting.cpp` (~L949 tick, L191 shop) | tractor is a buyable flag today |

---

## Phase 0 — Voice playback plumbing (foundation)

Make the comm lines we already author *audible* in the cloned voices.

- **0.1 Voice-bank build tool.** `tools/build_voice_bank.py`: ffmpeg-convert
  `assets/speech/generated/audio/*.mp3` (+ `scenes/*.mp3`) to **PCM16 mono WAV**
  under `assets/audio/voice/`, and emit `assets/data/voice_bank.json` mapping
  `voice_id -> {faction, category, [wav paths]}` derived from `comms.json`. Map
  a female clip as `confed_f`.
- **0.2 `voice.{h,cpp}` module.** `load()` + `say(faction|voice_id, category,
  world_pos, bool to_player)` over the `audio` mixer; 2D for player-directed
  hails (radio feel), `play_world` for ambient NPC-to-NPC; **one-voice-at-a-time**
  for player-directed lines (queue/duck).
- **0.3 Wire into comm.** In `comm::npc_engage_bark` + `comm::report_player_kill`,
  when a feed line is pushed *for the player*, also call `voice::say(...)`; the
  HUD feed text is the subtitle. Add a debug-panel "force bark" button.

---

## Phase 1 — Reactive contraband search (Militia / Confed)

- **1.1 Contraband data + helpers.** `assets/data/contraband.json` (ids +
  severity: `brilliance`, `slaves`, `ultimate`, …); `commodity::is_contraband`,
  `player::carrying_contraband`.
- **1.2 Per-ship aggro override.** A `bool aggro_player` (or target id) on the
  NPC the AI reads as "treat player as hostile" WITHOUT a global rep flip.
  Reused by Phase 2.
- **1.3 `hailing.{h,cpp}` search director.** Per-NPC interaction state
  (`Idle/Hailing/Searching/Resolved`). When the player is in comms range of a
  friendly/neutral **Militia or Confed**, once per encounter + cooldown, roll
  **35%** → search hail (voice). Clean → "you're clear". Contraband → set
  `aggro_player` on that NPC + same-faction wingmates in range. Fleeing → guilty
  → same hostile path.

---

## Phase 2 — Scripted encounter scenarios (data-driven)

- **2.1 Scenario engine.** `assets/data/scripted_encounters.json` +
  `scripted_encounters.{h,cpp}`: conditions checked against `perception`
  contacts (near classes/faction, count, rep gate, chance, cooldown,
  once-per-system); a dialogue script (voice line refs); `spawn_on_accept`
  (via `encounters::SpawnFn` / MissionForce-style); a `reward` block.
- **2.2 Confed distress set (10–15 variants).** Lone Stiletto/Broadsword hails
  *"tailed by Dralthi, assist?"*, **ignores player rep** (redemption path);
  accept spawns a Dralthi wing; kills pay credits + Confed rep. Uses male +
  female confed voices (`confed_f` repurposed). Author lines via `gen_comms.py`
  → `synth_comms.py`.
- **2.3 Rare ambient inspection scene.** Spawn merchant+militia near player, play
  the authored `conv_00` exchange; player may intervene for rep/loot.

---

## Phase 3 — Rumors & dynamic objectives

- **3.1 "Ask for rumors" verb.** Target a friendly ship in range → comm verb;
  plays a bar-patron/merchant rumor voice (we have 404 patron lines).
  Per-ship cooldown.
- **3.2 Lead roll + model.** **2–5%** → a `Lead` (rough location of something
  valuable, possibly an adjacent system via `galaxy` adjacency). Store active
  leads in `PlayerState` (save/load).
- **3.3 Dynamic objective + nav point.** `objectives.{h,cpp}` (or extend
  `mission_tracker`): push a **transient `NavPointDef`** (flagged dynamic, never
  serialized into system JSON), positioned between navs or **>500 km from the
  sun**; render on the nav map.
- **3.4 Expiry + payoff.** Landing on any base **expires all leads**; reaching a
  lead spawns the loot entity (cargo / weapon / scrap / rare persistent
  upgrade) the tractor pulls in.

---

## Phase 4 — Inventory, salvage, loot rarity (structural)

- **4a. Universal tractor.** Remove `tractor_beam` from the shop; treat
  `has_tractor_beam` as always true. Bind **`Z`** to auto-pull nearby dropped
  loot into inventory (tractor logic at `outfitting.cpp` ~L949).
- **4b. Unified inventory model.** `inventory.{h,cpp}`:
  `ItemKind{Commodity,Salvage,Weapon,Upgrade}`, `Rarity{Basic,Rare,Legendary}`,
  `InventoryItem{id, kind, rarity, qty, mods}`. Add
  `std::vector<InventoryItem>` to `PlayerState`; commodities + items draw from
  the **same `cargo_capacity`** (1 per discrete item). Extend `savegame.cpp`.
- **4c. Salvage drops + loot tables.** Kills (and asteroids) drop loot entities
  (`scrap_metal` + salvage, sometimes cargo, occasionally a weapon). Reuse the
  dropped-cargo/tractor path; `assets/data/loot_tables.json` per faction/class.
- **4d. Gun-mount rarity + weapon rarity tiers.** Promote `gun_mounts[i]` to a
  struct `{gun_id, rarity, mods}`. `Rare = +10% fire / −10% energy`,
  `Legendary = +20% / −20%`, applied in `firing.cpp`/`gun.cpp`. Aces (named/elite
  NPCs) carry a tiny legendary drop chance. `savegame` reads old bare-string
  mounts as Basic (back-compat).
- **4e. Persistent upgrades.** Player-level `permanent_mods` list in
  `PlayerState` (e.g. legendary Shield Matrix = +5% shield), applied at
  `apply_player_loadout`. Survives ship purchases + save/load; the lossy hull
  swap keeps these (unlike fitted guns).
- **4f. Base-side sell & equip.**
  - **New `BaseScreen::CargoHold` sub-screen** (enum + `base.json` hotspot +
    `register_screen`): view the unified hold; sell weapon/upgrade items at a
    rarity-scaled price.
  - **Salvage-as-commodities** (`scrap_metal` etc. are real `commodity` ids) so
    they sell through the existing Commodity Exchange — no new UI.
  - **Equipment screen** gains: fit a looted weapon item into a mount (carries
    rarity), and **install** an Upgrade item into `permanent_mods`.
  - New headless `inventory::sell_item / equip_weapon / install_upgrade`
    (mirrors `outfitting`); `assets/data/loot_prices.json` (or `gun_price ×
    rarity multiplier`) for values; `savegame` serializes items + mount rarity
    + permanent_mods.

---

## Cross-cutting work

- **Save/load:** `savegame.cpp` extended for leads, inventory items, mount
  rarity struct (with back-compat read), permanent_mods.
- **Debug panel:** force-trigger search / scenario / rumor / loot drop; "give
  legendary X" button.
- **Voice coverage:** map `confed_f` to a female clip; author new scenario +
  rumor dialogue via the existing `gen_comms.py` → `synth_comms.py` pipeline.

## Milestone order

1. Phase 0 — voice plumbing.
2. Phase 1 — contraband search (builds the aggro override).
3. Phase 4a + 4b + 4c — tractor + inventory + scrap salvage (unlocks rewards).
4. Phase 2 — scripted Confed-distress set.
5. Phase 3 — rumors + dynamic nav objectives.
6. Phase 4d/4e/4f — rarity, persistent upgrades, base sell/equip.

# Persistent World — Interface Contracts (FROZEN)

Companion to `docs/persistent_world_plan.md`. This file exists so that every
issue in the Gemini Lives epic (#170) can be worked **in total isolation**:
each contract below is the agreed interface between issues. Implement against
the contract; stub the other side if it is not merged yet. Changing a
contract requires editing THIS file in the same PR and calling it out.

Issue map: #171 clock, #172 schedules/director, #173 named spawns,
#174 triggers/barks, #175-#178 + #180 cast, #179 Kilrathi voices,
#181 defector, #182 cinematics.

---

## C1. World clock (#171 provides; #172, #174, #182 consume)

```cpp
// src/world_clock.h
namespace world_clock {
    // The day counter lives on PlayerState (int day = 0; see C2).
    // These helpers are pure functions of that value.
    std::string stardate_string(int day);   // day 0 -> "2669.135"; rolls to "2670.001" after 2669.365
    int         quarter(int day);           // 1..4, 91-day blocks from day 0
    constexpr int k_epoch_year = 2669;
    constexpr int k_epoch_doy  = 135;       // stardate of day 0
    constexpr int k_concordia_news_day = 108;  // 2669.243 world-event gate
}
```

- Day advances by exactly +1 on every successful dock/landing, at the same
  site as autosave-on-land in `main.cpp`. Nothing else advances it.
- **Stub for isolated work:** a free function returning a constant or a
  dev-remote-settable int. Consumers must take the day VALUE as input
  (never read PlayerState directly outside the host layer).

## C2. Savegame v8 (#171)

- `PlayerState` gains `int day = 0`.
- `savegame::k_format_version` bumps 7 -> 8; key `"day"`; older saves
  default 0. No other schema changes ride in v8.

## C3. `assets/data/npc_schedules.json` (#172 owns; #175-#178, #180, #181 author data)

```json
{
  "schedules": [
    {
      "id": "reesa_vance_ore_run",
      "npcs": ["kort", "vance"],
      "formation": "convoy",
      "route_note": "free-text documentation",
      "legs": [
        { "days": [0],    "system": "troy",        "at": "nav:Junction Jump",    "activity": "outbound" },
        { "days": [2, 3], "system": "new_detroit", "at": "base:New Detroit",     "activity": "docked" }
      ],
      "period_days": 6
    }
  ]
}
```

Rules (enforced by `tools/schedule_lint.py`, part of #172):
- `leg = legs[current_day % period_days]` — resolution is a pure function.
- `days` arrays across legs must be disjoint and cover `0..period_days-1`.
- `at` is `nav:<name>` (in space) or `base:<name>` (dockside; NOT spawned in space).
- `system` values are galaxy ids (`assets/systems/<id>.json` must exist).
- One NPC must never be in two places on the same day (checked across ALL schedules).
- `activity` is free-text surfaced to bark tables (convention: outbound/return/docked/patrol/raid/hiding).
- Cast issues may LAND schedule JSON before #172 merges — it is inert data
  until the director loads it; lint in CI once #172 lands.

## C4. NPC director API (#172 provides; #173, #174 consume)

```cpp
// src/npc_director.h
namespace npc_director {
    void load(const std::string& path);   // idempotent, non-fatal on missing file
    void reset();                          // system load / launch sites
    void tick(const Ctx& ctx);             // spawn/despawn per current day+system
    bool present(const std::string& npc_id);                    // spawned in current system now
    bool docked_here(const std::string& npc_id, const std::string& base_id);
}
```

- Spawn positions: 15-22 km offset from the scheduled nav (anti-collision
  discipline from the jump-gate encounter fix).
- **Stub for isolated work:** `present()`/`docked_here()` returning values
  from a dev-remote override table.

## C5. Named-NPC spawn fields (#173 provides; #172, #180, #181 consume)

- Spawn request gains: `npc_id` (bible key), `display_name`, `invulnerable`.
- Invulnerability semantics ("retreat-not-die"): hull damage clamps at ~10%;
  AI breaks off, barks a disengage line, warps out. No kill flags, no loot,
  no rep, no mission/bounty credit.
- Named hails route voice + portrait through the character bible entry (C7).
- Ghraffid (#181) additionally needs a per-spawn stance override:
  Kilrathi-faction ship that is NON-hostile unless fired upon.

## C6. Trigger `when` extensions (#174 provides; #182, #181 consume)

New optional fields in `assets/cinematics/triggers.json` entries:

```json
{ "cinematic": "convoy_first_meet",
  "when": { "npc_present": "vance",
            "day_min": 0, "day_max": 107,
            "day_mod": { "period": 6, "days": [0, 1] } } }
```

- All conditions AND with the existing set (system/nav/flags/once/cooldown).
- `npc_present` consumes C4 `present()`; day gates consume C1 (values ride
  into `TriggerCtx` as data / std::function — the module stays engine-free
  and headless-testable).

## C7. Character bible extensions (`assets/data/characters.json`)

Existing fields (appearance/wardrobe/personality/voice_id) power portraits
+ TTS. This epic adds, per named NPC:

```json
"sian": {
  "display_name": "Sian Turner",
  "appearance": "...", "wardrobe": "...", "personality": "...",
  "voice_id": "female_2", "voice_speed": 0.92,
  "faction": "merchant", "role": "freighter_captain",
  "ship_class": "drayman", "home_base": "Oxford",
  "schedule_id": "sian_luxury_run",
  "invulnerable": true
}
```

- `voice_id` override wins over `tools/cinematics/voices.py` DEFAULT_VOICE_MAP.
- Full character copy (appearance/personality text) lives in the cast issues
  #175-#178, #180, #181 — paste from there.

### Voice assignments (audition results, 2026-07 — AUTHORITATIVE)

| Character | voice_id | Source |
|---|---|---|
| krieg | PrivBarMercgirl01 | audition |
| quist | PrivBarMercgirl01 | audition (never co-appears with krieg) |
| ferao | PrivFlightV0601 | audition |
| sian | female_2 (preset) | interim — real-life clone pending |
| chen_wl | female_1 (preset) | assigned |
| tayla | PrivBarTayla01 | original-actor clone |
| miggs | PrivBarMiggs01 | original-actor clone |
| murphy | PrivBarLynn01 | original-actor clone |
| goodin | PrivBarSandra01 | original-actor clone |
| cross | PrivBarTaryn01 | original-actor clone |
| brandt | PrivFlightV1201 | corrected (V0901 is Kilrathi) |
| sorrel | PrivFlightV0701 | assigned |
| voss | PrivFlightV0501 | assigned |
| harker | PrivFlightV0801 @0.95 | assigned |
| ives | PrivFlightV1101 | assigned |
| okafor | PrivFlightV1301 | assigned |
| brother_silas | PrivFlightV0401 @0.88 | assigned |
| pilgrim_ruth | AUDITION PENDING | sweet_girl preset is candidate |
| nakhra / vakka_kta / ghraffid | KilVoice01/02/03 | blocked by #179 |

## C8. Voice tooling conventions (#179 + all voiced content)

- Valid MiniMax emotions: `happy, sad, angry, fearful, disgusted,
  surprised, neutral`. ANYTHING ELSE fails silently at the API — voices.py
  validates and rejects loudly as of this epic.
- TTS text discipline (learned on penders_crusader): no ALL-CAPS words,
  use `<#0.3#>` pause markup, rephrase words the model mangles
  (e.g. "heretic"), normal sentence case.
- Voice-first timing: generate audio, measure duration (afinfo), THEN set
  cue `dur` = measured + ~0.1-0.5s pad.
- Kilrathi diction baseline (full guide lands with #179): no contractions,
  formal warrior-caste constructions, clan names as `X nar Y`, epithets
  "ape/hairless one" for humans. Authors may draft Kilrathi lines against
  this baseline before #179 merges; regenerate audio after.

## C9. Plot-flag namespace reserved by this epic

```
nakhra_met, nakhra_duel_1, nakhra_duel_2, nakhra_named,
ghraffid_met, ghraffid_spooked, ghraffid_trusted,
ruth_doubt_1, ruth_doubt_2, ruth_doubt_3,
penders_crusader_cleared (already live)
```

Set/cleared exclusively through `plot::run_action` grammar
(`set_flag:<f>` etc.) from cinematic `end.actions` / scenario `on_cleared`.

---

## Per-issue stub cheat-sheet

| Issue | Can start now | Stub needed |
|---|---|---|
| #171 | yes | none |
| #172 | yes | C1 day value (constant + dev override) |
| #173 | yes | none |
| #174 | yes | C4 present() via override table; day via ctx value |
| #175-#178 | yes | none for bible/portraits/voices; schedules land as inert data (C3) |
| #179 | yes | none |
| #180 | portraits/bible yes | voices wait on #179; lines draftable per C8 baseline |
| #181 | bible/portrait/schedule yes | voices #179; trigger wiring #174; spawn stance #173 |
| #182 | scene authoring yes (manual /cinematic/play) | trigger wiring per C6 when #174 lands |

# Persistent World Plan — "Gemini Lives" 

**Objective:** Evolve the cinematic/character work (Vera, Vance, Old Mack, Reesa) into a
persistent, lore-friendly living world: a real calendar, ~40 named NPCs with
schedules, scripted Grayson interactions, and a discoverable Kilrathi defector.

**Setting anchor (canon):** The year is **2669** — the darkest hour. The
*TCS Concordia* is lost at Vespus; Confed is quietly losing the war and that
fact is classified. Gemini Sector is a backwater that feels the squeeze:
militia stretched thin, pirates bold, Retros surging, and Kilrathi raiders
probing deeper than they should be able to.

---

## 1. Calendar System — Confed Stardates

### Lore research
Wing Commander canon has an explicit, consistent timekeeping format used in
every game, manual, and novel: the **decimal stardate `YYYY.DDD`** — the year
followed by the day-of-year (001–365). Examples from canon:

| Source | Date |
|---|---|
| Claw Marks (WC1 manual) | 2654.110 |
| WC2 mission briefings | 2667.xxx |
| WC3 opening (Concordia wreckage found) | 2669.243 |
| Victory Streak (WC3 manual) | 2669.xxx |

**Recommendation: adopt canon `2669.DDD` stardates, not Stardew seasons.**
It's zero-invention lore-friendly, it's a single int under the hood, and it
gives us 365 "days" of runway before we even have to think about year
rollover. Space has no seasons — but Gemini can have **quarterly economic
cycles** (see below) if we want the Stardew-style rhythm without breaking
fiction.

### Design
- **`day` counter on `PlayerState`** (int, 0-based). Display as
  `2669.(135 + day)` — starting the sandbox on **2669.135** (mid-year, a few
  months before the Concordia news breaks at 2669.243 — that date becomes a
  scripted world event we can use later!).
- **Advance +1 day on every landing** (dock at base/planet). That's the only
  clock. Simple, predictable, save-persisted.
- **Quarterly rhythm (optional flavor, not seasons):** `Q1–Q4` =
  91-day quarters. Commodity price modifiers, bar-chatter themes, and
  schedule variants can key off the quarter. This is the Stardew energy,
  wearing a Confed uniform.
- **Persistence:** savegame format **v8** adds `"day": int`. Older saves
  default `day = 0`. (savegame.h versioning discipline already handles this.)
- **UI:** stardate shown on the landing/concourse screen, save labels
  ("2669.148 — New Detroit"), and optionally the nav computer.

### Files
| File | Change |
|---|---|
| `src/world_clock.h/.cpp` (new) | day counter, stardate formatting, quarter helper |
| `src/player.h` | `int day = 0` |
| `src/savegame.cpp/.h` | v8: serialize `day` |
| `src/main.cpp` | advance on dock; show stardate on concourse |
| `src/dev_remote.*` | `/state` exposes stardate; `/advance_day` for testing |

---

## 2. The Cast — ~40 Named NPCs

All stored in `assets/data/characters.json` (existing character bible —
appearance/wardrobe/voice already power portraits + TTS), extended with new
fields: `faction`, `role`, `ship_class`, `home_base`, `schedule_id`,
`invulnerable: true`.

### Canonical expansions (already in the bible or straight from Privateer/WC3)
| ID | Name | Role | Notes |
|---|---|---|---|
| `tayla` | Tayla | Pirate fixer → now flies | Canonical. Give her a Talon + smuggling circuit out of Pentonville |
| `murphy` | Lynn Murphy | Militia commander | Canonical (RF). Patrol schedules, no-nonsense hails |
| `cross` | Taryn Cross | Ex-Confed drifter | Canonical. Wanders fringe systems |
| `goodin` | Lt. Sandra Goodin | Confed liaison | Canonical (WC3). Stationed at Perry Naval Base; hints at classified war news |
| `miggs` | Miggs | Loan-shark muscle | Canonical. Lurks New Detroit; unpleasant |
| `masterson`, `monkhouse`, `lynch`, `sandoval`, `terrell` | — | existing bible | already present; get schedules where sensible |

### New characters (10-ish per faction bucket)

**Merchants / Freighter captains** (join Reesa & Vance):
1. `sian` — Adaeze Sian, Drayman captain, luxury goods, Oxford ↔ New Constantinople
2. `brandt` — Kurt Brandt, grumpy ore hauler, Troy ↔ Hector circuit
3. `ferrao` — Luz Ferrão, tobacco/liquor runner, gray-market, Junction ↔ Palan
4. `chen_wl` — Wen-Li Chen, medical supplies, follows plague rumors (Pestilence, Famine…)

**Pirates:**
5. `dekker` — "Halfjaw" Dekker, Tayla's rival, ambushes Pyrenees lanes
6. `sorrel` — Sorrel Vane, charming toll-collector, "insurance" hails before shooting
7. `krieg` — Mad Anya Krieg, ex-militia gone rogue, hunted BY Lynn Murphy (schedule intersection!)

**Retros** (Vera's brothers-in-hate):
8. `brother_silas` — Brother Silas, Vera's superior, calm and far scarier
9. `pilgrim_ruth` — Ruth, young zealot with doubts (redemption arc potential)

**Bounty hunters:**
10. `voss` — Ilya Voss, professional, polite, hunts Krieg & Dekker on posted bounties
11. `quist` — Marisol Quist, flashy Centurion ace, competes with the player for kills

**Militia:**
12. `velez` — Captain Tomas Velez, Basque-based Talon patrol commander; dry, fair, and suspicious of both privateers and distant Confed brass. Voice-ready as `PrivFlightV0101`; suited to inspections, patrol briefings, and civilian-protection scenes.

**Confed:**
13. `harker` — Cmdr. Dane Harker, Gilgamesh-class captain, patrols War/Perry corridor
14. `ives` — Major "Songbird" Ives, Confed ace on rotation from the front, shell-shocked, drops hints the war is going badly
15. `okafor` — Rear Adm. Chidi Okafor, Perry brass, only appears dockside

**Kilrathi:**
16. `nakhra` — Nak'hra nar Kiranka, rival ace, Dralthi wing, prowls Kilrathi-border jumps, develops a rivalry with Grayson (recurring duels, taunting hails)
17. `vakka_kta` — Vakka'kta, Fralthi commander, raid-schedule incursions
18. **`ghraffid` — Ghraffid nar Hhallas, THE DEFECTOR** ⭐

### The Defector (marquee content)
- **Ghraffid nar Hhallas**, a Kamekh corvette commander who refused an order
  to massacre civilian transports. Now hiding with his loyal wing of 4
  Dralthi in the deep fringe.
- **Location: rotates among Lisacc, 41-GS, DN-N1912, Varnus, and KM-252**
  on a slow schedule (all five systems confirmed present in
  `assets/systems/` — Varnus verified).
- Discovery flow: player jumps into the fringe system → trigger fires →
  cinematic: tense standoff, Dralthi wing forms up, Ghraffid hails in
  formal warrior-caste diction. Non-hostile if not fired upon.
- Ongoing: repeat visits unlock dialogue (war intel, Kilrathi honor culture,
  hints of WC3 events), plot flags (`ghraffid_met`, `ghraffid_trusted`),
  and eventually a quest hook (supplies run for his crew?).
- Lore-friendly precedent: Kilrathi defectors are canon (Hobbes, Vak'ta).

---

## 3. Schedules — The Manifest System

New data file: **`assets/data/npc_schedules.json`**

**Lore correction (verified in repo data):** Humboldt and Potter are
*quadrants* of Gemini Sector, not systems — Troy/Pyrenees/Junction/Pentonville
sit in Humboldt; New Detroit/Oxford/New Constantinople/Metsor sit in Potter.
So "Reesa & Vance run Humboldt ↔ Potter" becomes a concrete cross-quadrant
route through real systems:

```json
{
  "schedules": [
    {
      "id": "reesa_vance_ore_run",
      "npcs": ["kort", "vance"],
      "formation": "convoy",
      "route_note": "Humboldt->Potter ore run: Troy -> Junction -> New Detroit and back",
      "legs": [
        { "days": [0],    "system": "troy",        "at": "nav:Junction Jump",    "activity": "outbound" },
        { "days": [1],    "system": "junction",    "at": "nav:New Detroit Jump", "activity": "outbound" },
        { "days": [2, 3], "system": "new_detroit", "at": "base:New Detroit",     "activity": "docked" },
        { "days": [4],    "system": "junction",    "at": "nav:Troy Jump",        "activity": "return" },
        { "days": [5],    "system": "troy",        "at": "base:Achilles",        "activity": "docked" }
      ],
      "period_days": 6
    }
  ]
}
```

- **Resolution rule:** `leg = schedule.legs[(current_day % period_days)]`.
  Pure function of the calendar — no simulation, no drift, fully
  deterministic and save-friendly (nothing extra to persist!).
- **Spawning:** when the player enters a system (or flies near the scheduled
  nav), the **NPC director** checks which named NPCs are "here today" and
  spawns them: correct ship class, name shown in comms/targeting,
  `invulnerable` flag set.
- **Docked legs** put the NPC in the bar/concourse instead (future: bar
  presence = fixer-style conversations keyed to plot flags).
- Reesa & Vance get their **Humboldt-quadrant ↔ Potter-quadrant ore run**
  (Troy -> Junction -> New Detroit) as the flagship schedule. Murphy patrols militia lanes; Nak'hra prowls border
  jumps on a raid cadence; Ghraffid rotates the fringe on a slow loop.

### Invulnerability (interim)
- `ScriptedNPC` spawn flag → ship takes no hull damage below 10%, AI breaks
  off and jumps out ("cinematic disengage") instead of dying.
- Immersion-safe framing: named ships *retreat*, they don't pop. Revisit
  later for consequence-driven vulnerability.

### Files
| File | Change |
|---|---|
| `assets/data/npc_schedules.json` (new) | manifest data |
| `src/npc_director.h/.cpp` (new) | schedule resolution + spawn/despawn + invuln flag; mirrors `scripted::` module patterns (load/reset/tick, non-fatal missing file) |
| `src/encounters.*` | named-NPC spawn support (name, portrait ref, invulnerable) |
| `src/comm.cpp` | named-NPC hails use character voice_id + portrait |
| `tools/schedule_lint.py` (new) | validates schedules: systems/navs/bases exist, period matches legs, no NPC in two places on one day |

---

## 4. Scripted Grayson Interactions

Leverage what we already built — this is the payoff of the cinematic system:

- **Trigger extension:** `cinematic_triggers.h` gains two conditions:
  `npc_present: "vance"` (NPC director says they're spawned here today) and
  `day_min`/`day_max`/`day_mod` (calendar gates). Everything else (flags,
  nav proximity, once-latching) already exists.
- **Tier 1 — comm barks (cheap, many):** schedule-aware hails through the
  existing comm/voice systems. Vance grumbles about pirate activity on the
  ore run; Murphy warns you off if your record is dirty; Nak'hra taunts.
- **Tier 2 — cinematics (expensive, ~8–10 scenes):**
  1. First meeting w/ Reesa+Vance convoy in Humboldt (intro their run)
  2. Vance convoy under pirate attack — Dekker ambush (join or watch)
  3. Vera returns (penders_crusader follow-up, `penders_crusader_cleared` gate)
  4. Brother Silas cold interrogation encounter
  5. Nak'hra first duel challenge (border jump)
  6. Nak'hra rematch (kill-count / day gated)
  7. **Ghraffid discovery standoff** (fringe system) ⭐
  8. Ghraffid trust scene (second visit, flag-gated)
  9. Goodin dockside intel scene (hints of Concordia — foreshadowing 2669.243)
  10. Murphy vs. Krieg — player stumbles into a militia-vs-rogue firefight
- All authored via the **cinematic-director agent** + Studio bridge pipeline
  (portraits, MiniMax voices, hot reload) — the factory is already running.

---

## 5. Execution Phases

**Phase 1 — Clock (small, unblocks everything)**
- [ ] `world_clock` module + PlayerState.day + savegame v8 + dock hook + UI stardate
- Agent: code-puppy · Validate: land 3×, stardate advances, save/load round-trips

**Phase 2 — Character bible expansion**
- [ ] ~20 new entries in `characters.json` (appearance/wardrobe/voice/faction/role/ship)
- [ ] Portrait `_ref.png` per character (gen-ref pipeline, human approval pass)
- [ ] Voice assignments from the MiniMax voice pool
- Agent: cinematic-director (bible + portraits), me for lore copy · Validate: `portraits list` all OK

**Phase 3 — Schedules + NPC director**
- [ ] `npc_schedules.json` (Reesa/Vance run first, then ~10 more)
- [ ] `npc_director` module + named/invulnerable spawn path + comm integration
- [ ] `schedule_lint.py`
- Agent: code-puppy · Validate: warp to Troy on day 0, convoy at Junction Jump; advance to day 2, they're docked at New Detroit

**Phase 4 — Trigger extensions + Tier-1 barks**
- [ ] `npc_present` + day conditions in cinematic triggers
- [ ] Schedule-aware comm barks for all scheduled NPCs
- Agent: code-puppy · Validate: headless trigger tests + in-game hail checks

**Phase 5 — Tier-2 cinematics (the fun part)**
- [ ] 10 scenes above, authored/iterated via Studio pipeline
- Agent: cinematic-director · Validate: in-game playback review (you), like we did for penders_crusader

**Phase 6 — The Defector arc**
- [ ] Ghraffid rotation schedule + discovery/trust cinematics + plot flags
- Agent: cinematic-director + code-puppy · Validate: full discovery flow in Lisacc

---

## 6. Risks & Open Questions

1. **Stardates vs. Stardew seasons — your call.** Plan assumes canon
   `2669.DDD` + quarterly flavor. If you want cozier "10-day season" vibes,
   the world_clock can display both; data model is identical.
2. **Day-advance trigger:** landing-only means a player who never docks
   freezes time. Fine for now? (Could also advance on jump later.)
3. ~~`varnus` system missing~~ — RESOLVED: verified present in
   `assets/systems/varnus.json`; kept in Ghraffid's rotation.
4. **Voice pool pressure:** ~20 new characters need distinct-enough MiniMax
   voices; the pool is finite. Mitigation: share voices across factions that
   never co-appear, tune `speed`/pitch per character.
5. **Named-NPC spawn collisions** with random encounter/militia spawns at the
   same nav — NPC director should claim navs politely (offset spawn like the
   jump-gate fix we shipped).
6. **Scope:** ~40 characters × portraits × voices is real money/time in API
   calls. Phase 2 can start with the ~15 that have schedules/scenes and
   backfill the rest.

---

*Prepared by Biscuit  — say the word and I'll start with Phase 1.*

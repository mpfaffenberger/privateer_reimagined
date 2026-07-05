# Cinematic Studio — in-game authoring UI + agent bridge

The Studio lets you drive the whole cinematic pipeline **from inside the game**:
describe a cutscene in English, describe *when it should fire* in English,
describe the *world state it leaves behind* in English, then fine-tune every
line's voice + portrait generation — all from an ImGui panel.

The engine stays 100% AI/network-free. The split:

```
┌────────────── in-game (C++) ───────────────┐   ┌──────── out-of-game (Python/agent) ────────┐
│ ImGui Studio panel (cinematic_studio.cpp)  │   │ studio_bridge.py (daemon, polls requests/) │
│  · compose brief / triggers / outcome text │──▶│  · "refine" = deterministic regen via      │
│  · per-line: voice profile, prompts, seed  │   │    portraits.py / voices.py w/ overrides   │
│  · writes  assets/cinematics/studio/       │   │  · "author"/English compile = invokes the  │
│           requests/<id>.json               │   │    cinematic-director agent (code-puppy)   │
│  · polls   responses/<id>.json             │◀──│  · writes responses/<id>.json              │
│  · Play / Stop / Reload / Seek (local)     │   └────────────────────────────────────────────┘
│ Trigger evaluator (cinematic_triggers.cpp) │
│ Outcome applier (hook in main.cpp)         │
└────────────────────────────────────────────┘
```

---

## 1. Data-driven TRIGGERS — `assets/cinematics/triggers.json`

English like *"player is flying a tarsus, out of missiles, has ≥20 units of
iron, and comes within 20k of Troy Nav 3"* is compiled (by the
cinematic-director agent) into:

```json
{
  "triggers": [
    {
      "cinematic": "ambush_troy",
      "once": true,
      "cooldown_s": 0,
      "when": {
        "system": "troy",
        "ship_class": "tarsus",
        "missiles_max": 0,
        "cargo": [ { "commodity": "iron", "min_units": 20 } ],
        "near_nav": { "nav": "Troy Nav 3", "radius_m": 20000 },
        "requires_flags": [],
        "forbids_flags": ["ambush_troy_seen"]
      }
    }
  ]
}
```

Semantics:
* Every field in `when` is optional; ALL present conditions must hold (AND).
* Vocabulary v1: `system` (galaxy id), `ship_class`, `missiles_max` /
  `missiles_min` (total across the 3 ammo types), `cargo` (list of
  {commodity, min_units} against the hold), `near_nav` ({nav name, radius_m},
  current system only), `requires_flags` / `forbids_flags` (plot.h).
* `once` (default true) latches per-session after firing; `cooldown_s` gates
  re-fires when `once=false`. Triggers only evaluate in Flight, never while a
  cinematic (or the death cam) is active.
* Missing/unparseable file = log one line + no triggers (voice::load policy).
* Hot-reloaded by `POST /cinematic/reload` and the Studio's Reload button.

Engine: `src/cinematic_triggers.{h,cpp}` — `load(path)`, `reset()`,
`tick(const TriggerCtx&, float now_s)`. `TriggerCtx` is built by main.cpp per
Flight frame (system id, ship class, missile total, player pos, nav lookup,
cargo-units lookup, PlayerState* for flags) — same decoupling as
`scripted::WorldCtx`.

Phase-A implementation notes (shipped):
* Firing goes through `set_play_hook(fn)` (the `set_despawn_hook` seam
  pattern): main.cpp wires it to `cinematic::play(id)` gated on Flight /
  no-active-cinematic. A refused fire does NOT latch — the trigger retries
  next frame. This keeps the module engine-free and headlessly testable
  (tools/test_cinematic.cpp exercises latch + cooldown with a fake hook).
* `now_s` is the same wall-clock `scripted::tick` uses (cooldown bookkeeping).
* Nav names resolve exact-first, then bare-prefix ("Troy Nav" matches
  "Troy Nav 8") — the /dock matcher's tolerance.
* At most ONE trigger fires per tick. `reset()` is wired at both
  `scripted::reset` sites (system load, base launch).

## 2. OUTCOME — post-cinematic world state (in the cinematic JSON)

English like *"after the cinematic the player is at Troy Nav 3 and 3 Confed
Stilettos are fighting 3 Kilrathi Dralthi"* compiles into a top-level block of
the cinematic file:

```json
"outcome": {
  "player_at_nav": "Troy Nav 3",
  "spawns": [
    { "class": "stiletto", "faction": "confed",   "count": 3 },
    { "class": "dralthi",  "faction": "kilrathi", "count": 3, "hostile": false }
  ]
}
```

Semantics:
* Applied when the cinematic ends **naturally or is skipped** (a skip must not
  strand the story). NOT applied on external `/cinematic/stop`.
* `player_at_nav`: teleport the player to that nav in the current system.
  Pair it with a `location` block (below) so "current system" is guaranteed
  to be the one the nav actually lives in.
* `spawns`: groups spawned near the player via `encounter_spawn`; natural
  faction hostility makes confed-vs-kilrathi brawl immediately.
  `hostile: true` additionally aggros the player.
* Engine: parsed in `cinematic_parse` (engine-free, non-throwing);
  `cinematic.cpp` fires a registered `set_outcome_hook(fn)` — main.cpp
  implements teleport + spawns (same seam pattern as `set_despawn_hook`).

## 2b. LOCATION — entry-point teleport (in the cinematic JSON)

A cinematic is authored against a specific backdrop. The optional top-level
block records it, and **every play path** (Studio panel, `/cinematic/play`,
the `play_cinematic:` plot token, triggers, `--play-cinematic`, F8/F9)
teleports the player there BEFORE the timeline starts:

```json
"location": { "system": "penders_star", "nav": "Asteroid Field" }
```

Semantics (host helper `cinematic_play_located` in main.cpp; the engine only
parses — `cinematic_parse.h` `Location` + `cinematic::peek_location`):
* **same system**: snap the player to the nav (the `/dock` parking recipe:
  nav pos + small offset, zero velocity), then play.
* **different system**: queue the SAME deferred frame-boundary switch
  `/goto` uses (`g.pending_goto`) and park the id in `g.pending_cinematic`;
  the Flight tick replays the helper after the rebuild — snap, then play.
* **unknown system**: play is REFUSED with a clear error (dev_remote reply +
  Studio status line). Unknown nav *name* in the right system degrades to
  "play in place" + a log line; `tools/cinematics/validate` warns on both.
* Authoring: `Cinematic.location(system, nav)` — ALWAYS emit it, derived from
  the trigger's `system` / `near_nav.nav`; `outcome.player_at_nav` must name
  a nav that exists in that same system.

## 3. STUDIO REQUEST PROTOCOL — `assets/cinematics/studio/`

* `requests/<id>.json` (written by the in-game UI):

```json
{
  "id": "req_1730000000",
  "kind": "author",                    // "author" | "refine"
  "cinematic_id": "ambush_troy",       // target (refine) / suggested id (author)
  "brief": "Grayson gets ambushed leaving Troy...",
  "triggers_text": "player flying a tarsus, out of missiles, ...",
  "outcome_text": "player ends at Troy Nav 3; 3 confed stilettos fight 3 dralthi",
  "image": { "quality": "medium", "style_extra": "harsher rim light" },
  "line_overrides": [
    { "index": 0, "voice_id": "PrivFlightV1501", "speed": 1.0,
      "text": "Wrong lane, Troy-boy.",
      "emotion": "cold sneer, leaning into the comm",
      "portrait_prompt_extra": "red warning light on the face",
      "seed_image": "assets/cinematics/portraits/pirate/_ref.png" }
  ],
  "regen": { "portraits": [0], "voices": [0] }   // indices into line cues, or "all"
}
```

* `responses/<id>.json` (written by the bridge): `{"id", "status":
  "working"|"done"|"error", "message", "cinematic_id", "log": [...]}`.
* The UI lists pending/done requests and offers one-click Reload+Play when done.

Phase-B implementation notes (shipped — the panel's emitter is
`cinematic_studio_io::request_to_json`, round-tripped in test_cinematic):
* Empty optional keys are **omitted**, never emitted as `""`/`[]`:
  `cinematic_id`, `brief`, `triggers_text`, `outcome_text`,
  `image.style_extra`, `line_overrides`, `regen`. The `image` block (with
  `quality`) is always present. Inside a line override only the fields the
  user actually changed are emitted. The bridge treats absence as "keep".
* Strings are raw UTF-8 (only spec-mandated escapes; no `\uXXXX`).
* Override/regen `index` counts **line cues only** (0 = first `line` in the
  timeline), matching the example above.
* The panel emits index lists for `regen`; the `"all"` shorthand remains
  legal for hand-written requests and the bridge must accept both.
* Request ids are `req_<unixtime>`, suffixed `_<n>` when two sends land in
  the same second.
* `emotion` is request-only: it is NOT stored in the cinematic JSON — the
  panel keeps a per-session side map and persists it only into refine
  requests.
* The UI's Requests tab rescans `requests/` + `responses/` every ~2s (or on
  Refresh) and joins them by id; a missing response file displays as
  `pending`. `[Delete]` removes the request+response pair.
* `kind:"refine"` is **deterministic** (no LLM): the bridge re-runs
  `portraits.gen_line` / `voices.gen_line_voice` with the overrides.
  `kind:"author"` (and any English trigger/outcome compile) is handed to the
  cinematic-director agent.

Phase-C implementation notes (shipped — `tools/cinematics/studio_bridge.py`):
* Run from the repo root: `python -m tools.cinematics.studio_bridge`
  (daemon, 2s poll) / `--once` (drain queue and exit) / `--interval N`.
* The bridge creates `responses/` itself, writes `status:"working"` BEFORE
  processing, and NEVER deletes requests (the panel owns their lifecycle).
  The request **filename stem is the id**; ids with a terminal (`done` /
  `error`) response are skipped forever. A leftover `working` response is
  treated as a crashed previous run and reprocessed on restart — run ONE
  bridge at a time.
* **refine**: portraits regenerate to the SAME `portraits/<char>/<scene>_<seq>.png`
  path (character/scene/seq parsed from the cue's portrait path; unparseable
  paths — e.g. a `_ref.png` — are log-and-skip, never guessed). Voices
  regenerate to the same `voice_file` path (`force=True` bypasses the
  exists-skip); a line without one gets `voices.py`'s stable hash name wired
  in via `builder.apply_overrides`, which also applies `text` changes —
  untouched cues are never rewritten. `image.style_extra` +
  `portrait_prompt_extra` ride inside the prompt (so in the cache key);
  `seed_image` is the gen-line `ref_override`; `image.quality` maps to
  `OPENAI_IMAGE_QUALITY` for the duration of the call.
* **author**: shells out headlessly —
  `code-puppy --agent cinematic-director -p "<compiled prompt>"` (cwd = repo
  root, 10 min timeout, stdout tail captured into `log[]`). The prompt embeds
  brief/triggers_text/outcome_text/cinematic_id, points the agent at this doc
  + `docs/cinematic_format.md`, and instructs: author via `builder.py`
  (`auto_portraits` + `auto_voices`), MERGE the compiled trigger into
  `triggers.json`, compile the outcome block, validate, print the id. If the
  CLI is missing or `STUDIO_BRIDGE_NO_AGENT=1` (CI), the response degrades to
  `status:"error"` with `message: "run: <exact command to paste>"`.
* **DEVIATION — key softening**: a missing image key does NOT fail a refine
  (portrait regen degrades to the placeholder backend, matching the rest of
  the pipeline), and a missing `MINIMAX_API_KEY` skips just the voice regens
  with a log note (existing audio kept). Only structural problems — bad
  request JSON, unknown kind/cinematic, out-of-range index — produce
  `status:"error"`.
* **DEVIATION — emotion & TTS**: the per-line `emotion` drives the PORTRAIT
  only; it is deliberately NOT sent to MiniMax `t2a_v2` (its `voice_setting`
  emotion enum is small, fixed, and unreliable with cloned voices on
  `speech-2.8-hd`, and Studio emotions are free-form prose). Voice delivery
  knobs are `voice_id` + `speed`.

## 4. Voice profiles — `assets/data/voice_profiles.json`

The selectable list for the UI dropdown (id + label + flavor), generated from
the cloned MiniMax cast (synth_scenario_voices VOICE_MAP + voice_bank.json).
`voices.py` honors `voice_id` overrides per line; `characters.json` may pin a
default per character.

Shipped format (Phase A): a TOP-LEVEL JSON ARRAY (not wrapped in an object):

```json
[ { "id": "PrivBarPc01", "label": "Player (Grayson)", "flavor": "..." }, ... ]
```

10 profiles — the union of `tools/synth_scenario_voices.py` VOICE_MAP and
`tools/cinematics/voices.py` DEFAULT_VOICE_MAP: PrivBarPc01 (player),
PrivFlightV0801 (confed male — also the TTS fallback), PrivMerchantFem01,
PrivFlightV1401 (merchant male), PrivFlightV0401 (retro), PrivFlightV1001
(steltek/eccentric), PrivFlightV1501 (pirate), PrivFlightV0101 (militia),
PrivFlightV0501 (bounty hunter), PrivFlightV0901 (kilrathi). Hand-authored
data; extend by appending — the ids must exist in the MiniMax cloned cast.

## 5. Seed images

Per-character reference override: the Studio can point a character's
generation at ANY image path (`seed_image`), which `portraits.gen_line`
uses as the conditioning reference instead of `_ref.png`. Selecting a new
canonical look = copy it to `portraits/<char>/_ref.png` via the UI button.

## Build phases

* **A (engine)**: `cinematic_triggers.{h,cpp}` + outcome parse/hook/apply +
  `voice_profiles.json`.
* **B (engine)**: `cinematic_studio.{h,cpp}` ImGui panel (compose / requests /
  line editor / playback), **Ctrl+K** to toggle. File IO/JSON shaping lives
  in the engine-free `cinematic_studio_io.{h,cpp}` (headlessly tested in
  tools/test_cinematic.cpp). Playback verbs share the exact dev_remote
  lambdas (main.cpp wires both), so panel + HTTP behave identically; the
  panel stays usable while a cinematic plays (Stop/Seek mid-cutscene).
* **C (python, shipped)**: `studio_bridge.py` (poll/refine/author daemon) +
  `portraits.py` `--ref` / `--prompt-extra` (both in the cache key) +
  `voices.py` `force=` regen + `builder.apply_overrides` (surgical per-line
  JSON edits). See Phase-C implementation notes in §3 and the Studio workflow
  in `tools/cinematics/README.md`.

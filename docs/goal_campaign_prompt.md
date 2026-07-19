# /goal prompt — implement the full Privateer story campaign

Copy everything below the line into the goal runner.

---

Implement the complete Wing Commander: Privateer story campaign (23 plot
missions, 9 fixer chains) in this repo, end to end, until it is playable
from a fresh save through the drone finale.

## Ground truth — read these FIRST, in this order

1. `docs/GAP_ANALYSIS.md` — what exists vs what's missing. Trust code
   over comments: many comments are stale liars (see issue #149). Verify
   claims with grep before acting on any comment.
2. GitHub issues (read via `gh issue view <n>`):
   - #136 — campaign epic (mission checklist + infra build order)
   - #137, #138, #139, #140, #146 — infrastructure blockers
   - #113–#135 — one issue per mission: briefing, exact enemy waves,
     payouts, world-state changes, failure policy, implementation notes.
     Treat each issue as the mission's spec. Do not invent content that
     contradicts them.
3. `gamefaq` (repo root) section 12 — the original walkthrough the specs
   were derived from; consult when an issue is ambiguous.

## Build order — strictly this sequence

Phase 0 is the foundation; each mission phase must be playable before
starting the next. Never build mission N+1 on unverified mission N.

**Phase 0 — infrastructure** (#137, #138, #139, #146)
1. Plot-flag layer + plot items on `PlayerState`, savegame bumped to v7
   (#138). Stable string keys, older saves default to campaign-off.
   Debug-panel get/set for flags.
2. Bar screen + fixer framework (#137). Replace the `"BAR - fixers TBD"`
   stub in `base_screens.cpp`. Fixer registry supports BOTH fixed base-id
   placement AND predicate placement (M22 needs "any mining base except
   rygannon"). Conversation UI (portrait + accept/refuse), reused later
   for the Oxford library scene and Terrell's office. Bar art exists at
   `assets/concourse/*/bar_*.png`; VO at `assets/speech/bar/`.
3. Scripted-encounter extensions (#139): named NPCs with kill-memory,
   conditional re-ambush, talk-then-attack, multi-wave kill-all-at-nav
   objectives, trigger-region spawns, mission-scoped encounter-density
   modifiers, comm-interaction objective. All data-driven in
   `assets/data/scripted_encounters.json` — grow the grammar, keep C++
   generic.
4. `Faction::Steltek` (#146) — append-only enum add; everyone-hostile;
   rep serialization is name-keyed so saves survive.

**Phase 1 — Sandoval** (#113): cargo run + bar-conversation settle +
artifact plot item + fixer handoff to Tayla. Smallest possible proof of
the whole pipeline. STOP and verify end-to-end before continuing.

**Phase 2 — Tayla arc** (#114–#117): pirate-neutrality stance override
(scoped, reverting), contraband runs with mission-scoped militia heat,
the secret compartment (20 units, scan-exempt, contraband-only — touches
`inventory.*`, `carrying_contraband`, Cargo Hold UI), Riordian
(named NPC + conditional re-ambush + talk-then-attack).

**Phase 3 — Lynch arc** (#118–#121): comm-delivery objective (Seelig),
Kroiz re-ambush, passenger payload (completes on landing), the Miggs
betrayal with mid-mission objective rewrite + optional trigger-region
ambush.

**Phase 4 — Masterson arc** (#122–#125): the escort mission type (#140).
Friendly Drayman with travel goal, per-wave aggro targeting (escortee vs
player), landing-order completion gate, escortee-death = fail. Library
scene grants the Monkhouse lead.

**Phase 5 — Murphy + Monkhouse** (#126–#129): blockade docking-refusal on
Palan (plot-gated, lifts after M16), multi-wave kill-alls, player-allied
wingmen (M16), Kilrathi route-conditional ambush + map/translator plot
item (M17).

**Phase 6 — Cross arc** (#130–#133): plot-gated jump links to the
frontier (delta/beta/gamma/delta_prime jump refusal until unlocked —
extend the existing dangling-jump behavior in `galaxy.cpp`), flesh out
the four frontier system JSONs (delta_prime.json is an 831-byte skeleton;
`assets/bases/derelict_base` and `assets/ships/derelict` already exist),
Garrovick, the Kilrathi gauntlet, the derelict interaction that grants
the Steltek gun (make it unbuyable in shops), and the cross-system
invulnerable drone pursuer (`drone_active`, damage-multiplier-zero flag,
rate-limited spawns).

**Phase 7 — finale** (#134, #135): predicate-placed Goodin fixer, Retro
gauntlet, the Steltek mid-route scene (gun boost event), the Confed fleet
at Blockade Point Tango (friendly vs drone but lethal to player), damage
gating so ONLY the boosted Steltek gun hurts the drone
(weapon-whitelist: extend the immunity flag, do not special-case
`firing.cpp`), Terrell office finale, `campaign_complete`.

## Engineering rules (non-negotiable)

- Match existing architecture: data-driven JSON in `assets/data/`,
  registration-hook screens, headless-testable model/view splits
  (MISSIONS_HEADLESS pattern), name-keyed save fields, log-and-carry-on
  loaders. Read neighboring modules before writing new ones.
- Files under 600 lines. New campaign code goes in new modules (e.g.
  `src/fixers.*`, `src/plot.*`, `src/campaign_missions.*` or data),
  NOT into `main.cpp` (it's roughly 390 KB of prior sin — see #151; do not
  make it worse).
- Append, don't repurpose: enums, save fields, mission types.
- Keep each PR/commit scoped to one phase step; build + test between.
- The sandbox must remain 100% intact for players who never talk to
  Sandoval. Campaign-off is the default state and must stay invisible.
- Fix stale comments you touch in passing; do not trust them.
- No emojis anywhere in code, comments, or commit messages.

## Verification loop (every step)

1. `cmake --build build -j` — must compile clean.
2. Run existing headless tests (see `tools/test_*`); add headless tests
   for: plot-flag save round-trip (v6 -> v7 migration), fixer gating
   predicates, each new objective type's settle logic, secret-compartment
   scan exemption, damage whitelist.
3. Live smoke via the agentic-testing HTTP API (dev_remote, port 47001;
   see `src/dev_remote.h`): boot with `--dev-land <base>` or plain, then
   assert through:
   - `GET /player` — credits paid? rep moved? docked where expected?
   - `GET /missions` — accepted mission present with correct status
     string; gone after settle.
   - `GET /events?since=N` — the assertion stream: every comm feed line
     (mission accept/complete, rep deltas) and mode transition
     ("flight -> landed @ oakham_pirate") appears here. Poll
     incrementally with the returned `latest`.
   - `POST /spawn` / `POST /kill` — stage combat objectives cheaply.
   Extend the API as the campaign needs it (plot-flag get/set lands with
   #138; write-side endpoints are the dev_remote phase-2 issue). Every
   new mission phase should be verifiable by curl alone — if the judge
   can't assert it over HTTP, add the endpoint/event first.
4. Save/load round-trip at every mission boundary; a v6 (pre-campaign)
   save must still load.
5. Update the epic #136 checklist (`gh issue edit`/comment) and close
   each mission issue as it becomes verifiably playable.

## Definition of done

- Fresh save -> full campaign playable in order through M23; drone dies
  only to the boosted Steltek gun; `campaign_complete` set; sandbox
  continues after.
- Every mission failable + re-offerable per its issue's failure policy.
- All new state survives save/load; v6 saves migrate.
- All 24 campaign issues (#113–#136) closed with a verification note.
- `docs/GAP_ANALYSIS.md` P0 section updated to reflect reality.

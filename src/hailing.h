#pragma once
// -----------------------------------------------------------------------------
// hailing.h — contraband search director (Phase 1).
//
// Once-per-encounter search hail for Militia / Confederation NPCs in
// comms range. Phase 1 lives HERE so the wider Phase 2 scripted-encounter
// engine can build on top of it later — the per-NPC interaction state
// (`Idle / Searching / Resolved`) is intentionally the same shape the
// future scenario director will reuse.
//
// Behaviour:
//   * Idle  : in comms range AND global cooldown elapsed -> roll 35% to
//             start a scan. Miss -> Resolved (skip for the encounter).
//             Hit  -> Searching; voice::say(Search) + comm feed push.
//   * Searching : after 2.5 s, or if normal flight carries the player out
//             of comms range, resolve from actual visible cargo. Contraband
//             -> GUILTY (aggro, hostile voice); clean -> "Scan complete.
//             You're clean, safe travels." (voice::Clear).
//   * Resolved : nothing; the per-NPC entry stays in the map until the
//             ship is no longer alive in the registry, at which point
//             the prune pass below drops it.
//
// Wiring (Phase 1): reset() on system load + landing (same site as
// encounters::init). tick() each frame AFTER perception + AI so the
// aggro override we set is visible to the very next AI pass.
// -----------------------------------------------------------------------------

#include "ship_registry.h"

struct PlayerState;   // forward decl: player.h would drag in the whole repo

namespace hailing {

// Clear all per-NPC interaction state + the global search cooldown.
// Call on system load and on landing (same site as encounters::init).
void reset();

// Per-frame search director. Called once per Flight update AFTER
// perception::tick and ship_ai::tick so any aggro override we set this
// frame is read by the AI on the *next* frame (intentional: the search
// itself needs the conversation to play out first).
//
// `now_s` is wall-clock seconds (matches the same `stm_sec(stm_now())`
// the AI tick uses).
void tick(ShipRegistry& ships, const Ship& player_ship,
          const PlayerState& player, float now_s);

} // namespace hailing

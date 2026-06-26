#pragma once
// -----------------------------------------------------------------------------
// scripted_encounters.h — Phase 2 scenario director (data-driven encounters).
//
// A lightweight, data-driven director that lives over the live ShipRegistry.
// Each scenario in `assets/data/scripted_encounters.json` declares:
//
//   * a TRIGGER  — `near_classes` (optional), `near_faction`, `max_count`,
//                   `chance`, `cooldown_s`, `once_per_system`, and
//                   `ignore_player_rep`;
//   * a DIALOGUE — an ordered list of `{voice, line}` turns played one
//                   per `k_turn_gap_s` once the trigger fires;
//   * a SPAWN    — `spawn_on_accept{faction, class, count, delay_s}` for
//                   a one-time wing spawn N seconds after dialogue ends;
//   * a REWARD   — `reward{credits, rep{faction:delta}, loot_roll}` granted
//                   once the spawned wing (if any) is cleared.
//
// Runtime state is file-static: scenarios never change at runtime, so
// the director is just a per-frame evaluation + playback loop with no
// exposed gameplay surface beyond the three entry points below.
// -----------------------------------------------------------------------------

#include "encounters.h"        // encounters::SpawnFn / SpawnRequest
#include "ship_registry.h"

#include <string>

// PlayerState is forward-declared (player.h pulls too much for the header)
// — the .cpp includes player.h directly.
struct PlayerState;

namespace scripted {

// Load the scenario table from `path`. Idempotent — clears any prior table.
// Missing/unparseable file is NON-fatal (logs one line and the director
// becomes a silent no-op). Same "log + carry on" policy as comm::load /
// voice::load so the rest of the engine doesn't gate on the data file.
void load(const std::string& path);

// Clear all runtime state: per-scenario triggered/cooldown flags, the
// active playback slot, and any in-flight spawn / awaiting-resolution
// state. Call on system load AND on launch-from-base (same sites as
// hailing::reset).
void reset();

// Per-frame trigger evaluation + active-playback advancement. Call once
// per Flight update AFTER hailing::tick (so any aggro/state hailing set
// is already visible). `now_s` matches hailing's `t_now` wall-clock.
// `spawn` is the same SpawnFn the encounter director + /spawn command
// use, so scenario-spawned ships share the atlas/sprite slot pool.
void tick(ShipRegistry& ships, const Ship& player_ship, PlayerState& player,
          float now_s, const encounters::SpawnFn& spawn);

} // namespace scripted

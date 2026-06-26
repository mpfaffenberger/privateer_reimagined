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
//                   per `k_turn_gap_s` once the trigger fires.
//
// Wave A (this commit) wires the loader + trigger + dialogue playback path.
// Spawn + rewards (`spawn_on_accept`, `reward`) are PARSED and STORED
// opaquely for a later wave; they are NOT acted on here.
//
// Runtime state is file-static: scenarios never change at runtime, so
// the director is just a per-frame evaluation + playback loop with no
// exposed gameplay surface beyond the three entry points below.
// -----------------------------------------------------------------------------

#include "ship_registry.h"

#include <string>

namespace scripted {

// Load the scenario table from `path`. Idempotent — clears any prior table.
// Missing/unparseable file is NON-fatal (logs one line and the director
// becomes a silent no-op). Same "log + carry on" policy as comm::load /
// voice::load so the rest of the engine doesn't gate on the data file.
void load(const std::string& path);

// Clear all runtime state: per-scenario triggered/cooldown flags and any
// in-flight playback. Call on system load AND on launch-from-base (same
// sites as hailing::reset).
void reset();

// Per-frame trigger evaluation + active-playback advancement. Call once
// per Flight update AFTER hailing::tick (so any aggro/state hailing set
// is already visible). `now_s` matches hailing's `t_now` wall-clock.
void tick(const ShipRegistry& ships, const Ship& player_ship, float now_s);

} // namespace scripted

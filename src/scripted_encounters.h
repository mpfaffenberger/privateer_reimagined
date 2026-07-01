#pragma once
// -----------------------------------------------------------------------------
// scripted_encounters.h — Phase 2 scenario director (data-driven encounters).
//
// A lightweight, data-driven director that lives over the live ShipRegistry.
// Each scenario in `assets/data/scripted_encounters.json` declares:
//
//   * a TRIGGER  — `kind` selects the shape (#139):
//       "near_ships" (default): `near_classes` (optional), `near_faction`,
//           `max_count`, `chance`, `cooldown_s`, `once_per_system`;
//       "at_nav":    player within `radius_m` of nav `nav` (by name) —
//           requires `system`;
//       "in_system": player anywhere in galaxy system `system`;
//       "on_launch": player just launched from base `base` ("" = any) —
//           fed by notify_launch() below.
//     ALL kinds also gate on the campaign plot state: `requires_flags`
//     (all set) + `forbids_flags` (none set) — plot.h. Mission-scoped
//     encounters are just scenarios gated on their mission's flags.
//   * a DIALOGUE — an ordered list of `{voice, line}` turns played one
//                   per `k_turn_gap_s` once the trigger fires (may be
//                   empty for pure combat waves);
//   * WAVES      — `waves: [ { delay_s, spawns: [ {faction, class, count,
//                   name?, unique?} ] } ]`, spawned SEQUENTIALLY: wave
//                   N+1 launches only after wave N is dead. A spawn with
//                   `unique` writes plot flag `killed:<unique>` when it
//                   dies — the kill-memory that conditional re-ambushes
//                   gate on (forbids_flags: ["killed:riordian"]). The
//                   legacy `spawn_on_accept{...}` still parses as a
//                   single wave.
//   * a REWARD   — `reward{credits, rep{faction:delta}, loot_roll}` granted
//                   once ALL waves are cleared;
//   * ACTIONS    — `on_cleared: ["set_flag:x", ...]` run through the
//                   shared plot::run_action grammar when the scenario
//                   resolves — how a scripted fight advances the campaign.
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
struct StarSystem;

namespace scripted {

// World context the new trigger kinds evaluate against (#139). `system`
// may be null during teardown frames — kinds needing it skip cleanly.
struct WorldCtx {
    const StarSystem* system = nullptr;   // live system (nav positions)
    std::string       system_id;          // galaxy id, e.g. "troy"
};

// Latch an "on_launch" pulse: the player just launched from `base_id`.
// Consumed by the first matching on_launch scenario within a short
// window. Call AFTER reset() at the launch site.
void notify_launch(const std::string& base_id);

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
          float now_s, const WorldCtx& world, const encounters::SpawnFn& spawn);

} // namespace scripted

#pragma once
// -----------------------------------------------------------------------------
// ai_brain.h -- the data-driven combat brain: table registry + evaluator.
//
// Loads the condition->maneuver tables (assets/ai/*.ai.json) into a registry,
// resolves which table a ship uses (by faction, fallback "default"), and runs
// the per-tick evaluator that picks + executes a maneuver. This is the
// clean-room reimplementation of the vanilla MNVR `+0x0c`/`+0x14` dispatch and
// PGG's `ProcessLogic` two-channel selection (docs/ai_maneuver_system.md).
//
// Split from ai_maneuver.h (pure data) so the registry/evaluator -- which need
// Ship / ShipRegistry -- don't drag those into every translation unit that
// only wants the enums.
// -----------------------------------------------------------------------------

#include "ai_maneuver.h"

#include <string>
#include <string_view>

struct Ship;
class ShipRegistry;
enum class Faction : uint8_t;

namespace ai_brain {

// Scan `dir` for `*.ai.json` and build the table registry. Logs one line per
// table loaded plus a summary. Returns the count loaded. Safe to call before
// ship spawning; tables are looked up lazily by ship_ai::tick.
int load_all(const std::string& dir);

// Registry lookup. find() returns nullptr on miss; default_table() returns the
// "default" table (or the first loaded, or nullptr if none).
const AILogicTable* find(std::string_view name);
const AILogicTable* default_table();

// Pick the table a ship should use: by faction name, else "default".
const AILogicTable* resolve_for_faction(Faction f);

// Run the combat brain for one ship for this tick: resolve table + tier, run
// the interrupt then logic channels to (re)select a maneuver, then execute the
// selected maneuver (writes s.behavior / s.controller). Called by
// ship_ai::tick only when the ship has a hostile target. Assumes
// s.ai.target_id is already set to the engaged hostile.
void run_combat(Ship& s, const ShipRegistry& all, float t_now);

// Force one specific maneuver this tick, bypassing table selection. Used by
// ship_ai.cpp for the personality override "a hunted Coward flees" (the
// being-targeted signal is external to the condition metrics). Resolves the
// target the same way run_combat does; no-op behavior if the target is gone.
void run_forced(Ship& s, const ShipRegistry& all, float t_now, AIManeuver m);

// Shared helpers (also used by ship_ai.cpp's non-combat gating).
float hp_fraction(const Ship& s);
float flee_threshold_for(const Ship& s);   // f6 -> flee HP fraction (docs s0)

} // namespace ai_brain

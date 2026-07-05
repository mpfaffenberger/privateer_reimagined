#pragma once
// -----------------------------------------------------------------------------
// cinematic_triggers.h — data-driven cutscene TRIGGERS (Cinematic Studio
// Phase A1, docs/cinematic_studio.md §1).
//
// A tiny declarative evaluator over `assets/cinematics/triggers.json`: each
// trigger names a cinematic id plus a `when` block of world conditions
// (system / ship class / missile totals / cargo holdings / nav proximity /
// plot-flag gates). ALL present conditions must hold (AND); every field is
// optional. `once` (default true) latches per-session after firing;
// `cooldown_s` gates re-fires when once=false.
//
// Shape copied from the scripted-encounter director (scripted_encounters.h):
//   * load(path)  — idempotent, NON-fatal on a missing/bad file (voice::load
//                   "log one line + no-op" policy);
//   * reset()     — clears once-latches + cooldowns; call at the same sites
//                   as scripted::reset (system load, base launch);
//   * tick(ctx)   — per-Flight-frame evaluation against a host-built
//                   TriggerCtx (same decoupling as scripted::WorldCtx).
//
// Firing goes through a HOST hook (set_play_hook — the set_despawn_hook seam
// pattern): main.cpp wires it to `cinematic::play(id)` gated on "in Flight,
// no cinematic active, not Dying". A refused fire (hook returns false) does
// NOT latch — the trigger simply retries next frame. This module therefore
// depends on NOTHING but json.h / plot.h / HandmadeMath, so it links into
// the headless test target (tools/test_cinematic.cpp) with zero engine deps.
// -----------------------------------------------------------------------------

#include "json.h"

#include <HandmadeMath.h>

#include <functional>
#include <string>
#include <vector>

struct PlayerState;

namespace cinematic::triggers {

// What one trigger needs to know about the world THIS frame. Built by
// main.cpp per Flight frame; the lookups close over the live StarSystem /
// PlayerState so this header stays engine-free.
struct TriggerCtx {
    std::string system_id;          // galaxy id, e.g. "troy"
    std::string ship_class;         // PlayerState::ship_class_name, e.g. "tarsus"
    int         missiles_total = 0; // sum of player::missile_count over all types
    HMM_Vec3    player_pos{ 0, 0, 0 };

    // Nav-position lookup: name -> world pos, current system only. Returns
    // false for an unknown nav. The host matches bare or suffixed names the
    // way the /dock resolver does ("Troy Nav" matches "Troy Nav 8").
    std::function<bool(const std::string& nav, HMM_Vec3& out_pos)> nav_pos;

    // Cargo-units lookup: commodity id -> units in the hold (summed across
    // stacks, PlayerState::cargo). Unknown commodity = 0.
    std::function<int(const std::string& commodity_id)> cargo_units;

    // For plot::has_flag gates (requires_flags / forbids_flags). A null
    // player fails any flag-gated trigger cleanly.
    const PlayerState* player = nullptr;
};

// One parsed trigger (schema: docs/cinematic_studio.md §1). Sentinel -1 on
// the missile bounds means "condition absent".
struct Trigger {
    std::string cinematic;          // cinematic id to play (required)
    bool        once       = true;
    float       cooldown_s = 0.0f;

    // ---- when (all optional, AND) ----
    std::string system;             // "" = any
    std::string ship_class;         // "" = any
    int         missiles_max = -1;  // total <= max
    int         missiles_min = -1;  // total >= min
    struct CargoReq { std::string commodity; int min_units = 0; };
    std::vector<CargoReq>    cargo;
    bool        has_near_nav = false;
    std::string nav;                // near_nav.nav (by name)
    float       radius_m     = 0.0f;
    std::vector<std::string> requires_flags;   // all must be set
    std::vector<std::string> forbids_flags;    // none may be set
};

// Translate an already-decoded triggers document ({"triggers":[...]}) into
// `out`. NEVER throws; entries without a "cinematic" id are skipped with a
// log line. Returns false + sets `err` only when `root` isn't a JSON object.
// Exposed (rather than buried in load) so the headless test can drive it.
bool parse_triggers(const json::Value& root, std::vector<Trigger>& out,
                    std::string& err);

// Load the trigger table from `path`. Idempotent — clears any prior table
// AND all runtime latches. Missing/unparseable file is NON-fatal: log one
// line, zero triggers (voice::load policy). Called at startup and by the
// /cinematic/reload hook (hot reload).
void load(const std::string& path);

// Clear runtime state only (once-latches + cooldown stamps); the parsed
// table survives. Call at the scripted::reset sites (system load, launch).
void reset();

// Register the host's "fire a cinematic" recipe. main.cpp wires it to
// cinematic::play(id) gated on Flight / no-active-cinematic / not-Dying.
// Returning false = fire refused; the trigger does NOT latch and retries.
void set_play_hook(std::function<bool(const std::string& id)> fn);

// Per-frame evaluation. Fires AT MOST one trigger per tick (through the
// play hook). `now_s` is the same wall-clock scripted::tick uses (cooldown
// bookkeeping). No-op with an empty table — call it unconditionally.
void tick(const TriggerCtx& ctx, float now_s);

// Number of loaded triggers (headless-test / status introspection).
int count();

} // namespace cinematic::triggers

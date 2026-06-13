#pragma once
// -----------------------------------------------------------------------------
// galaxy.h — the universe-as-graph.
//
// One level up from system_def.h. Where a StarSystem is "everything inside one
// star system" (suns, belts, stations, the nav points you target with N), the
// Galaxy is "which systems exist and how they connect". It is the data layer
// the jump mechanic (np-6al.3) rides on, and it deliberately knows NOTHING
// about scenes, GPU resources, or rendering — it is pure data + lookup.
//
// Two halves, both parsed from assets/galaxy.json:
//
//   * systems — a catalog mapping a stable `id` ("troy", "pyrenees") to its
//     display name, sector, a coarse 2D `galaxy_position` for the future jump
//     map, and the `json_path` of the per-system file load_system() already
//     knows how to read. The engine still materialises ONE system's full JSON
//     at a time; this catalog just enumerates what's loadable and where.
//
//   * jumps — a DIRECTED edge list. Each link points from (system, jump nav)
//     to (destination system, arrival nav). Authored in reciprocal pairs so a
//     ship can jump back the way it came. jump_target() answers the question
//     the jump mechanic asks: "the player targeted this jump point — where
//     does it lead, and which gate do they arrive at on the far side?".
//
// Lookup-only API, no mutation after load: neighbors() for the jump map / AI
// route planning, jump_target() for the actual jump, find() / json_path_for()
// for the loader. A missing system or an unsurveyed (dangling) jump point is
// answered with an empty/!ok result, never a throw — calling code treats a
// dead-end gate as "nothing happens", same resilience philosophy as the rest
// of the data loaders.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <string>
#include <vector>

namespace galaxy {

// One node in the graph — a known star system. `id` is the currency every
// other layer references (player.current_system, save files, jump links);
// `json_path` is what load_system() consumes.
struct SystemEntry {
    std::string id;             // "troy"
    std::string display_name;   // "Troy"
    std::string sector;         // "Troy Sector"
    std::string json_path;      // "assets/systems/troy.json"
    HMM_Vec2    galaxy_position{ 0.0f, 0.0f };  // coarse 2D for the jump map
};

// One directed jump edge. `from_nav` names a kind=="jump" NavPointDef in the
// `from` system; `to_nav` names the arrival gate in the `to` system.
struct JumpLink {
    std::string from;       // source system id
    std::string from_nav;   // jump nav name in the source system
    std::string to;         // destination system id
    std::string to_nav;     // arrival nav name in the destination system
};

// Result of a jump lookup. `ok` is false for a nav that isn't a known jump
// edge (an unsurveyed frontier gate, or a typo'd nav name) — callers treat
// that as "this gate goes nowhere yet".
struct JumpTarget {
    bool        ok = false;
    std::string system;     // destination system id
    std::string nav;        // arrival nav name on the far side
};

struct Galaxy {
    std::vector<SystemEntry> systems;
    std::vector<JumpLink>    jumps;

    // System lookup by id. nullptr when unknown.
    const SystemEntry* find(const std::string& id) const;

    // The per-system JSON path for an id, or "" when the id is unknown.
    std::string json_path_for(const std::string& id) const;

    // Every system reachable in one jump from `id` (destination ids,
    // de-duplicated). Empty for an unknown system or a leaf with no links.
    std::vector<std::string> neighbors(const std::string& id) const;

    // Resolve a (current system, jump nav name) to its destination. The
    // heart of the jump mechanic: returns {ok=false} when no edge matches.
    JumpTarget jump_target(const std::string& system,
                           const std::string& jump_nav) const;

    bool empty() const { return systems.empty(); }
};

// Parse assets/galaxy.json into `out`. Returns false (and logs) on IO /
// parse failure or an empty system list, leaving `out` untouched. Unknown
// fields are ignored (forward-compat), matching load_system()'s policy.
bool load(const std::string& path, Galaxy& out);

} // namespace galaxy

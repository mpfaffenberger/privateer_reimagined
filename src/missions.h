#pragma once
// -----------------------------------------------------------------------------
// missions.h — the Mission Computer: generated cargo + mercenary jobs.
//
// The "find work" half of the sandbox loop (np-zte.1). Where economy.h sells
// you goods and outfitting.h sells you a ship, this module hands you JOBS — a
// reason to fly somewhere and a payout for doing it. Six kinds, modelled on
// vanilla Privateer's mission set (MissionType below):
//
//   * Patrol         — scout N non-base nav points in a system.
//   * Scout          — reach a single nav point; enemies may/may not appear.
//   * Attack         — destroy N hostiles at a nav point.
//   * DefendBase     — repel a hostile attack at a base.
//   * Bounty         — destroy N ships of an outlaw faction across current +
//                      adjacent systems.
//   * CargoDelivery  — haul N units of a commodity to a base in another
//                      system. Accepting LOADS the cargo into your hold (so
//                      it eats capacity and a refusal is possible when the
//                      hold is full); delivering at the destination base
//                      pays the reward and removes it.
//
// Bounty progress advances through the SAME player-kill path the reputation
// system rides (np-ma2.1): missions::on_player_kill is called next to
// comm::report_player_kill, so there's one kill-attribution truth, not two.
//
// DATA-DRIVEN, NOT a hardcoded list. generate() enumerates real destination
// bases by reading this system's nav points + each neighbour's (via the
// galaxy graph + load_system), picks real commodities from the catalog, and
// scales rewards by distance (jumps) + cargo value. The numbers are tunable
// constants documented at the top of missions.cpp.
//
// BOARD PERSISTENCE: the board you SEE is regenerated on each dock from a
// seed = hash(base_id) XOR a slow wall-clock window (k_board_refresh_secs),
// so it feels alive between visits but is stable within one sitting and not
// pure per-frame noise. The board itself is transient (cached module state,
// not saved). The missions you ACCEPT live in PlayerState::missions and ARE
// saved (savegame.cpp) — see player.h ActiveMission.
//
// HEADLESS SPLIT: the model (generate / accept / deliver / on_player_kill)
// is pure logic and unit-testable offline; the ImGui screen body is compiled
// only when MISSIONS_HEADLESS is undefined (same pattern as ECONOMY_HEADLESS
// / COMM_HEADLESS), so tools/test_missions.cpp links the real logic without
// the render/audio stack.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

struct PlayerState;
struct ActiveMission;  // forward: full type lives in player.h
struct StarSystem;     // forward: full type lives in system_def.h
enum class Faction : uint8_t;

namespace galaxy { struct Galaxy; }

namespace missions {

// Stable integer keys — mirror ActiveMission::type (player.h). These match
// the vanilla Privateer mission set. Append-only from here on.
enum class MissionType : int {
    Patrol        = 0,   // scout up to N non-base nav points in a system
    Scout         = 1,   // reach a single nav point; enemies may/may not appear
    Attack        = 2,   // destroy N hostiles at a nav point
    DefendBase    = 3,   // repel a hostile attack at a base
    Bounty        = 4,   // hunt a faction target across current + adjacent systems
    CargoDelivery = 5,   // haul a commodity to a base in another system
};

// Where a mission was offered. Affects the type mix and the pay band.
enum class MissionSource : int {
    Computer         = 0,   // Mission Computer (cheapest, all merc-type jobs)
    MercenariesGuild = 1,   // attack/bounty/defend/patrol/scout, top of band
    MerchantsGuild   = 2,   // cargo/bounty, almost always cross-system
};

// Short uppercase label for a mission type — for the Mission Computer UI and
// the headless test. Never nullptr; falls back to "?" for an unknown value.
const char* type_label(MissionType t);      // "PATROL", "SCOUT", "ATTACK", ...

// Display name for a mission source — for the Mission Computer UI and any
// briefing text that calls out where the offer came from. Never nullptr.
const char* source_label(MissionSource s);  // "Mission Computer", ...

// One GENERATED offer on a base's board (not yet accepted). The persistent,
// accepted form is ActiveMission (player.h); convert with to_active().
struct Mission {
    std::string id;                  // unique within the board it came from
    MissionType  type = MissionType::CargoDelivery;   // default for untyped ctors
    MissionSource source = MissionSource::Computer;   // where the offer came from (#7)
    std::string  giver_faction;      // base's faction, display flavour
    std::string  title;              // one-line board entry
    std::string  description;        // longer brief shown on the board
    int64_t      reward = 0;         // credits on completion

    // ---- CargoDelivery payload ----
    std::string commodity_id;        // catalog id of the goods
    int         units = 0;           // units to haul
    std::string origin_base;         // base_id where the job is offered
    std::string dest_system;         // galaxy system id to deliver to
    std::string dest_base;           // base_id at the destination
    std::string dest_base_name;      // display name of the dest base (nav name)
    std::string dest_system_name;    // display name of the dest system

    // ---- Bounty payload ----
    std::string target_faction;      // lowercase faction name to hunt
    int         count_required = 0;  // kills needed

    // ---- Per-type payload (#7) ----
    // Token map (from re/mission_text.json + re/mission_economy.md) — each
    // catalog token maps to exactly one of the fields below. Only the
    // subset meaningful to this Mission's `type` is populated; the rest
    // stay at their default ("" / 0 / empty vector).
    //
    //   $EN   -> target_faction (also reused for non-bounty enemies)
    //   $DB   -> dest_base (Cargo) | target_base (DefendBase, NEW)
    //   $DS   -> dest_system (Cargo) | target_system (non-cargo, NEW)
    //   $NN   -> nav_count        (Patrol count of nav points, NEW)
    //   $DN,$DN1,$DN2 -> nav_targets (NEW, vector of nav-point ids)
    //   $DO   -> bounty_region    (Bounty search region, NEW)
    //   $D1   -> last_seen_system (Bounty last-confirmed sighting, NEW)
    //   $D2   -> last_seen_alt_system (Bounty alternate last-seen, NEW)
    //   $CG   -> commodity_id     (Cargo only)
    //   $PY   -> reward
    //
    // "Append don't repurpose" — dest_base / dest_system / target_faction
    // / count_required are kept above and not renamed. The new fields are
    // for the four new types + the new bounty metadata. Heavy types like
    // HMM_Vec3 stay OUT — the live nav/world positions get resolved
    // lazily by #13 against the ids stored here.
    std::string               target_system;        // $DS for non-cargo
    std::vector<std::string>  nav_targets;          // $DN / $DN1 / $DN2 nav points
    int                       nav_count       = 0;  // $NN (Patrol)
    int                       hostiles_required = 0;// Attack kill count (parallel to count_required)
    std::string               target_base;          // $DB for DefendBase (base under attack)
    std::vector<std::string>  bounty_region;        // $DO for Bounty (systems in hunt region)
    std::string               last_seen_system;     // $D1 for Bounty
    std::string               last_seen_alt_system; // $D2 for Bounty
};

// ---- generation -------------------------------------------------------------

// Deterministically generate a board of plausible missions for `base_id` in
// `system_id`, using the galaxy graph to find reachable destination bases.
// `seed` makes it reproducible (tests pass a fixed value). `source` gates
// the eligible mission types (Computer = all six, MercGuild = non-cargo,
// MerchGuild = cargo + bounty). Pure: no global state touched, no UI, no
// audio. Returns a handful of mixed-type missions (5-8); empty only when
// the catalogs aren't loaded. The default `source=Computer` keeps the
// original one-board call sites compiling unchanged.
std::vector<Mission> generate(const std::string& base_id,
                              const std::string& system_id,
                              const galaxy::Galaxy& g,
                              uint64_t seed,
                              MissionSource source = MissionSource::Computer);

// Live wrapper: compute the slow-clock seed for `base_id`, then generate and
// cache ALL THREE source boards (Computer / Mercenaries / Merchants) for it,
// each off a source-folded seed so the three screens show distinct mixes.
// Call once per dock. Logs a one-line summary + each generated mission.
void generate_board(const std::string& base_id,
                    const std::string& system_id,
                    const galaxy::Galaxy& g);

// The cached board for `source` from the last generate_board() (what the
// matching screen shows). Defaults to the Mission Computer board so the
// original no-arg call sites keep compiling.
const std::vector<Mission>& board(MissionSource source = MissionSource::Computer);

// ---- accept / complete (all credit/cargo moves via player:: helpers) --------

// Can this offer be accepted right now? For CargoDelivery this is a hold-
// space check (units must fit in `capacity` - used); bounties always can.
bool can_accept(const PlayerState& p, const Mission& m, int capacity);

// Accept `m`: append an ActiveMission to the player and, for a delivery,
// LOAD the cargo into the hold (player::add_cargo). Returns false (no
// mutation) when can_accept() fails. Logs the outcome.
bool accept(PlayerState& p, const Mission& m, int capacity);

// Deliver the active CargoDelivery mission `mission_id` at base `at_base`:
// validates the destination matches, removes the hauled cargo, pays the
// reward (player::add_credits), drops the mission, and pushes a comm line.
// Returns false when no such deliverable mission exists here.
bool complete_delivery(PlayerState& p, const std::string& mission_id,
                       const std::string& at_base);

// Optional: abandon an active mission (any type). For a delivery this also
// jettisons the hauled cargo. Returns false if the id isn't active.
bool abandon(PlayerState& p, const std::string& mission_id);

// ---- in-flight progress + generic completion (the #13 tracker seam) --------
//
// #13 owns the live world tracking; it just flips reach/clear state on the
// ActiveMission and asks the model to settle the payout. These two calls are
// that seam — pure state mutation + a per-type objectives check, no UI.

// ---- status / panel seam (Navmap mission status, np-19.3) -----------------
//
// Shortest hop count from `from_sys` to `to_sys` over `g`. -1 = unreachable,
// 0 = same system. Dijkstra-style min-heap so a future weighted graph is a
// drop-in (today every edge is unit weight — the queue degenerates into
// BFS). Capped at k_max_hops to bound work on an adversarial universe; 16
// is well above the current 11-hop diameter of the real assets/galaxy.json.
// Public so the navmap mission-status panel (np-19.3) can pick a jump nav
// on the shortest path toward a cross-system cargo/bounty target.
int hops_between(const galaxy::Galaxy& g,
                 const std::string& from_sys,
                 const std::string& to_sys);
//
// One place where the per-type mission status string is built so the live
// in-flight readout (#18) and the navmap mission-status panel (this issue)
// can't drift. Pure: read-only against PlayerState::missions, the active
// system (for base-name lookup), and current_system_id. No ImGui, no
// audio — test_missions links this against MISSIONS_HEADLESS.
struct MissionStatus {
    std::string text;            // formatted one-liner, e.g. "PATROL 2/5"
    std::string target_system;   // display name of destination system;
                                  // non-empty ONLY for cross-system rows
                                  // (Cargo into another system, Bounty
                                  // with no in-system nav). Empty for
                                  // any row whose objective lives in the
                                  // current system.
    bool        in_current_system = false;
};

MissionStatus mission_status(const ActiveMission& am,
                             const StarSystem& sys,
                             const std::string& current_system_id);

// Mark nav target `nav_index` of active mission `mission_id` as reached
// (Scout / Patrol / Attack visit-a-nav objectives). Idempotent. Returns
// false when no such active mission or the index is out of range.
bool mark_nav_reached(PlayerState& p, const std::string& mission_id,
                      size_t nav_index);

// Settle an objective-driven mission whose objectives are now met: pay the
// reward (player::add_credits), drop it from the active list, and push a comm
// line — the SAME payout path as the cargo/bounty completions. Handles
// Scout (single nav done), Patrol (all navs done), Attack & DefendBase
// (progress >= hostiles_required). CargoDelivery settles via
// complete_delivery() and Bounty via on_player_kill(), so for those this is
// a no-op returning false. Returns true iff the mission completed + dropped.
bool complete_if_objectives_met(PlayerState& p, const std::string& mission_id);

// ---- kill progress (wired into the np-ma2.1 player-kill path) --------------

// The player just destroyed a `victim`-faction ship in `current_system`.
// Two kinds of missions advance here, both off the one kill-attribution
// truth (called right beside comm::report_player_kill):
//
//   * Bounty  — every active bounty whose target faction matches AND whose
//     hunt region (`bounty_region`) contains `current_system` (an empty
//     region falls back to "any system" for safety). Auto-pay + drop any that
//     complete (via the shared pay_and_drop helper) and push comm lines.
//
//   * Attack / DefendBase — every active mission whose target faction
//     matches AND whose `target_system == current_system`. Advance progress
//     per-kill and push a comm line ("Attack: k/n" / "Defend: k/n"), but do
//     NOT settle inline: the next mission_tracker::tick() runs
//     complete_if_objectives_met() (every frame for attack/defend), keeping
//     a single completion path and avoiding erase-mid-iteration hazards.
//     (The mission force spawns outside the tracker's 6km nav bubble, so the
//     spawned hostiles are what the player actually kills — counting those
//     per-kill is what lets attack/defend ever complete.)
//
// Returns the number of missions that advanced. Called right beside
// comm::report_player_kill so kill attribution stays single-sourced.
int on_player_kill(PlayerState& p, Faction victim,
                   const std::string& current_system);

// ---- screen (np-9cu.4 hook seam) --------------------------------------------

// Register the Mission Computer screen body with the base-screen framework.
// Compiled only in the live game (see MISSIONS_HEADLESS in the header note).
void register_screen();

} // namespace missions

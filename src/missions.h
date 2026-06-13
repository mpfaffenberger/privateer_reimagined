#pragma once
// -----------------------------------------------------------------------------
// missions.h — the Mission Computer: generated cargo-delivery + bounty jobs.
//
// The "find work" half of the sandbox loop (np-zte.1). Where economy.h sells
// you goods and outfitting.h sells you a ship, this module hands you JOBS — a
// reason to fly somewhere and a payout for doing it. Two kinds for v1:
//
//   * CargoDelivery — haul N units of a commodity to a base in this or a
//     neighbouring system. Accepting LOADS the cargo into your hold (so it
//     eats capacity and a refusal is possible when the hold is full);
//     delivering at the destination base pays the reward and removes it.
//
//   * Bounty — destroy N ships of an outlaw faction (pirate / retro /
//     kilrathi — derived from faction baselines, never hardcoded). Progress
//     advances through the SAME player-kill path the reputation system rides
//     (np-ma2.1): missions::on_player_kill is called next to
//     comm::report_player_kill, so there's one kill-attribution truth, not
//     two. Completing auto-pays + notifies on the qualifying kill.
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
enum class Faction : uint8_t;

namespace galaxy { struct Galaxy; }

namespace missions {

// Stable integer keys — mirror ActiveMission::type (player.h). Never reorder.
enum class MissionType : int { CargoDelivery = 0, Bounty = 1 };

// One GENERATED offer on a base's board (not yet accepted). The persistent,
// accepted form is ActiveMission (player.h); convert with to_active().
struct Mission {
    std::string id;                  // unique within the board it came from
    MissionType type = MissionType::CargoDelivery;
    std::string giver_faction;       // base's faction, display flavour
    std::string title;               // one-line board entry
    std::string description;         // longer brief shown on the board
    int64_t     reward = 0;          // credits on completion

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
};

// ---- generation -------------------------------------------------------------

// Deterministically generate a board of plausible missions for `base_id` in
// `system_id`, using the galaxy graph to find reachable destination bases.
// `seed` makes it reproducible (tests pass a fixed value). Pure: no global
// state touched, no UI, no audio. Returns a handful (cargo + bounty) of
// Missions; empty only when the catalogs aren't loaded.
std::vector<Mission> generate(const std::string& base_id,
                              const std::string& system_id,
                              const galaxy::Galaxy& g,
                              uint64_t seed);

// Live wrapper: compute the slow-clock seed for `base_id`, call generate(),
// and cache the result as the current board (board()). Call once per dock.
// Logs a one-line summary + each generated mission.
void generate_board(const std::string& base_id,
                    const std::string& system_id,
                    const galaxy::Galaxy& g);

// The cached board from the last generate_board() (what the screen shows).
const std::vector<Mission>& board();

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

// ---- bounty progress (wired into the np-ma2.1 player-kill path) -------------

// The player just destroyed a `victim`-faction ship. Advance every active
// bounty whose target matches, auto-pay + drop any that complete, and push
// comm lines. Returns the number of bounties that advanced. Called right
// beside comm::report_player_kill so kill attribution stays single-sourced.
int on_player_kill(PlayerState& p, Faction victim);

// ---- screen (np-9cu.4 hook seam) --------------------------------------------

// Register the Mission Computer screen body with the base-screen framework.
// Compiled only in the live game (see MISSIONS_HEADLESS in the header note).
void register_screen();

} // namespace missions

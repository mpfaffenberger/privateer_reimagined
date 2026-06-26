// -----------------------------------------------------------------------------
// hailing.cpp — contraband search director (Phase 1). See header for design.
// -----------------------------------------------------------------------------

#include "hailing.h"

#include "comm.h"
#include "voice.h"
#include "player.h"      // player::carrying_contraband
#include "faction.h"     // faction::to_name
#include "ship.h"        // Ship + ShipAIState

#include <random>
#include <unordered_map>
#include <cstdio>
#include <string>

namespace hailing {

namespace {

// ---- tuning -----------------------------------------------------------------
// All "named" per the Phase 1 spec (issues #50..#56). Kept file-static:
// hailing::tick() is a singleton director; nothing outside cares.
constexpr float k_search_chance      = 0.35f;   // 35% search-init roll
constexpr float k_comms_range_m      = 6000.0f; // ~6 km hail envelope
constexpr float k_search_delay_s     = 2.5f;    // scan-in-progress duration
constexpr float k_global_cooldown_s  = 18.0f;   // min s between any searches

// ---- per-NPC interaction state ---------------------------------------------
enum class Phase : uint8_t {
    Idle,        // not yet tried to scan this NPC
    Searching,   // scan in flight; awaiting timer + player outcome
    Resolved,    // scan finished (or skipped) — leave the NPC alone for now
};

struct NpcState {
    Phase phase       = Phase::Idle;
    float search_start = 0.0f;   // wall-clock when Searching started
};

// Patched together by (ship_id, phase).  ship_id is Ship::id (monotonic
// uint32, never 0 except for "none"). Phase transitions stay linear:
// Idle -> Searching -> Resolved. Resolved never reverts — that's the
// whole point: a single scan per encounter, capped by the global
// cooldown for the player's stay in the system.
std::unordered_map<uint32_t, NpcState> g_state;

// Wall-clock of the last search initiation (Idle -> Searching). Used to
// pace `k_global_cooldown_s` so the whole encounter director doesn't
// spam searches across many NPCs at once. Initialised far in the past
// so the very first roll is allowed.
float g_last_search_s = -1000.0f;

// Deterministic RNG, same pattern as comm.cpp. Search rolls are cosmetic
// flavour so a fixed seed buys reproducibility across runs (debug
// asserts about "did the hail fire this time" stay stable).
std::mt19937& rng() {
    static std::mt19937 r{0xC0FFEEu ^ 0x4D1A1u};   // distinct seed
    return r;
}

// Display name for the feed. Prefer the polished faction word over the
// raw lowercase catalogue id (faction::to_name returns "militia" — feed
// text wants "Militia"). Falls back to faction::to_name for any other
// faction we might extend this director to later.
const char* display_name(Faction f) {
    switch (f) {
        case Faction::Militia: return "Militia";
        case Faction::Confed:  return "Confederation";
        default:               return faction::to_name(f);
    }
}

// Format "Militia: ..."-style feed text into comm::push. Centralises the
// `<name>: <body>` join so every hailing branch reads identically.
void feed(Faction f, const char* body, bool taunt = true) {
    char line[256];
    std::snprintf(line, sizeof(line), "%s: %s",
                  display_name(f), body);
    comm::push(std::string(line), taunt);
}

} // namespace

void reset() {
    g_state.clear();
    // Reset the cooldown too: a fresh system/landing = fresh director,
    // no carry-over from the previous encounter.
    g_last_search_s = -1000.0f;
}

void tick(ShipRegistry& ships, const Ship& player_ship,
          const PlayerState& player, float now_s)
{
    // ---- main pass -----------------------------------------------------
    for (Ship& s : ships) {
        if (!s.alive)              continue;   // dead NPCs don't hail
        if (s.is_player)           continue;   // never the player
        // Only the lawful factions run contraband scans. Pirates, Retro,
        // Kilrathi etc. don't care (and probably wouldn't bother with
        // the paperwork).
        if (s.faction != Faction::Militia && s.faction != Faction::Confed)
            continue;

        // Already hostile -> don't try to scan. The director was here
        // before; once we're committed to a fight there's nothing
        // useful for the search to say. Mark Resolved so we don't keep
        // poking at the entry each frame.
        if (s.ai.aggro_player || s.provoked_by_player) {
            g_state[s.id].phase = Phase::Resolved;
            continue;
        }

        NpcState& ns = g_state[s.id];
        const float dist = HMM_LenV3(HMM_SubV3(s.position, player_ship.position));

        switch (ns.phase) {
        case Phase::Idle: {
            // Two preconditions for a roll: in comms range AND the
            // global cooldown has elapsed. Both gates are cheap; the
            // distance test is the heavy one.
            if (dist > k_comms_range_m) break;
            if ((now_s - g_last_search_s) < k_global_cooldown_s) break;

            // 35% chance to start a scan. Roll a uniform float and
            // gate on `k_search_chance` — anything below the threshold
            // is a hit, anything at or above is a miss.
            std::uniform_real_distribution<float> pick(0.0f, 1.0f);
            const float r = pick(rng());
            if (r >= k_search_chance) {
                // MISS — the NPC chose not to scan; mark Resolved so
                // we don't roll again for this NPC.
                ns.phase = Phase::Resolved;
                break;
            }

            // HIT — commit to the scan. Lock the global cooldown so
            // other NPCs in range don't immediately follow suit.
            ns.phase       = Phase::Searching;
            ns.search_start = now_s;
            g_last_search_s  = now_s;

            feed(s.faction, "Cut your engines for a contraband scan.");
            voice::say(s.faction, voice::Category::Search,
                       s.position, /*to_player=*/true);
            break;
        }

        case Phase::Searching: {
            // Player fled the scan envelope -> treat as fleeing a
            // lawful stop. Same hostile path as contraband.
            if (dist > k_comms_range_m) {
                s.ai.aggro_player = true;
                feed(s.faction, "Fleeing a lawful scan? Hostile!");
                voice::say(s.faction, voice::Category::Hostile,
                           s.position, /*to_player=*/true);
                ns.phase = Phase::Resolved;
                break;
            }

            // Scan timer expired -> verdict time. Contraband goes
            // hostile (same path as fleeing); clean gets the radio
            // all-clear.
            if ((now_s - ns.search_start) < k_search_delay_s) break;

            if (player::carrying_contraband(player)) {
                s.ai.aggro_player = true;
                feed(s.faction,
                     "Contraband detected. Prepare to be destroyed.");
                voice::say(s.faction, voice::Category::Hostile,
                           s.position, /*to_player=*/true);
            } else {
                feed(s.faction,
                     "Scan complete. You're clean, safe travels.");
                voice::say(s.faction, voice::Category::Clear,
                           s.position, /*to_player=*/true);
            }
            ns.phase = Phase::Resolved;
            break;
        }

        case Phase::Resolved:
            // Nothing this frame. The NPC's been scanned / skipped.
            break;
        }
    }

    // ---- prune pass ----------------------------------------------------
    // Drop state entries for ships no longer alive in the registry.
    // find_by_id returns nullptr if the slot was despawned (the
    // generation bumped); alive=false covers ships that died in-world.
    // Keep it cheap — at the demo's ship count the map is tiny.
    for (auto it = g_state.begin(); it != g_state.end(); ) {
        Ship* s = ships.find_by_id(it->first);
        if (!s || !s->alive) {
            it = g_state.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace hailing

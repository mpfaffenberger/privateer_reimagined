#pragma once
// -----------------------------------------------------------------------------
// player.h — everything the player OWNS and everywhere the player IS.
//
// The persistent half of the player, distinct from the transient half:
// the Ship instance in the registry (hull/shield state, this frame's
// controller inputs) is rebuilt every session, while PlayerState is the
// part a save file would serialize — credits, cargo, the ship you own
// and what's bolted to it, how every faction feels about you, and where
// you are. Three downstream features stand on this struct: commodity
// trading (np-9cu.2) mutates credits + cargo, the ship dealer
// (np-9cu.3) swaps ship_class_name + equipment, and save/load
// (np-ymp.1) serializes the whole thing.
//
// Representation philosophy: strings and ints, no pointers. The shops
// interpret "tarsus" / "meson_blaster" / shield level 2 against their
// catalogs at point of use; PlayerState itself never holds a ShipClass*
// or GunStats* that could dangle across a data reload or deserialize
// stale. The one lookup helper that needs class data
// (cargo_capacity) takes the ShipClass* as a parameter for exactly
// this reason.
//
// Cargo manifest entries remember bought_at_price — the average paid
// per unit, blended on re-buys — so the trading screen can show the
// classic "you paid 35, it sells for 52 here" profit readout without a
// transaction-history sidecar.
//
// PlayerReputation (the type) stays in faction.h where the stance
// machinery lives; the INSTANCE moves here from AppState because rep
// is owned-by-the-player, persistent state. perception::tick keeps
// taking `const PlayerReputation&` — it doesn't care where it lives.
// -----------------------------------------------------------------------------

#include "faction.h"

#include <cstdint>
#include <string>
#include <vector>

struct ShipClass;

// One stack of one commodity in the hold.
struct CargoEntry {
    std::string commodity_id;        // catalog id, e.g. "iron" (commodity.h)
    int         units           = 0;
    int         bought_at_price = 0; // average credits/unit paid (see header)
};

// One ACCEPTED mission, with live progress — the persistent half of the
// mission system (missions.h owns the generated OFFERS; this is what the
// player actually took on and what save/load round-trips). Kept as a flat
// strings-and-ints POD here, deliberately NOT including missions.h, to
// preserve player.h's "no upstream deps, no pointers, serializes clean"
// discipline. missions.cpp converts between Mission and ActiveMission.
//
// `type` is a STABLE integer key (0 = CargoDelivery, 1 = Bounty), mirrored
// by missions::MissionType — an int rather than the enum so the save format
// (savegame.cpp) and this struct never depend on the enum's declaration
// order. Only the fields relevant to the mission's `type` are meaningful.
struct ActiveMission {
    std::string id;                  // stable id within its origin board
    int         type           = 0;  // 0 = CargoDelivery, 1 = Bounty
    std::string giver_faction;       // display flavour, e.g. "Confederation"
    std::string title;               // pre-formatted one-liner for the board
    int64_t     reward         = 0;  // credits paid on completion

    // ---- CargoDelivery payload ----
    std::string commodity_id;        // catalog id of the hauled goods
    int         units          = 0;  // units loaded into the hold on accept
    std::string dest_system;         // galaxy system id to deliver to
    std::string dest_base;           // base_id at the destination

    // ---- Bounty payload ----
    std::string target_faction;      // lowercase faction name to hunt
    int         count_required = 0;  // kills needed
    int         progress       = 0;  // kills landed so far (<= count_required)
};

struct PlayerState {
    // ---- wealth ---------------------------------------------------------
    // int64 on purpose: a Galaxy hold (225 units) of high-value goods
    // plus a long career's earnings overflows int32 without trying.
    int64_t credits = 0;

    // ---- reputation -----------------------------------------------------
    PlayerReputation rep;            // zeroed = unknown stranger (see faction.h)

    // ---- owned ship + fitted equipment -----------------------------------
    // All by-name/by-level; shops resolve against catalogs at point of
    // use (see header). gun_mounts[i] is the gun name in mount slot i
    // ("meson_blaster", matching gun::from_name's vocabulary); "" =
    // empty slot. Levels are 0-based shop tiers the equipment dealer
    // will define; 0 = stock.
    std::string              ship_class_name;       // "tarsus"
    std::vector<std::string> gun_mounts;
    int                      shield_level    = 0;
    int                      engine_level    = 0;
    bool                     cargo_expansion = false;

    // ---- cargo hold -------------------------------------------------------
    std::vector<CargoEntry> cargo;

    // ---- ordnance: finite missile ammo (np-zte.2) -------------------------
    // Unlike guns (energy-limited but never "out"), missiles are consumable.
    // Indexed by MissileType (DF/HS/IR — missile.h); one fired = one gone,
    // restocked at a base. Kept as a flat array (not a missile.h include) to
    // preserve player.h's "strings + ints, no upstream deps" discipline —
    // the index meaning is the stable contract, mirrored by MissileType.
    int missiles[3] = { 0, 0, 0 };

    // ---- afterburner fuel (np-zte.2) --------------------------------------
    // A finite resource the cruise/afterburner engine (camera.cpp's cruise
    // system — there is no SEPARATE afterburner) drains while engaged.
    // Regenerates slowly when not afterburning and refuels at a base. At
    // zero the engine cuts out until it recovers. Persisted so a half-empty
    // tank survives save/load. See the tuning constants below.
    float afterburner_fuel = 0.0f;

    // ---- accepted missions ------------------------------------------------
    // Missions the player has taken on (mission computer, np-zte.1). Cargo
    // deliveries also occupy cargo slots above; bounties carry a kill
    // counter. Serialized by savegame.cpp with stable string keys so the
    // board you accepted survives a save/load.
    std::vector<ActiveMission> missions;

    // ---- location ---------------------------------------------------------
    std::string current_system;      // "troy" (assets/systems/<name>.json)
    std::string last_docked_base;    // "" until first landing
    bool        docked = false;
};

namespace player {

// The canonical Privateer start: a Tarsus, a modest bankroll, the Troy
// system, an empty hold. Gun loadout mirrors the hardcoded player
// mounts in main.cpp (2x meson blaster) so the data and the spawned
// Ship agree from day one. `start_system` lets the --system CLI
// override flow through.
PlayerState new_game(const std::string& start_system);

// Starting credits. Named constant because the exact number is a
// gameplay-tuning knob, not a fact — the 1995 game handed you a small
// bankroll barely covering one cargo run, which is the feel we want.
constexpr int64_t k_new_game_credits = 2000;

// ---- afterburner-fuel tuning (np-zte.2) ------------------------------------
// All seconds-based so they read directly: a full tank gives
// k_afterburner_fuel_max / k_afterburner_drain_per_s seconds of cruise
// (100 / 25 = 4s burst), and refills from empty over
// k_afterburner_fuel_max / k_afterburner_regen_per_s seconds of NOT cruising
// (100 / 12 ≈ 8.3s). Drain > regen on purpose — afterburner is a sprint,
// not a cruise speed you hold forever. Tweak these to taste; they're the
// only knobs the feature exposes.
constexpr float k_afterburner_fuel_max   = 100.0f;  // full tank
constexpr float k_afterburner_drain_per_s = 25.0f;  // burned while afterburning
constexpr float k_afterburner_regen_per_s = 12.0f;  // recovered while not

// New-game / new-hull starting missile loadout, indexed by MissileType
// (DF/HS/IR). A handful of each so the player has ordnance to learn the
// system with; the equipment dealer (future) and base rearm restock it.
constexpr int k_new_game_missiles[3] = { 4, 2, 2 };

// ---- credits ------------------------------------------------------------
// spend() refuses (returns false, no mutation) when funds are short.
// add() clamps at int64 max rather than wrapping — nobody's earning
// 9.2 quintillion credits legitimately, but a buggy price table
// shouldn't corrupt a save.
bool can_afford(const PlayerState& p, int64_t cost);
bool spend_credits(PlayerState& p, int64_t cost);
void add_credits(PlayerState& p, int64_t amount);

// ---- cargo --------------------------------------------------------------
// Capacity comes from the ShipClass (resolved by the caller — see the
// no-pointers note in the header) plus the expansion flag. The +25%
// expansion factor matches the original game's Cargo Expansion upgrade.
int cargo_units_used(const PlayerState& p);
int cargo_capacity(const PlayerState& p, const ShipClass* klass);

// add_cargo merges into an existing stack (blending bought_at_price by
// unit-weighted average) or appends a new one. Fails (false, no
// mutation) when units <= 0 or the hold would overflow capacity.
// remove_cargo fails when the manifest holds fewer units than asked;
// a stack emptied to zero is erased from the manifest.
bool add_cargo(PlayerState& p, const std::string& commodity_id,
               int units, int price_per_unit, int capacity);
bool remove_cargo(PlayerState& p, const std::string& commodity_id, int units);

// ---- ordnance (np-zte.2) ----------------------------------------------------
// type_index is a MissileType (0=DF,1=HS,2=IR); out-of-range is a no-op.
// missile_count reads the stock; consume_missile decrements one and returns
// true, or false (no mutation) when the rack is empty — the caller turns
// that into the out-of-ammo click. add_missiles tops up (clamped ≥ 0).
int  missile_count(const PlayerState& p, int type_index);
bool consume_missile(PlayerState& p, int type_index);
void add_missiles(PlayerState& p, int type_index, int count);

// ---- afterburner fuel (np-zte.2) -------------------------------------------
// Clamped helpers so call sites never have to remember the [0,max] bounds.
// drain returns the fuel actually consumed (which can be less than the ask
// when the tank runs dry mid-step) so the caller can tell when it bottomed
// out. regen tops up toward max. refuel_full sets it to max (base service).
float drain_afterburner(PlayerState& p, float amount);
void  regen_afterburner(PlayerState& p, float amount);
void  refuel_full(PlayerState& p);

} // namespace player

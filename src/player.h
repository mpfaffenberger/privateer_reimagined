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
#include "inventory.h"

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
// `type` is a STABLE integer key mirroring missions::MissionType — an int
// rather than the enum so the save format (savegame.cpp) and this struct
// never depend on the enum's declaration order. The vanilla Privateer set
// (see missions.h) covers Patrol / Scout / Attack / DefendBase / Bounty /
// CargoDelivery. Only the fields relevant to the mission's `type` are
// meaningful.
struct ActiveMission {
    std::string id;                  // stable id within its origin board
    int         type           = 0;  // missions::MissionType value
    int         source         = 0;  // missions::MissionSource value (#7, save-stable int)
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

    // ---- Per-type payload (#7) — mirrors Mission:: in missions.h ----
    // Only the subset meaningful to this ActiveMission's `type` is set;
    // the rest stay at default ("" / 0 / empty vector). All string/int/
    // vector<string> by design — NO HMM_Vec3 / heavy includes — so #8's
    // save format stays a simple field dump. Live nav positions and world
    // resolution are #13's job; this struct carries only ids.
    std::string               target_system;        // $DS for non-cargo (action system)
    std::vector<std::string>  nav_targets;          // $DN / $DN1 / $DN2 nav points
    int                       nav_count        = 0; // $NN (Patrol)
    int                       hostiles_required = 0;// Attack kill count (parallel to count_required)
    std::string               target_base;          // $DB for DefendBase (base under attack)
    std::vector<std::string>  bounty_region;        // $DO for Bounty (systems in hunt region)
    std::string               last_seen_system;     // $D1 for Bounty
    std::string               last_seen_alt_system; // $D2 for Bounty

    // ---- live in-flight progress (#12 / set by the #13 tracker) ----
    // Per-nav "reached" flags, sized to nav_targets on accept() and flipped
    // to 1 as the player visits each nav point. Scout completes when its
    // single entry is 1; Patrol when ALL are 1. Attack/DefendBase ride the
    // shared `progress` counter against `hostiles_required` instead. Kept a
    // plain uint8 vector (no enum/heavy include) so it serializes clean.
    std::vector<uint8_t>      nav_done;
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
    // Name from armor::find(), e.g. "Plasteel Armor" / "Tungsten Armor".
    // Empty means NO armor package fitted (base hull cm only) — armor is a
    // purchasable upgrade, so new games / fresh hulls start empty.
    std::string              armor_name;
    bool                     cargo_expansion = false;

    // Discrete buy-once-per-ship flags (np-3dp.27). New games have ALL
    // false, so the starter Tarsus can't jump, has no tractor, etc. until
    // you buy them at the dealer. Item names match equipment_prices.json's
    // discrete_equipment map (jump_drive, ecm_l1..l3, repair_droid,
    // adv_repair_droid, tractor_beam). The runtime effect of each flag
    // lives at the place it's checked: jump.cpp gates on jump_drive,
    // missile::lock accepts ecm_lN for a break chance, tractor.cpp on
    // tractor_beam, etc.
    bool                     has_jump_drive    = false;
    int                      ecm_level         = 0;   // 0 none / 1..3 ECM L1..L3
    bool                     has_repair_droid  = false;  // bought the base droid
    bool                     adv_repair_droid  = false;  // upgraded to advanced (2x)
    bool                     has_tractor_beam  = false;

    // ---- guild memberships (#16) -----------------------------------------
    // One-time paid memberships that unlock a guild's mission board at any
    // base hosting that guild's computer. New games start in neither — you
    // pay the join fee (below) the first time you walk into the screen.
    // The board bodies themselves are #17; this flag just gates entry.
    bool                     merc_guild_member     = false;  // Mercenaries' Guild
    bool                     merchant_guild_member = false;  // Merchants' Guild

    // ---- cargo hold -------------------------------------------------------
    std::vector<CargoEntry> cargo;

    // ---- unified-hold items (Phase 4 Wave 1, #80 + #81) ------------------
    // Discrete things dropped, bought, or salvaged into the hold. Each
    // InventoryItem contributes qty cargo spaces to the unified capacity
    // (see cargo_units_used), on top of the commodity stacks in `cargo`
    // above. Weapons and upgrades carry their rarity + per-shot mods
    // through mounting; salvage/commodity-kind items MAY stack when their
    // id+rarity+kind matches an existing entry (add_item merges those).
    std::vector<inventory::InventoryItem> items;

    // ---- ordnance: finite missile ammo (np-zte.2) -------------------------
    // Unlike guns (energy-limited but never "out"), missiles are consumable.
    // Indexed by MissileType (DF/HS/IR — missile.h); one fired = one gone,
    // restocked at a base. Kept as a flat array (not a missile.h include) to
    // preserve player.h's "strings + ints, no upstream deps" discipline —
    // the index meaning is the stable contract, mirrored by MissileType.
    int missiles[3]  = { 0, 0, 0 };

    // Torpedo rack (separate physical launcher on the hull). Just one
    // canonical ammo type -- Proton Torpedo. Indexing by DF/HS/IR is gone;
    // the rack holds a flat count of 35-cr torpedoes.
    int torpedoes   = 0;

    // Hardware ownership for the launcher slots themselves (np-zte.2,
    // np-launchers-bump). Modelled as four discrete physical hardpoints
    // — left/right on each side, one for missiles and one for torpedoes
    // — so the player can buy/sell either side independently of the
    // other. The Tarsus starts the run with one missile launcher (the
    // left hardpoint), and neither torpedo launcher. Total hardware
    // count is the sum of the four flags; capacity is count * 10.
    bool missile_launcher_left  = true;  // Tarsus starter: one launcher
    bool missile_launcher_right = false; // second slot, can be bought
    bool torpedo_launcher_left  = false; // not in Tarsus starter
    bool torpedo_launcher_right = false; // not in Tarsus starter

    // ---- afterburner fuel (np-zte.2) -- MERGED INTO MAIN ENERGY POOL ----
    // The separate `afterburner_fuel` float is gone. The cruise/afterburner
    // engine now drains from the player Ship's `energy_gj` (the same pool
    // the guns spend from), and energy regen (firing.cpp) is the only
    // refill source — holding TAB burns the bank, releasing lets it
    // recharge. Drain rate constant below is the only knob still relevant.
    // Fewer state fields, one bar in the HUD, one tactical decision
    // ("burst speed or burst fire?"). No base refuel service needed.

    // ---- accepted missions ------------------------------------------------
    // Missions the player has taken on (mission computer, np-zte.1). Cargo
    // deliveries also occupy cargo slots above; bounties carry a kill
    // counter. Serialized by savegame.cpp with stable string keys so the
    // board you accepted survives a save/load.
    std::vector<ActiveMission> missions;

    // ---- career stats: kills per faction (np-3dp.19) ----------------------
    // Total ships the player has personally destroyed, bucketed by the
    // victim's faction. Indexed by Faction; serialized by stable faction
    // NAME (like rep) so an enum reorder never scrambles old saves.
    // Purely a record/scoreboard today; missions track their own progress.
    int64_t faction_kills[kFactionCount] = {};

    // ---- live ship damage snapshot (np-3dp.19) ---------------------------
    // The player Ship's per-facing armor/shield + energy reserve, mirrored
    // here each frame so a save captures the hull's CURRENT condition (land
    // damaged -> reload damaged). hp_valid=false means "no snapshot yet"
    // (fresh new_game) -> the spawned ship stays at the full health
    // heal_to_full gives it. Units: cm-of-durasteel (armor/shield), GJ
    // (energy) — same as Ship.
    bool  hp_valid       = false;
    float hp_armor_fore  = 0.0f;
    float hp_armor_aft   = 0.0f;
    float hp_armor_port  = 0.0f;
    float hp_armor_starboard = 0.0f;
    float hp_shield_fore = 0.0f;
    float hp_shield_aft  = 0.0f;
    float hp_shield_port = 0.0f;
    float hp_shield_starboard = 0.0f;
    float hp_energy      = 0.0f;

    // ---- location ---------------------------------------------------------
    std::string current_system;      // "troy" (assets/systems/<name>.json)
    std::string last_docked_base;    // "" until first landing
    bool        docked = false;
};

namespace player {

// The canonical Privateer start (np-3dp.25): a stock Tarsus fitted with a
// SINGLE laser cannon + a single launcher of 4 heat-seekers, a modest
// bankroll, an empty hold, docked at Achilles Mining Base in Troy. The
// spawn path (main.cpp apply_player_loadout) fits these onto the live
// Ship, so this data is the single source of truth. `start_system` lets
// the --system CLI override the recorded location.
PlayerState new_game(const std::string& start_system);

// Starting credits. Named constant because the exact number is a
// gameplay-tuning knob, not a fact — the 1995 game handed you a small
// bankroll barely covering one cargo run, which is the feel we want.
constexpr int64_t k_new_game_credits = 2000;

// ---- guild join fees (#16) ----------------------------------------------
// One-time membership dues, deducted via spend_credits() the first time the
// player enters a guild screen and accepts. Values are the vanilla Privateer
// numbers (FAQ): the Mercenaries' Guild is the pricier combat board, the
// Merchants' Guild the cheaper trade board. spend_credits refuses the join
// (and the screen says so) if the player is short.
constexpr int64_t k_merc_guild_fee     = 5000;
constexpr int64_t k_merchant_guild_fee = 1000;

// ---- afterburner energy drain (np-zte.2 / merged pool) ---------------------
// Afterburner now spends from the player Ship's energy_gj (shared with the
// guns). Drain rate matches the Tarsus's 50 GJ/s recharge so holding TAB
// exactly cancels regen — sustained afterburn parks energy at zero (no
// gun shots until you let off the boost), short bursts pop in and out as
// the bank recovers. No separate fuel tank, no separate regen rate, no
// base refuel service — simpler model, single resource decision.
constexpr float k_afterburner_drain_per_s = 50.0f;  // GJ/s drained from energy_gj

// New-game / new-hull starting missile loadout, indexed by MissileType
// (DF/HS/IR). The canonical Tarsus start (np-3dp.25) carries a single
// launcher of 4 heat-seekers and nothing else; the equipment dealer and
// base rearm restock / diversify it.
constexpr int k_new_game_missiles[3] = { 0, 4, 0 };

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

// add_item for the unified hold (#80 + #81). Refuses (false, no
// mutation) when it.qty <= 0 OR the unified hold (cargo + items) would
// overflow `capacity` -- callers pass cargo_capacity(p, klass) as today.
// On success: a Weapon- or Upgrade-kind item always appends (no stack
// merge); a Salvage- or Commodity-kind item merges into an existing
// stack of the SAME id + kind + rarity by adding qty, or appends if no
// match is found. Each merged/appended qty counts toward capacity.
bool add_item(PlayerState& p, const inventory::InventoryItem& it, int capacity);

// True iff ANY cargo stack is a contraband commodity (Phase 1.1). The
// search director (hailing.{h,cpp}) uses this to decide whether to
// roll the contraband branch on a hail. Cheap (one unordered_map probe
// per non-zero stack).
bool carrying_contraband(const PlayerState& p);

// ---- ordnance (np-zte.2) ----------------------------------------------------
// type_index is a MissileType (0=DF,1=HS,2=IR); out-of-range is a no-op.
// missile_count reads the stock; consume_missile decrements one and returns
// true, or false (no mutation) when the rack is empty — the caller turns
// that into the out-of-ammo click. add_missiles tops up (clamped ≥ 0).
int  missile_count(const PlayerState& p, int type_index);
bool consume_missile(PlayerState& p, int type_index);
void add_missiles(PlayerState& p, int type_index, int count);

// Same shape as the missile helpers, but for the torpedo rack (no type).
int  torpedo_count(const PlayerState& p);
bool consume_torpedo(PlayerState& p);
void add_torpedoes(PlayerState& p, int count);

// ---- afterburner fuel (np-zte.2) -- merged into energy pool -------------
// drain_afterburner / regen_afterburner / refuel_full are gone: callers now
// touch the player Ship's energy_gj directly (firing.cpp owns its regen).
// No declarations needed.

} // namespace player

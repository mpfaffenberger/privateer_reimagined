#pragma once
// -----------------------------------------------------------------------------
// outfitting.h — hull + equipment pricing and the Ship Dealer / Equipment
// screen bodies (np-9cu.3).
//
// The shop layer that sits between the player's WALLET + owned ship (player.h)
// and the catalogs that describe what's buyable: ShipClass (ship_class.h),
// the gun table (gun.h), and the hand-authored price data in
// assets/data/ship_prices.json + assets/data/equipment_prices.json. It is the
// outfitting twin of economy.h — same data-driven, no-hardcoded-numbers
// philosophy, same np-9cu.4 screen-registration seam, same player::-helpers-
// only mutation rule so save/load (np-ymp.1) serialises every change.
//
// Two product lines:
//   * Ship Dealer — swap the owned HULL. Buying charges (price - trade-in of
//     the old hull) and RE-FITS the new hull's default loadout. v1 choice
//     (documented at buy_hull): the loadout RESETS to the hull default and
//     fitted guns/upgrades on the old hull are lost with no refund — the UI
//     WARNS before committing. Keeps the swap math a single clean credits
//     line instead of a per-part part-out economy.
//   * Equipment — buy guns into the hull's mounts (capped by mount count;
//     a turret mount first needs its turret hardware bought, #145),
//     climb the shield/engine upgrade ladders (capped by the hull's
//     max_shield_level / max_engine_level), and buy the one-time cargo
//     expansion (the flag player::cargo_capacity already reads).
//
// Engine level -> speed: the player flies camera-driven (see main.cpp), so
// engine_level is folded into the player's effective top speed by scaling the
// camera's speed caps off the hull's cruise/afterburner numbers. See
// effective_speed_caps + the note in outfitting.cpp; main.cpp applies it on
// launch and at startup. (The SHIELD effect_pct TODO in ship.cpp is a
// separate concern; see that comment.)
//
// The pricing + transaction MODEL is pure data (no ImGui) so the offline
// harness (tools/test_outfitting.cpp) links it under OUTFITTING_HEADLESS,
// exactly like economy.cpp / test_economy.cpp. The screen bodies are the only
// part behind the headless guard.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

struct PlayerState;
struct ShipClass;

namespace outfitting {

// Load assets/data/ship_prices.json + equipment_prices.json. Idempotent
// (reload replaces). Returns the number of priced hulls. Logs one summary.
int load(const std::string& ship_prices_path, const std::string& equip_prices_path);

// ---- pricing queries (0 = unknown / not for sale) ---------------------------
struct HullOffer { std::string id; int64_t price = 0; };
const std::vector<HullOffer>& hull_catalog();
int64_t hull_price(const std::string& hull_id);
// Trade-in credit for owning `hull_id` (= price * trade_in_pct, floored).
int64_t hull_trade_in(const std::string& hull_id);
// Net credits to switch owned hull `current` -> `target` (price minus the
// trade-in of `current`; may be negative if you downgrade to a cheaper hull).
int64_t hull_net_cost(const std::string& target, const std::string& current);

int64_t gun_price(const std::string& gun_short_name);
int64_t armor_price(const std::string& armor_name);
// Credits to upgrade the shield/engine to `target_level` (one step). 0 if the
// level is out of the priced ladder.
int64_t shield_upgrade_price(int target_level);
int64_t engine_upgrade_price(int target_level);
int64_t cargo_expansion_price();
// Turret hardware, one flat price per position (#145, `turret_price`).
int64_t turret_price();

// Effective top-speed caps for the player's current hull. Pure hull value;
// engine upgrades no longer scale speed (gamefaq 4.6.2 — engine upgrades
// produce power for weapons/AB/shields, they don't make you go faster).
// Falls back to the stock 300/600 when the hull class isn't loaded.
struct SpeedCaps { float cruise0 = 300.0f; float cruise1 = 600.0f; };
SpeedCaps effective_speed_caps(const PlayerState& p);

// Absolute GJ/s the engine upgrade ADDS to the player's recharge rate at
// the given upgrade level (hand-authored per-level table in
// equipment_prices.json).
float engine_recharge_bonus_for(int engine_level);

// Absolute GJ/s the shield generator CONSUMES from the player's recharge
// budget at the given upgrade level (hand-authored per-level table).
float shield_recharge_drain_for(int shield_level);

// ---- transactions (headless-safe; UI + harness share these) -----------------
// All enforce affordability + catalog/hull limits and mutate ONLY through
// player:: helpers. Return true on success (credits/equipment changed), false
// (no mutation) on refusal. `klass` is the player's CURRENT hull class.

// Swap to `target` hull. Charges hull_net_cost, resets loadout to the new
// hull's default FORWARD guns and stock shield/engine/cargo (see header —
// lossy in v1, the UI warns). Turret hardware is NOT included (#145): the
// turret mounts start empty until bought. No-op+false if target == current
// or unaffordable.
bool buy_hull(PlayerState& p, const std::string& target);

// Reset gun_mounts + turrets to `klass`'s stock guns (free, no credits).
// with_turrets=false (dealer hulls) leaves turret mounts empty and unowned;
// true (the --ship dev override) also grants every turret slot + its gun.
void fit_stock_guns(PlayerState& p, const ShipClass* klass, bool with_turrets);

// Fit `gun_short_name` into mount slot `mount_index` (0-based). Refused if the
// slot isn't player::mount_fittable (out of range / unbought turret) or the
// gun isn't for sale.
bool buy_gun(PlayerState& p, const std::string& gun_short_name,
             int mount_index, const ShipClass* klass);

// Buy / sell the turret HARDWARE for one of the hull's TurretSlots (#145).
// Buying refuses an unknown slot, one already owned, or a short wallet.
// Selling refunds turret_price() and refuses while any of the slot's mounts
// still carries a gun -- sell the guns first, nothing vanishes silently.
bool buy_turret(PlayerState& p, const std::string& slot_id, const ShipClass* klass);
bool sell_turret(PlayerState& p, const std::string& slot_id, const ShipClass* klass);

// Sell the gun currently fitted at `mount_index` back to the dealer for a
// full-price refund. Refused if the mount is empty (or out of range).
// The mount is cleared to "" on success so the slot can be re-fitted.
bool sell_gun(PlayerState& p, int mount_index, const ShipClass* klass);

// Buy and fit an armor package by ArmorType::name. Replaces the currently
// fitted package; empty PlayerState::armor_name means the hull's stock armor.
bool buy_armor(PlayerState& p, const std::string& armor_name);

// Climb one rung of the shield / engine ladder (level -> level+1), capped by
// the hull's max_shield_level / max_engine_level.
bool upgrade_shield(PlayerState& p, const ShipClass* klass);
bool upgrade_engine(PlayerState& p, const ShipClass* klass);

// Sell the current shield/engine upgrade (refund its upgrade price, drop
// one level). PlayerState is the SINGLE point of truth; the live ship's
// shield_mult is rebound on next launch by apply_player_loadout.
bool sell_shield(PlayerState& p, const ShipClass* klass);
bool sell_engine(PlayerState& p, const ShipClass* klass);

// Buy the one-time cargo expansion flag. Refused if already owned.
bool buy_cargo_expansion(PlayerState& p);

// Sell the cargo expansion back to the dealer (full refund + clear the
// flag). Refused if not owned.
bool sell_cargo_expansion(PlayerState& p);

// ---- discrete equipment (np-3dp.27) ----------------------------------------
// Buy-once-per-ship upgrades read out of equipment_prices.json's
// `discrete_equipment` map. Each is a bool flag the dealer flips; the
// runtime effects are wired at the loadout/launch seam (jump drive gates
// jump eligibility, ECM breaks missile locks, etc.). Known items:
//   * jump_drive      — allows taking a jump gate (the J prompt)
//   * ecm_lN (N=1..3) — passive missile-lock break chance per second
//   * repair_droid    — hull repair while flying
//   * adv_repair_droid (Righteous Fire) — repair_droid twice as fast
//   * tractor_beam    — pull loot cargo
int64_t discrete_price(const std::string& item);
bool    buy_discrete(PlayerState& p, const std::string& item);

// Register the Ship Dealer + Equipment screen bodies with the base-screen
// framework (np-9cu.4 seam). Call once at startup, after load().
void register_screens();

} // namespace outfitting

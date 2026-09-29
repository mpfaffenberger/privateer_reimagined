#pragma once
// -----------------------------------------------------------------------------
// repair.h — paid hull repair + rearm at a base (np-zte.2).
//
// A base service: for credits, restore the player's hull armor to full and
// restock missiles. Afterburner fuel used to be a third line item; the
// fuel pool merged into the ship's energy bank (player.h note), which
// recharges for free — there's nothing to sell, so the refuel option is
// gone. The hull state lives on the in-flight Ship (transient,
// per-session) while ammo lives on PlayerState (persistent), so the
// repair functions take BOTH and route every credit movement through
// player::spend_credits (one enforcement path, same discipline as
// outfitting.cpp).
//
// Pricing scales with what's actually consumed:
//   * hull — credits per cm of armor missing across all facings, so a
//            lightly-scratched hull is cheap and a near-wreck is dear.
//   * ammo — flat per-missile restock price, per type, up to a cap.
// All constants live in repair.cpp and are the feature's only tuning knobs.
//
// Headless-safe: links against ship.cpp + player.cpp with no UI/audio, so
// the offline harness can prove the credit math + heal without a window.
// -----------------------------------------------------------------------------

#include <cstdint>

struct Ship;
struct PlayerState;

namespace repair {

// What a full-service visit would cost RIGHT NOW, broken out so the UI can
// show line items and disable buttons the player can't afford. `total` is
// the sum; a field is 0 when nothing of that kind needs doing.
struct Quote {
    int64_t hull_cost    = 0;   // restore armor to full
    int64_t fuel_cost    = 0;   // legacy field, always 0 (kept so UI binds
                                // don't have to be churned; can be deleted
                                // alongside any lingering refuel buttons)
    int64_t missile_cost = 0;   // restock to the standard loadout
    int64_t total        = 0;
    bool    hull_damaged = false;
    bool    fuel_low     = false;  // legacy field, always false
    bool    missiles_low = false;
};

// Price the work. `ship` may be null (no player ship yet) — hull_cost is
// then 0. Never mutates anything.
Quote quote(const Ship* ship, const PlayerState& p);

// Repair the hull armor to full (via ship::heal_to_full) for hull_cost.
// Returns false (no mutation) if the ship is null, undamaged, or the player
// can't afford it. Logs the before/after for the validation trail.
bool repair_hull(Ship& ship, PlayerState& p);

// Restock missiles to the standard loadout for missile_cost. False if
// already stocked or unaffordable.
bool rearm(PlayerState& p);

// ---- missile + torpedo buying (np-3dp.26 + np-zte.2) -----------------------
// Privateer's equipment dealer sells HARDWARE for the launcher slots in
// addition to ammo. A "missile launcher" costs 10,000 cr and unlocks 10
// rack slots; a "torpedo tube" costs 2,500 cr and unlocks 10 torpedo
// slots. Both are persistent -- once bought they're OWNED for the run.
// The Tarsus starts the game with ONE missile launcher already fitted (and
// 4 heat-seekers to fill it), but no torpedo tube. Capacity here is
// PER-LAUNCHER, so a player who has bought both missile launchers has
// 20 slots, and the dealer math never sees the difference.
constexpr int k_missile_rack_per_launcher  = 10;
constexpr int k_torpedo_rack_per_tube     = 10;
constexpr int k_max_missile_launchers     = 2;   // the two-launcher rule (left + right)
constexpr int k_max_torpedo_launchers     = 2;   // was 1; now can have 2 (left + right)

// Hardware dealer prices (the OWNED-but-buyable items on the dealer screen).
constexpr int64_t k_missile_launcher_price = 10000;
constexpr int64_t k_torpedo_launcher_price =  2500;
// Sell-back refund is a fraction of the buy price (the dealer keeps the
// rest as a part-out fee). 75% is the chosen band; matches Privateer's
// "trade-in" feel without making the round-trip free.
constexpr double k_sell_refund_pct = 0.75;

// Convenience: sell refund for a missile launcher or torpedo tube.
constexpr int64_t k_missile_launcher_sell_price = 7500;   // 75% of 10 000
constexpr int64_t k_torpedo_launcher_sell_price = 1875;  // 75% of 2 500

// Per-missile dealer price by MissileType index (0=DF, 1=HS, 2=IR, 3=FF).
// Canonical gamefaq prices: 20 / 35 / 75; FF 100 is tuned. Out-of-range -> 0.
int64_t missile_price(int type);

// Compute total rack size for missiles, given the launchers the player owns.
// Returns 0 if the player has bought no missile launchers (i.e. no rack
// exists yet); the dealer hides its missile rows when this is 0.
int missile_rack_capacity(const PlayerState& p);
int torpedo_rack_capacity(const PlayerState& p);

// Helper queries: which side(s) are free vs. filled.
int missile_launchers_owned(const PlayerState& p);
int torpedo_launchers_owned(const PlayerState& p);

// True when a hardpoint slot is empty (neither a missile nor a torpedo
// launcher is fitted on that side). Each hardpoint holds ONE launcher, so
// buying missile-vs-torpedo on the same side is mutually exclusive — the
// Tarsus's starter missile is on the LEFT, so the player must sell it (or
// have a torpedo on that side) before fitting a torpedo there.
bool left_hardpoint_free(const PlayerState& p);
bool right_hardpoint_free(const PlayerState& p);

// Total missiles currently loaded across all types.
int missiles_total(const PlayerState& p);

// Buy `count` missiles of `type` (0=DF/1=HS/2=IR/3=FF) at missile_price each,
// capped so the rack never exceeds the player's capacity. Returns false
// (no spend) when the type/count is invalid, the rack is full, or the
// player can't afford the FULL count.
bool buy_missiles(PlayerState& p, int type, int count);

// Torpedo rack price. Privateer's Proton Torpedo canon is ~35 cr; we use
// a single price for torpedoes (no DF/HS/IR fan-out — torpedoes are one
// physical ammo type in Privateer, the labels DF/HS/IR are a missile-only
// thing).
int64_t torpedo_price();

// Total torpedoes currently loaded (single counter, no fan-out).
int torpedoes_total(const PlayerState& p);

// Buy `count` torpedoes, capped so the rack never exceeds capacity.
// Returns false (no spend) on bad count, full rack, or unaffordable.
bool buy_torpedo(PlayerState& p, int count);

// Buy hardware: a missile-launcher or torpedo-launcher "rack" you OWNED,
// and now have to PAY FOR. There are two physical hardpoints (left and
// right) for each ammo type; the player can fill them independently up
// to the 2-launcher-per-side cap. Returns false (no spend) if both
// hardpoints are already filled, or you can't afford the price.
bool buy_missile_launcher_left(PlayerState& p);
bool buy_missile_launcher_right(PlayerState& p);
bool buy_torpedo_launcher_left(PlayerState& p);
bool buy_torpedo_launcher_right(PlayerState& p);

// Sell-back: drop a single hardpoint for a credit refund. The side you
// have to sell is whichever is currently filled (priority: right, then
// left, so the Tarsus's starter LEFT launcher is the LAST to be sold off).
// Refuses if both sides are already empty.
bool sell_missile_launcher_left(PlayerState& p);
bool sell_missile_launcher_right(PlayerState& p);
bool sell_torpedo_launcher_left(PlayerState& p);
bool sell_torpedo_launcher_right(PlayerState& p);

// Sell-back for a single round of missile / torpedo ammo at full price.
// Refuses if the rack has zero of that type (nothing to sell).
bool sell_missile(PlayerState& p, int type);   // type is 0=DF,1=HS,2=IR,3=FF
bool sell_torpedo(PlayerState& p);   // single round at a time

} // namespace repair

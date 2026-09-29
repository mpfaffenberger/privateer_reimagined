#pragma once
// -----------------------------------------------------------------------------
// inventory.h — unified cargo-hold types for items + commodities (np-ma4).
//
// One cargo hold, two kinds of occupants, both counted in the same
// `cargo_capacity` (player::cargo cap) under the unified-hold design:
//
//   * Commodities  — bulk goods (the existing 50 commodities incl.
//                    SLAVES / MAGIC). Each stack of N is 1 cargo space
//                    per the unified-hold rule ("each discrete item = 1
//                    cargo space"); stacking is a presentational
//                    convenience, not a storage multiplier.
//   * Items        — discrete things: salvage, weapons, upgrades. Each
//                    discrete InventoryItem is 1 cargo space, period.
//                    Weapon = 1 gun, Upgrade = 1 installable buff,
//                    Salvage = 1 scrap drop.
//
// Why a single hold? The pre-existing `player::cargo` only modeled
// commodity stacks; loot drops + salvage + bought guns had no place to
// go and didn't share a capacity with bulk goods. Splitting them across
// two caps (commodity vs item) was tempting but doubled every UI that
// has to render the hold AND every transaction that moves mass around.
// One hold, one cap, one rendering path — drops, missions, NPC trades
// all flow through the same cargo model.
//
// ----- rarity + mods (the only tuning surface this header exposes) -----
// Rarity is a first-class property carried on every item from drop ->
// inventory -> mount/sell. WeaponMods is the per-weapon stat delta
// applied in firing.cpp / gun.cpp at fire time:
//   Rare     -> +10% fire-rate, -10% energy-per-shot
//   Legendary-> +20% fire-rate, -20% energy-per-shot
// (Same direction both axes — rarer guns shoot faster AND cheaper.
// Tuning lives in the .cpp; this header is the SHAPE only.)
//
// ----- savegame surface -----
// InventoryItem is a plain-old-data struct (no pointers, no owning
// non-trivial resources beyond std::string), so savegame.cpp can
// serialise it round-trip with the existing per-faction / per-commodity
// machinery — see Phase 4b / 4f in the plan.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>

// Forward decls keep this header's "shapes only, no heavy deps" discipline
// (it's pulled in by player.h, which everything includes). The pricing +
// sell helpers below take PlayerState&; the CargoHold screen body takes the
// base_screens BaseContext& — both defined elsewhere, only referenced here.
struct PlayerState;
struct BaseContext;

namespace inventory {

// What an InventoryItem IS. Drives both UI grouping (Commodities tab vs
// Items tab in the Cargo Hold) and downstream policy (weapons may be
// equipped, upgrades install into permanent_mods, salvage is sold as
// scrap — all distinct code paths in the .cpp).
enum class ItemKind : uint8_t {
    Commodity,   // bulk good, already exists in the commodity table; this
                 // is the explicit "I'm in the unified hold AS an item,
                 // not as a normal cargo stack" path (e.g. bounty/quest
                 // items carried as discrete units)
    Salvage,     // scrap / loot drop (scrap_metal, etc.). May also be a
                 // real `commodity` (Salvage-as-commodity sells through
                 // the Commodity Exchange — see 4f).
    Weapon,      // a gun the player can fit into a gun mount; rarity +
                 // mods travel with the item into the mount
    Upgrade,     // a permanent_mod the player can install (one-shot);
                 // not consumed by carrying, only by installing
};

// How rare / valuable an item is. Drives sell price multiplier, drop
// chance, and (for weapons) the stat delta in WeaponMods.
enum class Rarity : uint8_t {
    Basic,       // default; back-compat read for old bare-string mounts
    Rare,        // +10% fire-rate, -10% energy
    Legendary,   // +20% fire-rate, -20% energy
};

// Per-weapon stat delta. Applied at fire time by firing.cpp / gun.cpp;
// default is "no change" so old / Basic items carry zero impact and
// don't need a special case in the firing math.
struct WeaponMods {
    float fire_rate_mult = 1.0f;
    float energy_mult    = 1.0f;
};

// A single discrete thing in the unified hold. One item = one cargo
// space (per the unified-hold rule); `qty` is the stack count for
// bulk-ish things (Commodity/Salvage kind), or 1 for Weapon/Upgrade
// which never stack. `mods` is meaningful for Weapon-kind items and
// ignored otherwise (kept on the struct so a single savegame shape
// covers every kind without a discriminated union).
struct InventoryItem {
    std::string id;                                 // stable savegame id
    ItemKind    kind  = ItemKind::Salvage;          // default: scrap drop
    Rarity      rarity = Rarity::Basic;             // default: untuned
    int         qty   = 1;                         // stack size (see note)
    WeaponMods  mods;                               // weapon-kind only
};

// ----- loot pricing + the CargoHold sell screen (Phase 4f, #95/96/97) -----
//
// The sell side of the unified hold: load the hand-authored value table
// (loot_prices.json), price an InventoryItem, and let the CargoHold base
// screen liquidate loot for credits. Pricing is pure data (no ImGui); the
// screen body mirrors outfitting.cpp's hook+ImGui pattern.

// Parse loot_prices.json into a base-value map + rarity multipliers.
// Missing / unparseable file is NON-FATAL: sane defaults stand in
// (rare 2.5x, legendary 6x, unknown id 50 cr). Returns the number of
// base-value entries loaded. Logs a one-line summary.
int load_prices(const std::string& path);

// Sale value of an item: base_value(id, default 50) * rarity multiplier
// * qty, floored to whole credits.
int64_t item_value(const InventoryItem& it);

// Sell the item at p.items[index]: credit item_value to the player and
// erase the stack. Returns false (no mutation) on an out-of-range index.
bool sell_item(PlayerState& p, int index);

// #112: sell ONE unit of the bulk commodity stack at p.cargo[cargo_index]
// for `unit_price` credits. Decrements units; erases the stack when it hits
// zero. Returns false (no mutation) on a bad index or non-positive units.
// Shared by the LANDED CargoHold (market price) and the in-flight panel
// (flat default_cargo_unit_value()).
bool sell_cargo_unit(PlayerState& p, int cargo_index, int64_t unit_price);

// Flat per-unit credit value used for in-flight bulk-commodity sales (v1 has
// no market context mid-flight). Landed sales use the real exchange price.
int64_t default_cargo_unit_value();

// Install the Upgrade-kind item at p.items[index] (#99): resolve its
// PermanentMod (known upgrade ids carry a tuned effect; unknown ids
// default to shield_pct +0.05), refuse if an id-equal mod is ALREADY in
// p.permanent_mods (no stacking — see PermanentMod in player.h), else
// append the mod and erase the consumed item. Returns false (no mutation)
// on a bad index, a non-Upgrade item, or a duplicate. apply_player_loadout
// (main.cpp) re-applies installed mods to the live Ship each launch.
bool install_upgrade(PlayerState& p, int index);

// Equip the Weapon-kind item at p.items[item_index] into gun mount
// `mount_index` (#98): ensure p.gun_mounts has that slot (resize with
// empty MountSlot{} if short), overwrite it with a MountSlot carrying the
// item's id + rarity (so its WeaponMods follow the gun onto the hull),
// then erase the consumed item. Returns false (no mutation) on an
// out-of-range item_index, a non-Weapon item, or a mount that isn't
// player::mount_fittable on the current hull (negative, past the hull's
// mounts, or a turret whose hardware isn't installed -- #145).
// apply_player_loadout (main.cpp) re-applies gun_mounts to the live Ship
// each launch, so the fitted weapon takes effect on the next spawn.
bool equip_weapon(PlayerState& p, int item_index, int mount_index);

// CargoHold base-screen body (registered via base_screens::register_screen).
// Draws cargo usage, the commodity manifest (now sellable at market price),
// and the sellable items list. Defined under !INVENTORY_HEADLESS (ImGui).
void cargohold_screen(BaseContext& ctx);

// #112: in-flight inventory window. A standalone ImGui overlay (toggled by
// `I` in Flight) that shows the unified hold and lets the player sell bulk
// commodities (flat price) + sell/fit/install loot WITHOUT docking. Reuses
// the same model mutators as the CargoHold, so there's one enforcement path.
// `*p_open` is the caller's visibility flag (window close button clears it).
// No-op when p_open is null or *p_open is false.
void in_flight_panel(PlayerState& p, bool* p_open);

// Register the CargoHold screen body with base_screens. Called once at
// startup from main.cpp, beside outfitting::register_screens().
void register_screens();

} // namespace inventory

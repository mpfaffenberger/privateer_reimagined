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

} // namespace inventory

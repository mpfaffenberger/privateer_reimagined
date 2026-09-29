#pragma once
// Player gun loadout <-> live Ship mounts: building the mount list from the
// persistent loadout, and moving fitted guns between physical hardpoints.

#include "firing.h"
#include "gun.h"
#include "player.h"
#include "ship.h"
#include "ship_class.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace armament_loadout {

// Rebuild ship.mounts (+ the parallel gun_cooldowns / gun_armed / mount_mods)
// from ship.klass->default_guns and player.gun_mounts (#514, pulled out of
// main.cpp apply_player_loadout so it can be unit-tested). Mount POSITIONS,
// turret flags and cones come straight from the class default_guns (authored
// in assets/ships/<hull>/ship.json) -- the single source of truth for muzzle
// geometry. player.gun_mounts only decides how many hardpoints are FILLED and
// with what gun, in list order, capped at the hull's hardpoint count; with no
// loadout set (--ship dev override) every hardpoint keeps its default gun.
// Unknown gun names fall back to a Laser; an EMPTY slot (sold gun, or a
// turret whose hardware isn't owned, #145/#510) stays as an inert
// GunType::Count mount so mount indices keep lining up with gun_mounts.
inline void fit_player_mounts(Ship& ship, const PlayerState& player) {
    const ShipClass* k = ship.klass;
    ship.mounts.clear();
    const size_t slots = k ? k->default_guns.size() : size_t{0};
    const size_t n = player.gun_mounts.empty()
                       ? slots
                       : std::min(player.gun_mounts.size(), slots);
    for (size_t i = 0; i < n; ++i) {
        GunMount m = k->default_guns[i];   // position + default type from ship.json
        if (!player.gun_mounts.empty()) {
            const std::string& gun_id = player.gun_mounts[i].gun_id;
            if (gun_id.empty() || !player::mount_fittable(player, k, (int)i)) {
                m.type = GunType::Count;                              // inert: nothing fitted
            } else {
                const GunType t = gun::from_name(gun_id);
                m.type = (t == GunType::Count) ? GunType::Laser : t;  // player gun overrides type only
            }
        }
        // Keep the authored cone: only turrets read it, and clamping it
        // (the old 1-degree override) left player turrets unable to fire (#379).
        ship.mounts.push_back(m);
    }
    ship.gun_cooldowns.assign(ship.mounts.size(), 0.0f);
    // Fresh loadout = ALL mode, so the stored mode index matches the mask
    // (the legacy default index 3 isn't ALL for every loadout).
    firing::arm_all_guns(ship);
    // Carry per-mount weapon mods (#90) onto the live ship, parallel to
    // mounts. Default WeaponMods{} (1.0/1.0) is a no-op; we overwrite the
    // entries the player has actually fitted from their MountSlot::mods so
    // firing::tick applies the rarity fire-rate / energy deltas per shot.
    ship.mount_mods.assign(ship.mounts.size(), inventory::WeaponMods{});
    for (size_t i = 0; i < ship.mount_mods.size() && i < player.gun_mounts.size(); ++i)
        ship.mount_mods[i] = player.gun_mounts[i].mods;
}

inline bool swap_mounted_weapons(PlayerState& player, Ship& ship,
                                 size_t from, size_t to) {
    if (from == to || from >= player.gun_mounts.size() ||
        to >= player.gun_mounts.size() || from >= ship.mounts.size() ||
        to >= ship.mounts.size() || player.gun_mounts[from].gun_id.empty()) {
        return false;
    }
    // Neither gun may land in a mount the player can't fit -- e.g. a turret
    // whose hardware was never bought (#145). Same rule as the dealer.
    const bool to_occupied = !player.gun_mounts[to].gun_id.empty();
    if (!player::mount_fittable(player, ship.klass, (int)to) ||
        (to_occupied && !player::mount_fittable(player, ship.klass, (int)from))) {
        return false;
    }

    std::swap(player.gun_mounts[from], player.gun_mounts[to]);
    // Physical geometry and turret behavior belong to the destination mount.
    // Only the fitted gun and its item-owned runtime state move.
    std::swap(ship.mounts[from].type, ship.mounts[to].type);
    if (from < ship.mount_mods.size() && to < ship.mount_mods.size())
        std::swap(ship.mount_mods[from], ship.mount_mods[to]);
    if (from < ship.gun_cooldowns.size() && to < ship.gun_cooldowns.size())
        std::swap(ship.gun_cooldowns[from], ship.gun_cooldowns[to]);
    return true;
}

} // namespace armament_loadout

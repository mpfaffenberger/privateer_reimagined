#pragma once
// Transaction for moving fitted guns between existing physical hardpoints.

#include "player.h"
#include "ship.h"

#include <cstddef>
#include <utility>

namespace armament_loadout {

inline bool swap_mounted_weapons(PlayerState& player, Ship& ship,
                                 size_t from, size_t to) {
    if (from == to || from >= player.gun_mounts.size() ||
        to >= player.gun_mounts.size() || from >= ship.mounts.size() ||
        to >= ship.mounts.size() || player.gun_mounts[from].gun_id.empty()) {
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

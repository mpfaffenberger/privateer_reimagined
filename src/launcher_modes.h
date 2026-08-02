#pragma once
// Pure ordnance-selection rules shared by input, firing, HUD, and tests.

#include "missile.h"
#include "player.h"

namespace launcher_modes {

inline bool has_missile_hardware(const PlayerState& player) {
    return player.missile_launcher_left || player.missile_launcher_right;
}

inline bool has_torpedo_hardware(const PlayerState& player) {
    return player.torpedo_launcher_left || player.torpedo_launcher_right;
}

inline bool has_compatible_hardware(const PlayerState& player, int type_index) {
    if (type_index < 0 || type_index >= kMissileTypeCount) return false;
    return type_index == (int)MissileType::TORPEDO
        ? has_torpedo_hardware(player)
        : has_missile_hardware(player);
}

inline int ammo_count(const PlayerState& player, int type_index) {
    if (type_index < 0 || type_index >= kMissileTypeCount) return 0;
    return type_index == (int)MissileType::TORPEDO
        ? player.torpedoes
        : player.missiles[type_index];
}

inline bool selectable(const PlayerState& player, int type_index) {
    return has_compatible_hardware(player, type_index)
        && ammo_count(player, type_index) > 0;
}

// Advance to the next installed, loaded ordnance type. If none are loaded,
// retain the current selection so HUD state remains stable and honest.
inline int next_selection(const PlayerState& player, int current) {
    for (int step = 1; step <= kMissileTypeCount; ++step) {
        const int candidate = (current + step + kMissileTypeCount)
                            % kMissileTypeCount;
        if (selectable(player, candidate)) return candidate;
    }
    return current;
}

inline int missile_launcher_count(const PlayerState& player) {
    return (player.missile_launcher_left ? 1 : 0)
         + (player.missile_launcher_right ? 1 : 0);
}

inline int torpedo_launcher_count(const PlayerState& player) {
    return (player.torpedo_launcher_left ? 1 : 0)
         + (player.torpedo_launcher_right ? 1 : 0);
}

struct SideState {
    bool missile_installed = false;
    bool torpedo_installed = false;
    bool active = false;
};

inline SideState side_state(const PlayerState& player, int side,
                            int selected_type) {
    if (side < 0 || side > 1) return {};
    const bool left = side == 0;
    SideState state;
    state.missile_installed = left ? player.missile_launcher_left
                                   : player.missile_launcher_right;
    state.torpedo_installed = left ? player.torpedo_launcher_left
                                   : player.torpedo_launcher_right;
    state.active = selected_type == (int)MissileType::TORPEDO
        ? state.torpedo_installed : state.missile_installed;
    return state;
}

} // namespace launcher_modes

#pragma once
// Pure gun-group mode indexing shared by input, firing, and focused tests.

namespace gun_modes {

// Modes are {UNARMED, one per unique gun type, ALL}.
constexpr int count(int unique_type_count) {
    return unique_type_count > 0 ? unique_type_count + 2 : 1;
}

constexpr int normalize(int mode, int unique_type_count) {
    const int mode_count = count(unique_type_count);
    return ((mode % mode_count) + mode_count) % mode_count;
}

constexpr bool is_unarmed(int mode, int unique_type_count) {
    return normalize(mode, unique_type_count) == 0;
}

constexpr bool is_all(int mode, int unique_type_count) {
    return unique_type_count > 0 &&
           normalize(mode, unique_type_count) == unique_type_count + 1;
}

// Returns the zero-based unique-type index, or -1 for UNARMED / ALL.
constexpr int type_index(int mode, int unique_type_count) {
    const int normalized = normalize(mode, unique_type_count);
    return normalized > 0 && normalized <= unique_type_count
        ? normalized - 1 : -1;
}

// Turrets are autonomous defensive mounts, not forward-gun groups. They stay
// active in every G-key mode and their turret-only weapon types do not add a
// pointless selectable mode to the forward battery cycle.
constexpr bool participates_in_forward_cycle(bool is_turret) {
    return !is_turret;
}

constexpr bool mount_is_armed(bool is_turret, int mount_type,
                              bool all_forward, int selected_type) {
    return is_turret || all_forward ||
           (selected_type >= 0 && mount_type == selected_type);
}

} // namespace gun_modes

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

// ALL is always the last mode. An empty loadout has only UNARMED (index 0).
constexpr int all_mode(int unique_type_count) {
    return count(unique_type_count) - 1;
}

constexpr bool is_all(int mode, int unique_type_count) {
    return unique_type_count > 0 &&
           normalize(mode, unique_type_count) == all_mode(unique_type_count);
}

// Returns the zero-based unique-type index, or -1 for UNARMED / ALL.
constexpr int type_index(int mode, int unique_type_count) {
    const int normalized = normalize(mode, unique_type_count);
    return normalized > 0 && normalized <= unique_type_count
        ? normalized - 1 : -1;
}

} // namespace gun_modes

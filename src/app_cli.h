#pragma once
// -----------------------------------------------------------------------------
// app_cli.h — engine-independent launch option grammar.
//
// Parsing returns plain data. The Sokol host applies it to AppState, keeping
// command-line policy testable without exposing the giant runtime state.
// -----------------------------------------------------------------------------

#include <string>

struct LaunchOptions {
    std::string system_name = "troy";
    std::string player_ship;
    std::string dev_land_base;
    std::string cinematic;
    std::string goto_system;

    bool system_explicit = false;
    bool capture_clean = false;
    bool skip_title = false;
    bool seed_missions = false;
    bool force_windowed = false;
    bool dev_invuln = false;
    bool dev_jump_drive = false;

    int load_slot = -1;
    float dev_kill_at_s = -1.0f;
    float cinematic_at_s = 2.0f;
    float goto_at_s = 2.0f;
    int goto_soak_count = 0;
    float goto_interval_s = 1.0f;
    int jump_soak_count = 0;
    float jump_interval_s = 2.5f;
};

namespace app_cli {

LaunchOptions parse(int argc, char* const* argv);

} // namespace app_cli

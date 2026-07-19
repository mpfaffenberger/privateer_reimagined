// -----------------------------------------------------------------------------
// app_cli.cpp — launch option parser. See app_cli.h.
// -----------------------------------------------------------------------------

#include "app_cli.h"

#include <cstdlib>
#include <cstring>

namespace app_cli {

LaunchOptions parse(int argc, char* const* argv) {
    LaunchOptions out;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool has_value = i + 1 < argc;

        if (std::strcmp(arg, "--system") == 0 && has_value) {
            out.system_name = argv[++i];
            out.system_explicit = true;
        } else if (std::strcmp(arg, "--ship") == 0 && has_value) {
            out.player_ship = argv[++i];
        } else if (std::strcmp(arg, "--capture-clean") == 0) {
            out.capture_clean = true;
        } else if (std::strcmp(arg, "--skip-title") == 0) {
            out.skip_title = true;
        } else if (std::strcmp(arg, "--dev-land") == 0 && has_value) {
            out.dev_land_base = argv[++i];
        } else if (std::strcmp(arg, "--dev-missions") == 0) {
            out.seed_missions = true;
        } else if (std::strcmp(arg, "--windowed") == 0) {
            out.force_windowed = true;
        } else if (std::strcmp(arg, "--load") == 0 && has_value) {
            out.load_slot = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--continue") == 0) {
            out.load_slot = 0;
        } else if (std::strcmp(arg, "--dev-invuln") == 0) {
            out.dev_invuln = true;
        } else if (std::strcmp(arg, "--dev-jump-drive") == 0) {
            out.dev_jump_drive = true;
        } else if (std::strcmp(arg, "--dev-kill-at") == 0 && has_value) {
            out.dev_kill_at_s = (float)std::atof(argv[++i]);
        } else if (std::strcmp(arg, "--play-cinematic") == 0 && has_value) {
            out.cinematic = argv[++i];
        } else if (std::strcmp(arg, "--cine-at") == 0 && has_value) {
            out.cinematic_at_s = (float)std::atof(argv[++i]);
        } else if (std::strcmp(arg, "--goto") == 0 && has_value) {
            out.goto_system = argv[++i];
        } else if (std::strcmp(arg, "--goto-at") == 0 && has_value) {
            out.goto_at_s = (float)std::atof(argv[++i]);
        } else if (std::strcmp(arg, "--goto-soak") == 0 && has_value) {
            out.goto_soak_count = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--goto-interval") == 0 && has_value) {
            out.goto_interval_s = (float)std::atof(argv[++i]);
        } else if (std::strcmp(arg, "--dev-jump-soak") == 0 && has_value) {
            out.jump_soak_count = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--dev-jump-interval") == 0 && has_value) {
            out.jump_interval_s = (float)std::atof(argv[++i]);
        }
    }

    return out;
}

} // namespace app_cli

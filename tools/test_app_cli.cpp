// -----------------------------------------------------------------------------
// test_app_cli.cpp — engine-free proof for launch option parsing.
// -----------------------------------------------------------------------------

#include "app_cli.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

LaunchOptions parse(std::vector<std::string> args) {
    std::vector<char*> raw;
    raw.reserve(args.size());
    for (std::string& arg : args) raw.push_back(arg.data());
    return app_cli::parse((int)raw.size(), raw.data());
}
} // namespace

int main() {
    const LaunchOptions defaults = parse({"game"});
    check(defaults.system_name == "troy" && !defaults.system_explicit,
          "defaults to Troy without marking an override");
    check(defaults.load_slot == -1 && defaults.dev_kill_at_s < 0.0f,
          "load and kill automation default off");
    check(!defaults.dev_zone_editor, "hardpoint zone editor is off for players (#748)");

    const LaunchOptions full = parse({
        "game", "--system", "pyrenees", "--ship", "centurion",
        "--capture-clean", "--skip-title", "--dev-land", "helen",
        "--dev-missions", "--windowed", "--load", "7", "--dev-invuln",
        "--dev-jump-drive", "--dev-zone-editor", "--dev-kill-at", "3.5", "--play-cinematic",
        "demo", "--cine-at", "1.25", "--goto", "troy", "--goto-at", "4",
        "--goto-soak", "12", "--goto-interval", "0.5",
        "--dev-jump-soak", "9", "--dev-jump-interval", "0.75"
    });
    check(full.system_name == "pyrenees" && full.system_explicit,
          "system override carries explicit precedence");
    check(full.player_ship == "centurion" && full.dev_land_base == "helen",
          "string options parse");
    check(full.capture_clean && full.skip_title && full.seed_missions &&
          full.force_windowed && full.dev_invuln && full.dev_jump_drive &&
          full.dev_zone_editor,
          "boolean switches parse");
    check(full.load_slot == 7 && std::fabs(full.dev_kill_at_s - 3.5f) < 0.001f,
          "numeric launch options parse");
    check(full.cinematic == "demo" && std::fabs(full.cinematic_at_s - 1.25f) < 0.001f,
          "cinematic options parse");
    check(full.goto_system == "troy" && full.goto_soak_count == 12 &&
          full.jump_soak_count == 9,
          "system soak options parse");

    const LaunchOptions resume = parse({"game", "--load", "5", "--continue"});
    check(resume.load_slot == 0, "later --continue overrides an earlier slot");

    const LaunchOptions dangling = parse({"game", "--system"});
    check(dangling.system_name == "troy" && !dangling.system_explicit,
          "missing option value leaves defaults intact");

    std::printf("\n=== %s ===\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
// test_sandbox.h — hermetic save directory for headless test harnesses.
//
// savegame resolves its data dir from NP_DATA_DIR when set (see
// user_data_dir() in src/savegame.cpp). Any harness that can reach
// savegame::save / save_timestamped must call isolate_saves() first thing in
// main(), or its fixtures land in the player's real save picker and can even
// become the target of --continue (#242, #383).
// -----------------------------------------------------------------------------
#pragma once

#include "savegame.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace test_sandbox {

// Points savegame's data-dir resolution at `dir` (no validation; callers that
// need a guaranteed sandbox use isolate_saves()).
inline void set_data_dir(const std::filesystem::path& dir) {
#ifdef _WIN32
    _putenv_s("NP_DATA_DIR", dir.string().c_str());
#else
    setenv("NP_DATA_DIR", dir.string().c_str(), 1);
#endif
}

// Points NP_DATA_DIR at a fresh per-harness temp tree and returns it. The tree
// is wiped at the start of each run (not the end) so a failing run can be
// inspected afterwards. Aborts the process if savegame would still resolve
// outside the sandbox: losing a player's saves is worse than a red test.
inline std::filesystem::path isolate_saves(const char* harness) {
    namespace fs = std::filesystem;
    const fs::path dir =
        fs::temp_directory_path() / (std::string("new_privateer_test_") + harness);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    set_data_dir(dir);

    const std::string saves = savegame::saves_dir();
    if (saves.rfind(dir.string(), 0) != 0) {
        std::fprintf(stderr,
                     "FATAL: save sandbox not honoured (saves_dir='%s', expected under '%s')\n",
                     saves.c_str(), dir.string().c_str());
        std::exit(2);
    }
    std::printf("[sandbox] saves isolated in %s\n", saves.c_str());
    return dir;
}

} // namespace test_sandbox

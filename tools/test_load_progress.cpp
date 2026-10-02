// -----------------------------------------------------------------------------
// test_load_progress.cpp — headless proof for loading-bar stage timing (#694).
// -----------------------------------------------------------------------------

#include "load_progress.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;

template <typename T>
void expect(const char* name, const T& got, const T& want) {
    const bool ok = got == want;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
} // namespace

int main() {
    load_progress::Timeline t;

    // The startup sequence: each stage closes the one before it.
    const auto first = t.advance(100.0, 0.08f, "Initializing renderer...");
    expect("first stage opens a load", first.valid, false);

    const auto second = t.advance(150.0, 0.20f, "Preparing save data...");
    expect("second stage closes the first", second.valid, true);
    expect("closed stage label", second.label, std::string{"Initializing renderer..."});
    expect("closed stage duration", second.stage_ms, 50.0);
    expect("load time so far", second.load_ms, 50.0);

    const auto third = t.advance(400.0, 0.32f, "Loading Gemini Sector...");
    expect("third stage duration", third.stage_ms, 250.0);
    expect("load time accumulates", third.load_ms, 300.0);

    // Equal progress is still the same load (a stage may repeat a value).
    const auto repeat = t.advance(410.0, 0.32f, "Still loading...");
    expect("equal progress continues the load", repeat.valid, true);
    expect("equal progress load time", repeat.load_ms, 310.0);

    const auto done = t.advance(500.0, 1.0f, "Entering Gemini Sector...");
    expect("final stage closes the last one", done.label, std::string{"Still loading..."});

    // A save-menu load later restarts the bar from the bottom.
    const auto restart = t.advance(9000.0, 0.08f, "Loading saved game...");
    expect("lower progress opens a new load", restart.valid, false);

    const auto next = t.advance(9020.0, 0.30f, "Releasing the previous system...");
    expect("new load closes its own first stage", next.label, std::string{"Loading saved game..."});
    expect("new load time restarts", next.load_ms, 20.0);

    const auto null_label = load_progress::Timeline{}.advance(0.0, 0.5f, nullptr);
    expect("null label tolerated", null_label.valid, false);

    expect("current stage progress", t.progress(), 0.30f);
    expect("current stage label", t.label(), std::string{"Releasing the previous system..."});
    expect("current stage start", t.stage_start_ms(), 9020.0);

    // Creep: a long stage eases forward, monotonically, but never reaches
    // the next stage (the smallest gap between stages is 0.07).
    using load_progress::creep;
    expect("creep starts at the stage", creep(0.72f, 0.0), 0.72f);
    expect("creep ignores negative time", creep(0.72f, -50.0), 0.72f);
    expect("creep moves after a second", creep(0.72f, 1000.0) > 0.72f, true);
    expect("creep is monotonic", creep(0.72f, 20000.0) > creep(0.72f, 10000.0), true);
    expect("creep still moves after a minute",
           creep(0.82f, 90000.0) - creep(0.82f, 60000.0) > 0.003f, true);
    expect("creep stays below the next stage", creep(0.58f, 1e9) < 0.65f, true);
    expect("creep never passes 100%", creep(0.98f, 1e9), 1.0f);
    expect("finished bar stays full", creep(1.0f, 5000.0), 1.0f);

    std::printf("\n=== %s ===\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}

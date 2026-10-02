#pragma once
// -----------------------------------------------------------------------------
// load_progress.h — stage timing for the blocking loading bar (#694).
//
// Startup and menu-driven loads run synchronously and report coarse stages
// ("Building star system...", "Loading ship artwork...") to the loading
// bar. Timeline turns that stream of stages into per-stage durations so the
// log shows which stage is slow. Pure bookkeeping: the caller supplies the
// clock, so this is headless-testable.
// -----------------------------------------------------------------------------

#include <string>

namespace load_progress {

// The stage that a call to Timeline::advance() just closed.
struct StageEnd {
    bool        valid    = false;  // false when advance() opened a new load
    std::string label;             // the stage that just finished
    double      stage_ms = 0.0;    // time spent in that stage
    double      load_ms  = 0.0;    // time from the load's first stage to now
};

// One blocking load is a run of stages with non-decreasing progress. A
// stage whose progress is LOWER than the previous one means the bar
// restarted, so it opens a new load rather than closing a stage.
class Timeline {
public:
    StageEnd advance(double now_ms, float progress, const char* label);

    // The stage currently running (valid once advance() has been called).
    float              progress()       const { return progress_; }
    const std::string& label()          const { return label_; }
    double             stage_start_ms() const { return stage_start_ms_; }

private:
    bool        active_         = false;
    float       progress_       = 0.0f;
    std::string label_;
    double      stage_start_ms_ = 0.0;
    double      load_start_ms_  = 0.0;
};

// How far a still-running stage may creep the bar past its own progress.
// Kept below the smallest gap between consecutive stages (0.07), so the
// creep never overtakes the value the next stage reports.
constexpr float k_creep_span = 0.06f;

// Bar position for a stage that has been running for `stage_elapsed_ms`:
// eases from `stage_progress` toward stage_progress + k_creep_span without
// reaching it, so a long stage visibly moves. Never exceeds 1.
float creep(float stage_progress, double stage_elapsed_ms);

} // namespace load_progress

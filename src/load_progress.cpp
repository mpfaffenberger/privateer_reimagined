// load_progress.cpp — see load_progress.h.

#include "load_progress.h"

#include <algorithm>

namespace load_progress {

StageEnd Timeline::advance(double now_ms, float progress, const char* label) {
    StageEnd ended;
    if (active_ && progress >= progress_) {
        ended.valid    = true;
        ended.label    = label_;
        ended.stage_ms = now_ms - stage_start_ms_;
        ended.load_ms  = now_ms - load_start_ms_;
    } else {
        load_start_ms_ = now_ms;
    }
    active_         = true;
    progress_       = progress;
    label_          = label ? label : "";
    stage_start_ms_ = now_ms;
    return ended;
}

float creep(float stage_progress, double stage_elapsed_ms) {
    // Hyperbolic ease t / (t + tau): half the span after 15 s, and it keeps
    // visibly moving through stages that run for a minute or more (an
    // exponential ease flattens out and looks stuck again).
    constexpr double k_tau_ms = 15000.0;
    const double t = std::max(stage_elapsed_ms, 0.0);
    const double eased = t / (t + k_tau_ms);
    return std::min(1.0f, stage_progress + k_creep_span * (float)eased);
}

} // namespace load_progress

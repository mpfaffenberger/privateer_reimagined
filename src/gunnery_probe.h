#pragma once
// -----------------------------------------------------------------------------
// gunnery_probe.h — header-only AI gunnery hit-rate instrumentation.
//
// Counts NPC shots fired and shots that connect, bucketed by the shooter's
// skill (skill_f2), and prints a periodic hit-rate summary. Used to verify
// the skill-scaled gunnery scatter lands in the intended bands (novice ~17%,
// veteran ~24%, ace ~37%). Off unless enabled (--gun-stats). Header-only via
// C++17 inline variables so firing.cpp / projectile.cpp / main.cpp share one
// instance with no extra .cpp / CMake wiring.
// -----------------------------------------------------------------------------

#include <cstdio>

namespace gunnery_probe {

inline bool enabled = false;

struct Tier { long shots = 0; long hits = 0; };
inline Tier  g_lo;     // f2 <= 45  (novice / default)
inline Tier  g_mid;    // 45 < f2 < 55 (veteran)
inline Tier  g_hi;     // f2 >= 55  (ace)
inline double g_last_report = 0.0;

inline Tier& bucket(float f2) {
    if (f2 >= 55.0f) return g_hi;
    if (f2 >  45.0f) return g_mid;
    return g_lo;
}

inline void shot(float shooter_f2) { if (enabled) bucket(shooter_f2).shots++; }
inline void hit (float shooter_f2) { if (enabled) bucket(shooter_f2).hits++;  }

inline void report_if_due(double t_now_sec) {
    if (!enabled || (t_now_sec - g_last_report) < 8.0) return;
    g_last_report = t_now_sec;
    auto pct = [](const Tier& x) { return x.shots ? 100.0 * (double)x.hits / (double)x.shots : 0.0; };
    std::printf("[gunstats] novice(f2<=45) %ld/%ld %.1f%%   "
                "vet(45<f2<55) %ld/%ld %.1f%%   "
                "ace(f2>=55) %ld/%ld %.1f%%\n",
                g_lo.hits,  g_lo.shots,  pct(g_lo),
                g_mid.hits, g_mid.shots, pct(g_mid),
                g_hi.hits,  g_hi.shots,  pct(g_hi));
}

} // namespace gunnery_probe

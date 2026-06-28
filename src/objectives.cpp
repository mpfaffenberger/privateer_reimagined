// -----------------------------------------------------------------------------
// objectives.cpp — dynamic-objective ("lead") implementation (Phase 3
//                  Wave 1, issues #71/#73/#74/#75/#76/#77/#78).
//
// A lead is two coupled things:
//   1. A record in the file-static `g_leads` side-table (id + pos + label),
//      which is the authoritative list of "what's live right now".
//   2. A `dynamic = true` NavPointDef pushed into StarSystem::nav_points so
//      the existing nav-map / HUD targeting / autopilot render + target it
//      for free (no parallel UI code).
//
// The two are matched on demand by (dynamic flag + position) rather than a
// cached index, so re-ordering nav_points (e.g. a future spawn that erases a
// nav) never desyncs the lead from its marker. Floats are compared with a
// small epsilon because the position round-trips through the NavPointDef.
//
// RNG: one file-static mt19937 with a fixed seed, same convention as
// loot.cpp — placement is debug-stable across runs, which matters more than
// entropy for a content-test feature.
// -----------------------------------------------------------------------------

#include "objectives.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include "faction.h"
#include "loot.h"
#include "system_def.h"

namespace objectives {

namespace {

// Authoritative list of live leads. Mirrors the dynamic nav points in
// StarSystem::nav_points; see the file banner for the matching contract.
std::vector<Lead> g_leads;

// Monotonic id source so two leads at (nearly) the same spot stay distinct
// in logs / snapshots. Never reset — ids are cosmetic, not indices.
int g_next_id = 1;

// One RNG per process, fixed seed (distinct from loot.cpp's). Placement is
// debug-stable; reproducibility beats entropy for a content-test feature.
std::mt19937& rng() {
    static std::mt19937 r{0xC0DEBABEu ^ 0x1EAD51A7u};
    return r;
}

// A unit vector in a uniformly-random direction on the sphere.
HMM_Vec3 rand_unit_dir() {
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::uniform_real_distribution<float> a(0.0f, 6.2831853f);
    const float z   = u(rng());
    const float ang = a(rng());
    const float r   = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return HMM_V3(r * std::cos(ang), r * std::sin(ang), z);
}

// Does this nav point mark `lead`? Match on the dynamic flag + position
// (epsilon-tolerant) so a re-ordered nav_points list never desyncs.
bool nav_matches_lead(const NavPointDef& n, const Lead& lead) {
    if (!n.dynamic) return false;
    const float eps = 0.5f;   // sub-meter; positions round-trip exactly here
    return std::fabs(n.position.X - lead.pos.X) < eps &&
           std::fabs(n.position.Y - lead.pos.Y) < eps &&
           std::fabs(n.position.Z - lead.pos.Z) < eps;
}

// Erase the dynamic nav point matching `lead` from `sys.nav_points` (the
// first such match). No-op if none is found (already gone / never pushed).
void erase_lead_nav(StarSystem& sys, const Lead& lead) {
    for (auto it = sys.nav_points.begin(); it != sys.nav_points.end(); ++it) {
        if (nav_matches_lead(*it, lead)) {
            sys.nav_points.erase(it);
            return;
        }
    }
}

} // namespace

void add_lead(StarSystem& sys, HMM_Vec3 pos, const std::string& label) {
    Lead lead;
    lead.id    = g_next_id++;
    lead.pos   = pos;
    lead.label = label;
    g_leads.push_back(lead);

    NavPointDef nav;
    nav.name     = label;
    nav.kind     = "nav";
    nav.position = pos;
    nav.dynamic  = true;
    sys.nav_points.push_back(nav);

    std::printf("[lead] new: %s at (%.0f, %.0f, %.0f)\n",
                label.c_str(), pos.X, pos.Y, pos.Z);
}

HMM_Vec3 pick_lead_pos(const StarSystem& sys, HMM_Vec3 sun_pos) {
    std::uniform_real_distribution<float> coin(0.0f, 1.0f);

    // Branch A — midpoint-ish between two distinct existing nav points,
    // nudged by a random offset so leads don't stack exactly on a lane.
    // Only viable with >= 2 nav points; otherwise fall through to deep space.
    if (sys.nav_points.size() >= 2 && coin(rng()) < 0.5f) {
        std::uniform_int_distribution<size_t> pick(0, sys.nav_points.size() - 1);
        const size_t i = pick(rng());
        size_t j = pick(rng());
        if (j == i) j = (j + 1) % sys.nav_points.size();   // ensure distinct

        const HMM_Vec3 a   = sys.nav_points[i].position;
        const HMM_Vec3 b   = sys.nav_points[j].position;
        const HMM_Vec3 mid = HMM_MulV3F(HMM_AddV3(a, b), 0.5f);

        // Offset up to ~30km off the midpoint in a random direction.
        std::uniform_real_distribution<float> mag(2000.0f, 30000.0f);
        return HMM_AddV3(mid, HMM_MulV3F(rand_unit_dir(), mag(rng())));
    }

    // Branch B — deep space: a point > 500km from the sun in a random
    // direction (somewhere between 500km and 900km out).
    std::uniform_real_distribution<float> dist(500000.0f, 900000.0f);
    return HMM_AddV3(sun_pos, HMM_MulV3F(rand_unit_dir(), dist(rng())));
}

void tick(StarSystem& sys, HMM_Vec3 player_pos) {
    // Walk a copy-free index loop; on an arrival we grant the payoff, erase
    // the matching nav, swap-remove the lead, and re-check the same slot.
    for (size_t k = 0; k < g_leads.size();) {
        const Lead& lead = g_leads[k];
        const float d = HMM_LenV3(HMM_SubV3(player_pos, lead.pos));
        if (d < k_arrive_m) {
            // Payoff: an ace-tier pirate drop at the marker — a good cache
            // the player tractors into the hold (#84 path).
            loot::spawn_for(Faction::Pirate, lead.pos, /*is_ace=*/true);
            std::printf("[lead] reached %s -> loot dropped\n", lead.label.c_str());

            erase_lead_nav(sys, lead);
            g_leads.erase(g_leads.begin() + (long)k);
            continue;   // don't advance k — the next lead slid into this slot
        }
        ++k;
    }
}

void clear(StarSystem& sys) {
    // Drop every dynamic nav point regardless of which lead it belonged to —
    // robust even if the side-table and nav list ever drifted apart.
    sys.nav_points.erase(
        std::remove_if(sys.nav_points.begin(), sys.nav_points.end(),
                       [](const NavPointDef& n) { return n.dynamic; }),
        sys.nav_points.end());
    g_leads.clear();
}

int count() {
    return (int)g_leads.size();
}

const std::vector<Lead>& all() {
    return g_leads;
}

} // namespace objectives

// -----------------------------------------------------------------------------
// encounters.cpp — encounter director implementation.
//
// See encounters.h for the design rationale (host/director split, spawn
// budget, the "don't pop in visibly" placement strategy). This file is
// the bookkeeping: parse rules once, then every frame maintain the live
// population within budget.
//
// All director state lives in this translation unit (g_dir) — there's
// exactly one director per running game, and threading it through every
// caller would buy nothing. init() rebuilds it from a StarSystem;
// shutdown() forgets it. Both are cheap and idempotent.
// -----------------------------------------------------------------------------

#include "encounters.h"

#include "ship.h"
#include "ship_registry.h"
#include "system_def.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace encounters {

namespace {

// Region kind, resolved from the JSON string at init so the hot path
// switches on an enum instead of comparing strings every frame.
enum class Region { Anywhere, Belt, Lane };

// One spawn rule, materialised: parsed weights + resolved region geometry
// + the running budget/timer state and the list of ship ids it owns.
struct RuleRuntime {
    std::string name;
    Region      region = Region::Anywhere;

    // Belt geometry (Region::Belt): the asteroid field's AABB.
    HMM_Vec3 belt_center = { 0, 0, 0 };
    HMM_Vec3 belt_half   = { 0, 0, 0 };

    // Lane geometry (Region::Lane): the polyline of nav-point positions.
    std::vector<HMM_Vec3> lane_points;

    // Weighted draws. Parallel name/cumulative-weight layout so a pick is
    // one uniform draw + a linear walk (tiny lists — no need for a BST).
    std::vector<std::string> faction_names;
    std::vector<float>       faction_cum;     // cumulative weights
    std::vector<std::string> class_names;
    std::vector<float>       class_cum;

    int     max_concurrent  = 4;
    float   spawn_interval  = 8.0f;
    AIState initial_state   = AIState::Patrol;

    // Runtime: seconds since last spawn attempt, and the ids we own.
    float                 timer = 0.0f;
    std::vector<uint32_t> managed_ids;
};

struct DirectorState {
    bool                     active = false;
    std::vector<RuleRuntime> rules;
    std::mt19937             rng{ 0xE2C0DE11u };   // fixed seed: reproducible feel
};

DirectorState g_dir;

// ---- geometry helpers -------------------------------------------------------

float len(HMM_Vec3 v) { return std::sqrt(HMM_DotV3(v, v)); }

// Distance from a point to an axis-aligned box (0 inside the box).
float dist_to_aabb(HMM_Vec3 p, HMM_Vec3 center, HMM_Vec3 half) {
    const HMM_Vec3 d = {
        std::max(0.0f, std::fabs(p.X - center.X) - half.X),
        std::max(0.0f, std::fabs(p.Y - center.Y) - half.Y),
        std::max(0.0f, std::fabs(p.Z - center.Z) - half.Z),
    };
    return len(d);
}

// Closest point on segment [a,b] to p.
HMM_Vec3 closest_on_segment(HMM_Vec3 p, HMM_Vec3 a, HMM_Vec3 b) {
    const HMM_Vec3 ab = HMM_SubV3(b, a);
    const float    len2 = HMM_DotV3(ab, ab);
    if (len2 < 1e-3f) return a;
    float t = HMM_DotV3(HMM_SubV3(p, a), ab) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    return HMM_AddV3(a, HMM_MulV3F(ab, t));
}

// Closest point on a polyline to p (and its distance via the caller).
HMM_Vec3 closest_on_polyline(HMM_Vec3 p, const std::vector<HMM_Vec3>& pts) {
    HMM_Vec3 best = pts.empty() ? p : pts[0];
    float    best_d2 = 1e30f;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const HMM_Vec3 c  = closest_on_segment(p, pts[i], pts[i + 1]);
        const HMM_Vec3 dv = HMM_SubV3(c, p);
        const float    d2 = HMM_DotV3(dv, dv);
        if (d2 < best_d2) { best_d2 = d2; best = c; }
    }
    return best;
}

// A uniformly-distributed unit vector on the sphere (Marsaglia's method
// via inverse-CDF on the z-axis). No trig-bias clustering at the poles.
HMM_Vec3 random_unit_dir(std::mt19937& rng) {
    std::uniform_real_distribution<float> uz(-1.0f, 1.0f);
    std::uniform_real_distribution<float> ua(0.0f, 6.2831853f);
    const float z = uz(rng);
    const float a = ua(rng);
    const float s = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return { s * std::cos(a), z, s * std::sin(a) };
}

// ---- placement --------------------------------------------------------------

// Pick a spawn point for a rule: a random point on the 8-15 km shell
// around the player, biased into the rule's region. The shell distance
// is what guarantees "no visible pop-in"; the region bias is the flavour
// (in the belt, along the lane).
HMM_Vec3 pick_spawn_point(const RuleRuntime& r, HMM_Vec3 player, std::mt19937& rng) {
    std::uniform_real_distribution<float> ud(k_spawn_dist_min, k_spawn_dist_max);
    const HMM_Vec3 dir  = random_unit_dir(rng);
    const float    dist = ud(rng);
    HMM_Vec3 p = HMM_AddV3(player, HMM_MulV3F(dir, dist));

    switch (r.region) {
    case Region::Belt:
        // Clamp into the belt AABB so pirates spawn among the rocks. The
        // belt is large (tens of km), so the clamp rarely moves the point
        // far enough to break the offscreen-distance guarantee.
        p.X = std::clamp(p.X, r.belt_center.X - r.belt_half.X, r.belt_center.X + r.belt_half.X);
        p.Y = std::clamp(p.Y, r.belt_center.Y - r.belt_half.Y, r.belt_center.Y + r.belt_half.Y);
        p.Z = std::clamp(p.Z, r.belt_center.Z - r.belt_half.Z, r.belt_center.Z + r.belt_half.Z);
        break;
    case Region::Lane: {
        // Pull the point most of the way onto the nearest lane segment so
        // traffic hugs the corridor without sitting dead on the centreline.
        if (!r.lane_points.empty()) {
            const HMM_Vec3 on = closest_on_polyline(p, r.lane_points);
            p = HMM_AddV3(HMM_MulV3F(p, 0.35f), HMM_MulV3F(on, 0.65f));
        }
        break;
    }
    case Region::Anywhere:
        break;
    }

    // After region biasing the point may have crept inside the min shell
    // (clamping/lerping pulls toward the region). Push it back out along
    // the player->point ray so we never spawn right on top of the player.
    HMM_Vec3 to = HMM_SubV3(p, player);
    const float d = len(to);
    if (d < k_spawn_dist_min) {
        const HMM_Vec3 u = (d > 1e-3f) ? HMM_DivV3F(to, d) : dir;
        p = HMM_AddV3(player, HMM_MulV3F(u, k_spawn_dist_min));
    }
    return p;
}

// Is `player` close enough to a rule's region for it to start spawning?
// Anywhere rules are always armed; region rules need the player within
// k_region_activate_m of the region so belt pirates / lane traffic only
// materialise where they belong.
bool region_armed(const RuleRuntime& r, HMM_Vec3 player) {
    switch (r.region) {
    case Region::Anywhere: return true;
    case Region::Belt:
        return dist_to_aabb(player, r.belt_center, r.belt_half) <= k_region_activate_m;
    case Region::Lane: {
        if (r.lane_points.empty()) return true;
        const HMM_Vec3 on = closest_on_polyline(player, r.lane_points);
        return len(HMM_SubV3(on, player)) <= k_region_activate_m;
    }
    }
    return true;
}

// ---- weighted picks ---------------------------------------------------------

// Walk a cumulative-weight table for a uniform draw. Empty table -> "".
const std::string& weighted_pick(const std::vector<std::string>& names,
                                 const std::vector<float>& cum,
                                 std::mt19937& rng) {
    static const std::string empty;
    if (names.empty() || cum.empty()) return empty;
    std::uniform_real_distribution<float> u(0.0f, cum.back());
    const float roll = u(rng);
    for (size_t i = 0; i < cum.size(); ++i) {
        if (roll <= cum[i]) return names[i];
    }
    return names.back();
}

// Build a parallel name + cumulative-weight table from JSON weights,
// dropping non-positive weights. Returns the total (0 if nothing usable).
float build_cum(const std::vector<EncounterWeight>& src,
                std::vector<std::string>& names, std::vector<float>& cum) {
    float total = 0.0f;
    for (const auto& w : src) {
        if (w.weight <= 0.0f || w.name.empty()) continue;
        total += w.weight;
        names.push_back(w.name);
        cum.push_back(total);
    }
    return total;
}

// Total director-owned ships across all rules.
int total_managed() {
    int n = 0;
    for (const auto& r : g_dir.rules) n += (int)r.managed_ids.size();
    return n;
}

} // namespace

// ---- public API -------------------------------------------------------------

void init(const StarSystem& system) {
    shutdown();   // forget any prior system's rules

    for (const EncounterRuleDef& def : system.encounters) {
        RuleRuntime r;
        r.name           = def.name;
        r.max_concurrent = std::max(0, def.max_concurrent);
        r.spawn_interval = std::max(0.5f, def.spawn_interval);
        const AIState seed = ship_ai::from_name(def.initial_ai_state);
        r.initial_state  = (seed == AIState::Count) ? AIState::Patrol : seed;

        // Region resolution. Unknown / unresolvable regions degrade to
        // Anywhere (a roaming rule) rather than silently never firing.
        if (def.region == "belt") {
            if (def.field_index >= 0 &&
                def.field_index < (int)system.asteroid_fields.size()) {
                const AsteroidFieldDef& f = system.asteroid_fields[def.field_index];
                r.region      = Region::Belt;
                r.belt_center = f.center;
                r.belt_half   = f.half_extent;
            } else {
                std::fprintf(stderr, "[encounter] rule '%s': belt field_index %d "
                             "out of range — treating as 'anywhere'\n",
                             def.name.c_str(), def.field_index);
                r.region = Region::Anywhere;
            }
        } else if (def.region == "lane") {
            r.region = Region::Lane;
            for (const std::string& nav_name : def.lane) {
                for (const NavPointDef& n : system.nav_points) {
                    if (n.name == nav_name) { r.lane_points.push_back(n.position); break; }
                }
            }
            if (r.lane_points.size() < 2) {
                std::fprintf(stderr, "[encounter] rule '%s': lane resolved %zu/%zu "
                             "nav points (<2) — treating as 'anywhere'\n",
                             def.name.c_str(), r.lane_points.size(), def.lane.size());
                r.region = Region::Anywhere;
            }
        } else {
            r.region = Region::Anywhere;
        }

        const float fsum = build_cum(def.factions, r.faction_names, r.faction_cum);
        const float csum = build_cum(def.classes,  r.class_names,   r.class_cum);
        if (fsum <= 0.0f || csum <= 0.0f) {
            std::fprintf(stderr, "[encounter] rule '%s': empty faction or class mix "
                         "— skipped\n", def.name.c_str());
            continue;
        }

        // Stagger the first spawn so multiple rules don't all fire on
        // frame one: start the timer most of the way to the interval.
        r.timer = r.spawn_interval * 0.5f;
        g_dir.rules.push_back(std::move(r));
    }

    g_dir.active = !g_dir.rules.empty();
    if (g_dir.active) {
        std::printf("[encounter] director armed: %zu rule(s), budget %d ships\n",
                    g_dir.rules.size(), k_director_max);
    }
}

void shutdown() {
    g_dir.rules.clear();
    g_dir.active = false;
}

int population() { return total_managed(); }

void tick(const ShipRegistry& ships, HMM_Vec3 player_pos, float dt,
          const SpawnFn& spawn, const DespawnFn& despawn) {
    if (!g_dir.active) return;

    for (RuleRuntime& r : g_dir.rules) {
        // --- prune + retire pass ------------------------------------------
        // Walk this rule's ids: drop any whose ship is gone (despawned by
        // combat death elsewhere, etc.), and retire any that have drifted
        // past the keep-alive bubble while NOT actively fighting. Engaged /
        // breaking-off ships are immune — we never yank a ship out of a
        // fight the player can see.
        auto it = r.managed_ids.begin();
        while (it != r.managed_ids.end()) {
            const Ship* s = ships.find_by_id(*it);
            if (!s || !s->alive) {
                it = r.managed_ids.erase(it);
                continue;
            }
            const float d = len(HMM_SubV3(s->position, player_pos));
            const bool engaged = s->ai.state == AIState::Engage ||
                                 s->ai.state == AIState::BreakOff;
            if (d > k_despawn_radius && !engaged) {
                const uint32_t id = *it;
                it = r.managed_ids.erase(it);
                despawn(id);
                std::printf("[encounter] despawned %s @ %.0fkm (drifted), pop %d/%d\n",
                            r.name.c_str(), d / 1000.0f, total_managed(), k_director_max);
            } else {
                ++it;
            }
        }

        // --- spawn pass ---------------------------------------------------
        r.timer += dt;
        if (r.timer < r.spawn_interval)                       continue;
        // Budget/cap gates RESET the timer to 0 (not hold it at the
        // threshold): otherwise the moment a ship dies and frees a slot the
        // very next tick spawns a replacement — encounters appeared to
        // respawn instantly. Resetting forces a full spawn_interval delay
        // after a death before the replacement shows up. The region gate
        // still HOLDS at the threshold so arriving in a region spawns
        // promptly rather than after a dead wait.
        if ((int)r.managed_ids.size() >= r.max_concurrent)    { r.timer = 0.0f; continue; }
        if (total_managed() >= k_director_max)                { r.timer = 0.0f; continue; }
        if ((int)ships.size() >= k_registry_hard_cap)         { r.timer = 0.0f; continue; }
        if (!region_armed(r, player_pos))                     { r.timer = r.spawn_interval; continue; }

        r.timer = 0.0f;   // attempt consumed regardless of success

        SpawnRequest req;
        req.class_name = weighted_pick(r.class_names,   r.class_cum,   g_dir.rng);
        const std::string fac_name = weighted_pick(r.faction_names, r.faction_cum, g_dir.rng);
        req.faction = faction::from_name(fac_name);
        if (req.class_name.empty() || req.faction == Faction::Count) continue;

        req.position        = pick_spawn_point(r, player_pos, g_dir.rng);
        req.initial_ai_state = r.initial_state;
        req.patrol_anchor   = req.position;   // loiter / flee-home tether at spawn

        const uint32_t id = spawn(req);
        if (id != 0) {
            r.managed_ids.push_back(id);
            const float km = len(HMM_SubV3(req.position, player_pos)) / 1000.0f;
            std::printf("[encounter] spawned %s %s @ %.0fkm, pop %d/%d\n",
                        fac_name.c_str(), req.class_name.c_str(), km,
                        total_managed(), k_director_max);
        }
    }
}

} // namespace encounters

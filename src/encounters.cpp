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
#include "ship_class.h"
#include "ship_registry.h"
#include "system_def.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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

// One active combat mission's forced wing: the mission id we key off (so we
// spawn it exactly once) and the registry ids we brought in. Lives ALONGSIDE
// the rule director but is NOT a spawn rule — keeping it separate is what lets
// abandoning the mission (sync_active_missions drops the track) remove the
// bias without touching ambient traffic.
struct MissionTrack {
    std::string                     mission_id;
    std::vector<uint32_t>           ids;
    std::unordered_set<std::string>  triggered;   // objective_keys already rolled/spawned
    std::unordered_set<std::string>  yielded;     // objective_keys that actually spawned >0
    int                             arm_attempts = 0;  // guarantee_full top-up tries (anti-hammer)
};

struct DirectorState {
    bool                      active = false;
    std::vector<RuleRuntime>  rules;
    std::vector<MissionTrack> mission_forces;   // #14: per-mission forced wings
    std::mt19937              rng{ 0xE2C0DE11u };   // fixed seed: reproducible feel
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

// ---- mission force: faction typical-fighter weighting (#14) ------------------

// Per-faction fallback fighter when the system has no native presence for
// that faction (a Pirate bounty issued in a Confed system still needs a
// hull). Mirrors the hulls each faction actually fields in the nav tables.
const char* fallback_fighter(Faction f) {
    switch (f) {
    case Faction::Pirate:   return "talon";
    case Faction::Retro:    return "talon";
    case Faction::Kilrathi: return "dralthi";
    case Faction::Confed:   return "stiletto";
    case Faction::Militia:  return "talon";
    case Faction::Hunter:   return "demon";
    case Faction::Merchant: return "tarsus";
    case Faction::Civilian: return "tarsus";
    default:                return "talon";
    }
}

// Build a weighted ship-class table for `faction` from the SAME spawn data
// ambient traffic uses: per-nav encounter groups (weighted by group chance x
// member count) plus any legacy rule class mixes whose faction list includes
// `faction`. names/weights are parallel. Falls back to the per-faction
// default fighter when the system has no native presence for the faction so
// a mission target is never left without a hull.
void build_faction_fighter_table(const StarSystem& system, Faction faction,
                                 std::vector<std::string>& names,
                                 std::vector<float>& weights) {
    auto add = [&](const std::string& cls, float w) {
        if (cls.empty() || w <= 0.0f) return;
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == cls) { weights[i] += w; return; }
        names.push_back(cls);
        weights.push_back(w);
    };

    // Per-nav wcnews tables (the canonical ambient model).
    for (const NavPointDef& nav : system.nav_points)
        for (const EncounterGroupDef& g : nav.encounters)
            for (const EncounterMemberDef& m : g.members)
                if (faction::from_name(m.faction) == faction)
                    add(m.ship_class, std::max(g.chance, 1.0f) * (float)std::max(1, m.count));

    // Legacy rule class mixes (only when the rule can field this faction).
    for (const EncounterRuleDef& rule : system.encounters) {
        bool fields = false;
        for (const EncounterWeight& fw : rule.factions)
            if (fw.weight > 0.0f && faction::from_name(fw.name) == faction) { fields = true; break; }
        if (!fields) continue;
        for (const EncounterWeight& cw : rule.classes)
            add(cw.name, std::max(cw.weight, 1.0f));
    }

    if (names.empty()) add(fallback_fighter(faction), 1.0f);
}

// Weighted draw over a parallel name/weight table (non-cumulative). Empty
// table -> "".
const std::string& weighted_pick_w(const std::vector<std::string>& names,
                                   const std::vector<float>& weights,
                                   std::mt19937& rng) {
    static const std::string empty;
    if (names.empty()) return empty;
    float total = 0.0f;
    for (float w : weights) total += w;
    if (total <= 0.0f) return names.front();
    std::uniform_real_distribution<float> u(0.0f, total);
    float roll = u(rng);
    for (size_t i = 0; i < names.size(); ++i) {
        roll -= weights[i];
        if (roll <= 0.0f) return names[i];
    }
    return names.back();
}

// A spawn point for ONE mission-force member: a random point on the 8-15 km
// shell around the objective `anchor`, pushed clear of the player's own
// min-spawn shell (so nothing pops in on screen even when the player is
// loitering at the objective) and kept a fighter's-berth from earlier
// members. Best-effort: returns the least-crowded candidate if 8 tries can't
// satisfy separation, so the spawn never stalls.
HMM_Vec3 mission_spawn_point(HMM_Vec3 anchor, HMM_Vec3 player, std::mt19937& rng,
                            const std::vector<HMM_Vec3>& placed) {
    constexpr float k_min_sep = 600.0f;   // fighters: don't spawn on top of each other
    std::uniform_real_distribution<float> ud(k_spawn_dist_min, k_spawn_dist_max);
    HMM_Vec3 best = anchor;
    float    best_slack = -1e30f;
    for (int tries = 0; tries < 8; ++tries) {
        const HMM_Vec3 dir = random_unit_dir(rng);
        HMM_Vec3 p = HMM_AddV3(anchor, HMM_MulV3F(dir, ud(rng)));
        // Never inside the player's min-spawn shell (no visible pop-in).
        const HMM_Vec3 to = HMM_SubV3(p, player);
        const float    d  = len(to);
        if (d < k_spawn_dist_min) {
            const HMM_Vec3 u = (d > 1e-3f) ? HMM_DivV3F(to, d) : dir;
            p = HMM_AddV3(player, HMM_MulV3F(u, k_spawn_dist_min + 1000.0f));
        }
        float worst = 1e30f;
        for (const HMM_Vec3& q : placed)
            worst = std::min(worst, len(HMM_SubV3(p, q)) - k_min_sep);
        if (worst >= 0.0f) return p;
        if (worst > best_slack) { best_slack = worst; best = p; }
    }
    return best;
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
    g_dir.mission_forces.clear();   // #14: a new system re-arms missions fresh
    g_dir.active = false;
}

int population() { return total_managed(); }

// ---- wcnews per-nav encounter model (roll once on entry, no refill) --------
void populate_on_entry(const StarSystem& system, HMM_Vec3 player_pos,
                       HMM_Vec3 sun_pos, const SpawnFn& spawn) {
    // Seed RNG from wall-clock time so each fresh process gets a different
    // spawn at, say, Achilles — and from a per-process counter so visiting
    // a different system on the same launch also varies the roll. The wall
    // clock prevents the "every launch looks identical" bug; the counter
    // keeps two back-to-back visits in the same process from re-rolling
    // the same nav tables.
    static uint32_t    s_entry = 0;
    static const auto  s_t0 = std::chrono::steady_clock::now().time_since_epoch().count();
    ++s_entry;
    const uint32_t seed = (uint32_t)(s_t0 ^ (s_entry * 2654435761u) ^ 0x9E3779B9u);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);

    int spawned = 0, nav_tables = 0, convoys = 0;

    // Spawn a formed-up group at `center`: the FIRST ship becomes the lead
    // (a Traveler that cruises the lanes, or a Loiterer that circles when
    // `prefer_loiter`), and every other ship becomes an Escort holding a
    // formation offset off that lead -- which is what makes wings + convoys
    // move together. `mem` is a flat (faction,class) list. Members landing
    // inside the player's min-spawn shell get pushed out so nothing pops in
    // point-blank. Returns the lead's id (0 if the lead failed to spawn).
    auto spawn_group = [&](const std::vector<std::pair<Faction, std::string>>& mem,
                           HMM_Vec3 center, bool prefer_loiter) -> uint32_t {
        uint32_t lead_id = 0;
        // Track positions of prior members so we can keep new spawns a
        // safe distance away -- otherwise a wide wing spawns its members
        // in random 600-2000 unit offsets and a 525m Drayman + an 80m Talon
        // overlap, ram and instantly explode (np-3dp). Min separation is
        // generous -- spaced convoys read better visually too.
        std::vector<HMM_Vec3> placed;
        std::vector<bool>     placed_capital;   // parallel: was placed[j] a capital hull?
        placed.reserve(mem.size());
        placed_capital.reserve(mem.size());
        constexpr float k_min_sep_m   = 350.0f;     // min sep between group members
        constexpr float k_min_sep_pad = 120.0f;    // extra clearance beyond ship size
        constexpr float k_cap_sep_m   = 2000.0f;    // min sep when EITHER member is a capital
        for (size_t k = 0; k < mem.size(); ++k) {
            if (spawned >= k_entry_population_max) break;
            // Capital hulls (Drayman/Paradigm/Kamekh) need a much wider berth
            // from each other -- a hull-to-hull capital ram is an instant
            // double KO and reads terribly. Flag is data-driven (ship.json).
            const ShipClass* mem_class = ship_class::find(mem[k].second);
            const bool cur_capital = mem_class && mem_class->capital;
            // Pick a candidate: random scatter in a wider shell, then nudge
            // outward so the lead sits in clear space.
            HMM_Vec3 off = HMM_V3(U(rng) * 2 - 1, U(rng) * 2 - 1, U(rng) * 2 - 1);
            const float ol = len(off);
            off = (ol > 1e-3f) ? HMM_MulV3F(off, (1400.0f + U(rng) * 1600.0f) / ol)
                               : HMM_V3(1500.0f, 0, 0);
            HMM_Vec3 pos = HMM_AddV3(center, off);
            const HMM_Vec3 to_player = HMM_SubV3(pos, player_pos);
            const float dp = len(to_player);
            if (dp < k_spawn_dist_min) {
                const HMM_Vec3 dir = (dp > 1e-3f) ? HMM_MulV3F(to_player, 1.0f / dp)
                                                  : HMM_V3(0, 0, 1);
                pos = HMM_AddV3(player_pos, HMM_MulV3F(dir, k_spawn_dist_min + 1500.0f));
            }
            // Enforce min separation from previously-placed members. The
            // required gap is PER PAIR: capitals demand k_cap_sep_m from any
            // other capital, everyone else uses the crowd-scaled base gap.
            // Up to 8 jitter attempts before giving up -- if we can't fit,
            // accept the best candidate we found so the spawn doesn't stall.
            const float base_need = k_min_sep_m + k_min_sep_pad * (float)placed.size();
            auto pair_need = [&](size_t j) {
                return (cur_capital || placed_capital[j]) ? k_cap_sep_m : base_need;
            };
            HMM_Vec3 best = pos;
            float best_slack = -1e30f;   // worst (dist - need) across placed; higher = better
            for (int tries = 0; tries < 8; ++tries) {
                bool  ok    = true;
                float worst = 1e30f;     // min slack vs. any placed member
                for (size_t j = 0; j < placed.size(); ++j) {
                    const float slack = len(HMM_SubV3(pos, placed[j])) - pair_need(j);
                    worst = std::min(worst, slack);
                    if (slack < 0.0f) ok = false;
                }
                if (ok) break;                          // satisfies every pair
                if (worst > best_slack) { best_slack = worst; best = pos; }
                // jitter outward away from the nearest blocker. Scale the
                // nudge by the largest gap we might need so capitals get
                // shoved far enough in one hop.
                const float step = cur_capital ? k_cap_sep_m : base_need;
                HMM_Vec3 nudge = HMM_V3(U(rng) - 0.5f, U(rng) - 0.5f, U(rng) - 0.5f);
                const float nl = len(nudge);
                nudge = (nl > 1e-3f) ? HMM_MulV3F(nudge, step / nl)
                                      : HMM_V3(step, 0, 0);
                pos = HMM_AddV3(pos, nudge);
                if (tries == 7) pos = best;             // exhausted: take the best found
            }
            SpawnRequest req;
            req.class_name       = mem[k].second;
            req.faction          = mem[k].first;
            req.position         = pos;
            req.initial_ai_state = AIState::Patrol;
            req.patrol_anchor    = center;
            if (k == 0) {
                req.civ_role = (prefer_loiter && U(rng) < 0.5f) ? CivRole::Loiter
                                                                : CivRole::Traveler;
            } else {
                req.civ_role          = CivRole::Escort;
                req.formation_lead_id = lead_id;   // 0 -> escort falls back to Traveler
                const float a = (float)k * 2.39996323f;          // golden-angle scatter
                const float R = 1300.0f + 450.0f * (float)(k / 2);
                req.formation_offset = HMM_V3(std::cos(a) * R,
                                              (k % 2) ? 350.0f : -350.0f,
                                              std::sin(a) * R);
            }
            const uint32_t id = spawn(req);
            if (id != 0) {
                ++spawned;
                if (k == 0) lead_id = id;
                placed.push_back(req.position);   // remember for sep check on later members
                placed_capital.push_back(cur_capital);
            }
        }
        return lead_id;
    };

    // 1. Per-nav wcnews tables: roll ONE group per nav, spawn it formed-up.
    for (const NavPointDef& nav : system.nav_points) {
        if (nav.encounters.empty()) continue;
        ++nav_tables;

        float total = 0.0f;
        for (const auto& g : nav.encounters) total += std::max(0.0f, g.chance);
        if (total <= 0.0f) continue;
        float r = U(rng) * total;
        const EncounterGroupDef* chosen = &nav.encounters.front();
        for (const auto& g : nav.encounters) {
            r -= std::max(0.0f, g.chance);
            if (r <= 0.0f) { chosen = &g; break; }
        }

        std::vector<std::pair<Faction, std::string>> mem;
        for (const EncounterMemberDef& m : chosen->members) {
            const Faction fac = faction::from_name(m.faction);
            if (fac == Faction::Count) continue;
            for (int c = 0; c < m.count; ++c) mem.emplace_back(fac, m.ship_class);
        }
        // Groups parked at a base/planet tend to loiter; lane/gate groups travel.
        const bool loiter = (nav.kind == "station" || nav.kind == "planet");
        // Keep traffic OUT of the landing zone (np-3dp.22): a dockable
        // base spawns its group 7.5-10k toward the system centre (sun)
        // instead of right on the pad, so the auto-land approach stays
        // clear and you fly IN toward the base through open space. The
        // patrol anchor follows the offset centre, so they loiter out
        // there too, not back at the base.
        HMM_Vec3 spawn_center = nav.position;
        if (nav.dockable && !nav.base_id.empty()) {
            HMM_Vec3 to_sun = HMM_SubV3(sun_pos, nav.position);
            const float d = len(to_sun);
            if (d > 1.0f) {
                to_sun = HMM_MulV3F(to_sun, 1.0f / d);
                const float off = 7500.0f + U(rng) * 2500.0f;   // 7.5-10k
                spawn_center = HMM_AddV3(nav.position, HMM_MulV3F(to_sun, off));
            }
        }
        // Jump gates get pushed 15-22k off the nav so combat traffic
        // doesn't camp the gate and ambush / chatter over the player
        // on arrival (same idea as the base offset above, bigger).
        if (nav.kind == "jump") {
            HMM_Vec3 away = HMM_SubV3(sun_pos, nav.position);
            const float d = len(away);
            if (d > 1.0f) {
                away = HMM_MulV3F(away, 1.0f / d);
                const float off = 15000.0f + U(rng) * 7000.0f;  // 15-22k
                spawn_center = HMM_AddV3(nav.position, HMM_MulV3F(away, off));
            }
        }
        spawn_group(mem, spawn_center, loiter);
    }

    // 2. Occasional BIG merchant convoy crossing the system -- a fat, mostly
    //    unescorted target a pirate-aligned player can hunt (future loot
    //    tables make this a payday). Drayman/Galaxy/Tarsus -> our merchant
    //    hulls; 3-5 haulers + 1-2 militia escorts, spawned at a jump gate
    //    ("just jumped in") and traveling the lanes via the lead Traveler.
    if (U(rng) < 0.45f && !system.nav_points.empty() &&
        spawned + 4 <= k_entry_population_max) {
        HMM_Vec3 origin = system.nav_points.front().position;
        for (const NavPointDef& n : system.nav_points)
            if (n.kind == "jump") { origin = n.position; break; }
        std::vector<std::pair<Faction, std::string>> conv;
        const char* hulls[] = { "drayman", "galaxy", "tarsus" };
        const int nmerch = 3 + (int)(U(rng) * 3.0f);   // 3-5 haulers
        for (int i = 0; i < nmerch; ++i) conv.emplace_back(Faction::Merchant, hulls[i % 3]);
        const int nesc = 1 + (int)(U(rng) * 2.0f);     // 1-2 escorts
        for (int i = 0; i < nesc; ++i) conv.emplace_back(Faction::Militia, "talon");
        spawn_group(conv, origin, /*prefer_loiter*/false);
        ++convoys;
    }

    std::printf("[encounter] system entry roll: %d nav table(s), %d convoy(s), "
                "spawned %d ship(s) (no refill until next entry/launch)\n",
                nav_tables, convoys, spawned);
}

// ---- mission-driven forced spawns (#14) ------------------------------------

std::vector<std::string> faction_fighter_classes(const StarSystem& system,
                                                 Faction faction) {
    std::vector<std::string> names;
    std::vector<float>       weights;
    build_faction_fighter_table(system, faction, names, weights);
    return names;
}

namespace {
// Find the track for `mission_id`, or nullptr if none exists yet.
MissionTrack* find_track(const std::string& mission_id) {
    for (MissionTrack& t : g_dir.mission_forces)
        if (t.mission_id == mission_id) return &t;
    return nullptr;
}
// Find-or-create the track for `mission_id`. Returns a reference into
// g_dir.mission_forces (stable for the rest of this call).
MissionTrack& find_or_create_track(const std::string& mission_id) {
    if (MissionTrack* t = find_track(mission_id)) return *t;
    MissionTrack t;
    t.mission_id = mission_id;
    g_dir.mission_forces.push_back(std::move(t));
    return g_dir.mission_forces.back();
}
} // namespace

int ensure_mission_force(const StarSystem& system, const MissionForce& mf,
                         HMM_Vec3 player_pos, const SpawnFn& spawn) {
    if (mf.count <= 0 || mf.faction == Faction::Count) return 0;

    // Per-objective one-time gate: if this objective already triggered
    // (spawned or pre-rolled-miss), don't spawn again. A Patrol with navs
    // A/B/C keys on the nav NAME so each gets its own one-time spawn.
    MissionTrack& track = find_or_create_track(mf.mission_id);
    if (track.triggered.count(mf.objective_key)) return 0;

    const int ids_before = (int)track.ids.size();   // for per-call yield accounting
    std::vector<std::string> names;
    std::vector<float>       weights;
    build_faction_fighter_table(system, mf.faction, names, weights);

    const int target = std::min(mf.count, k_mission_force_max);

    // Spawn one ship, retrying the class pick up to a few times so a single
    // bad entry in the faction's fighter table (an unregistered class or a
    // missing atlas -> encounter_spawn returns 0) doesn't strand the slot.
    // Falls back to the first table entry if weighting rolls nothing. Returns
    // the new id, or 0 if every pick failed to spawn.
    auto spawn_one = [&](std::vector<HMM_Vec3>& placed) -> uint32_t {
        for (int tries = 0; tries < 4; ++tries) {
            SpawnRequest req;
            req.class_name = weighted_pick_w(names, weights, g_dir.rng);
            if (req.class_name.empty()) {
                if (!names.empty()) req.class_name = names.front();
                else return 0;
            }
            req.faction          = mf.faction;
            req.position         = mission_spawn_point(mf.anchor, player_pos,
                                                       g_dir.rng, placed);
            // Seed Patrol anchored on the objective; perception flips them to
            // Engage the moment the (hostile) player is in range. Same as ambient.
            req.initial_ai_state = AIState::Patrol;
            req.patrol_anchor    = mf.anchor;
            const uint32_t id = spawn(req);
            if (id != 0) {
                placed.push_back(req.position);
                return id;
            }
        }
        return 0;
    };

    if (mf.guarantee_full) {
        // GIVEN types (Attack/DefendBase/Bounty): keep the objective armed to
        // `target`. Spawn only the deficit (so combat losses after full arming
        // are NOT replaced — the goal is to kill what was there). Mark
        // triggered only once `target` are live; until then leave it
        // un-triggered so a partial first call (e.g. a bad class pick) is
        // topped up next frame. Bounded by arm_attempts so a hard failure
        // (faction has no spawnable class at all) eventually stops retrying.
        const int live = (int)track.ids.size();
        const int deficit = target - live;
        if (deficit <= 0) {
            track.triggered.insert(mf.objective_key);   // fully armed — one-time now
            return 0;
        }
        std::vector<HMM_Vec3> placed;
        for (int i = 0; i < deficit; ++i) {
            const uint32_t id = spawn_one(placed);
            if (id != 0) track.ids.push_back(id);
        }
        const int live_now = (int)track.ids.size();
        const int spawned_this_call = live_now - ids_before;
        if (spawned_this_call > 0) track.yielded.insert(mf.objective_key);
        if (live_now >= target) {
            track.triggered.insert(mf.objective_key);   // armed to target — done
        } else {
            // Still short. Retry next frame unless we've burned through the
            // attempt budget — then give up (mark one-time) so we don't hammer.
            if (++track.arm_attempts >= k_max_arm_attempts) {
                track.triggered.insert(mf.objective_key);
                std::fprintf(stderr,
                    "[encounter] mission '%s' [%s]: gave up arming after %d "
                    "attempts (%d/%d %s spawned) — target class unavailable\n",
                    mf.mission_id.c_str(), mf.objective_key.c_str(),
                    track.arm_attempts, live_now, target,
                    faction::to_name(mf.faction));
            }
        }
        std::printf("[encounter] mission '%s' [%s]: forced %d/%d %s hostile(s) at objective\n",
                    mf.mission_id.c_str(), mf.objective_key.c_str(),
                    spawned_this_call, target, faction::to_name(mf.faction));
        return spawned_this_call;
    }

    // RANDOM types (Scout/Patrol): spawn up to `target`, mark one-time
    // regardless (a partial roll is acceptable — enemies "may not show up").
    std::vector<HMM_Vec3> placed;
    placed.reserve(target);
    for (int i = 0; i < target; ++i) {
        const uint32_t id = spawn_one(placed);
        if (id != 0) track.ids.push_back(id);
    }

    track.triggered.insert(mf.objective_key);
    const int spawned_this_call = (int)track.ids.size() - ids_before;
    if (spawned_this_call > 0)
        track.yielded.insert(mf.objective_key);
    std::printf("[encounter] mission '%s' [%s]: forced %d/%d %s hostile(s) at objective\n",
                mf.mission_id.c_str(), mf.objective_key.c_str(),
                spawned_this_call, target, faction::to_name(mf.faction));
    return spawned_this_call;
}

void mark_mission_objective_triggered(const std::string& mission_id,
                                      const std::string& objective_key) {
    MissionTrack& track = find_or_create_track(mission_id);
    track.triggered.insert(objective_key);
}

bool mission_objective_triggered(const std::string& mission_id,
                                 const std::string& objective_key) {
    const MissionTrack* t = find_track(mission_id);
    return t && t->triggered.count(objective_key);
}

int mission_yielded_count(const std::string& mission_id) {
    const MissionTrack* t = find_track(mission_id);
    return t ? (int)t->yielded.size() : 0;
}

void sync_active_missions(const std::vector<std::string>& active_ids) {
    if (g_dir.mission_forces.empty()) return;
    std::unordered_set<std::string> live(active_ids.begin(), active_ids.end());
    for (auto it = g_dir.mission_forces.begin(); it != g_dir.mission_forces.end(); ) {
        if (!live.count(it->mission_id)) {
            std::printf("[encounter] mission '%s': force bookkeeping dropped "
                        "(abandoned/completed/left) — no further mission spawns\n",
                        it->mission_id.c_str());
            it = g_dir.mission_forces.erase(it);
        } else {
            ++it;
        }
    }
}

void prune_mission_tracks(const ShipRegistry& ships) {
    for (MissionTrack& t : g_dir.mission_forces) {
        auto it = t.ids.begin();
        while (it != t.ids.end()) {
            const Ship* s = ships.find_by_id(*it);
            if (!s || !s->alive)
                it = t.ids.erase(it);
            else
                ++it;
        }
    }
}

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

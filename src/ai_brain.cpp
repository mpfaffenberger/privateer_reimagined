// -----------------------------------------------------------------------------
// ai_brain.cpp -- data-driven condition->maneuver combat brain.
//
// Clean-room reimplementation (Apache-2.0) of the architecture shared by
// vanilla Privateer's decoded MNVR maneuver-object tree and PGG/Vega Strike's
// XML condition->script AggressiveAI. Patterns/ideas borrowed; NO GPL code
// copied. All numbers come from docs/ai_model.md s0 (the vanilla decode).
// See docs/ai_maneuver_system.md for the full design + license note.
//
// Three parts:
//   1. Registry  -- load assets/ai/*.ai.json tables, look up by faction.
//   2. Evaluator -- per tick, run the interrupt then logic channels to select
//                   a maneuver, hold it for its duration, execute it.
//   3. Maneuvers -- the catalog handlers, writing s.behavior / s.controller.
//                   The first four are ports of the existing ship_ai math.
// -----------------------------------------------------------------------------

#include "ai_brain.h"

#include "armor.h"
#include "aim.h"
#include "comm.h"
#include "faction.h"
#include "gun.h"
#include "shield.h"
#include "json.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"
#include "ship_sprite.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

// ====================================================================== //
//  Decoded vanilla constants (raw 1:1 world units; docs/ai_model.md s0)   //
// ====================================================================== //
namespace {

// Single rescale knob for the whole distance stack (1:1 today, s7.12).
constexpr float kPvtScale = 1.0f;

// Pursue -> attack-run boundary (s9.4.2: D <= 1000).
constexpr float k_pursue_attack_switch = 1000.0f * kPvtScale;

// NOTE on the decoded attack-run window (s9.4.2: fire tick 76, done 153): the
// absolute AI tick rate dt (DS:0x2768) is still [?] (s9.3), so converting those
// ticks to seconds is untrustworthy -- the old 7.6s..15.3s gate starved the
// guns (a ship was interrupted long before its fire window opened). We instead
// fire on GEOMETRY (in arc + within gun range + a short reaction gate); see
// should_fire / the AttackRun handler. The tick numbers stay [?] until dt is
// pinned by a dynamic trace.

// Random-gated maneuvers (the occasional evade) re-roll the Random01 metric on
// this fixed decision cadence rather than every render frame, so a JSON window
// [0,p] means ~p probability PER DECISION (~k_decision_hz/sec), independent of
// framerate. Without this the per-frame roll fired ~84%/sec at 60 fps and ships
// evaded continuously. 5 Hz is a deliberate decision cadence, NOT a claim about
// vanilla's dt.
constexpr float k_decision_hz = 5.0f;

// Throttle while a ship has a firing solution: slow to ~50% for time-on-target
// (vanilla pilots ease off the gas to keep guns on the mark). 0.4-0.6 reads
// right; 0.5 is the middle. Applied in LeadPursuit (when firing) + AttackRun.
constexpr float k_fire_throttle = 0.5f;

// EVADE/jink magnitude (s9.4.3): jitter is keyed from f3.
constexpr float k_evade_f3_default = 75.0f;

// Comm/taunt bark cooldown (s11.1).
constexpr float k_bark_cooldown_s = 6.0f;

// How far out maneuver aim points are projected (the controller chases a
// world point; sizing it past any reachable distance keeps it from
// decelerating on "arrival").
constexpr float k_aim_distance   = 20000.0f;
constexpr float k_flee_distance  = 50000.0f;

constexpr float kPi = 3.14159265358979323846f;

} // namespace

// ====================================================================== //
//  ai_maneuver:: name maps (defined here to avoid a 4th translation unit) //
// ====================================================================== //
namespace ai_maneuver {

const char* condition_name(AICondition c) {
    switch (c) {
        case AICondition::DistanceToTarget: return "distance";
        case AICondition::HullFraction:     return "hull";
        case AICondition::ShieldFraction:   return "shield";
        case AICondition::TargetInFront:    return "target_in_front";
        case AICondition::TargetFacingMe:   return "target_facing_me";
        case AICondition::TargetInRange:    return "target_in_range";
        case AICondition::Random01:         return "random";
        case AICondition::ClosingRate:      return "closing_rate";
        case AICondition::TimeInState:      return "time_in_state";
        default:                            return "?";
    }
}
AICondition condition_from_name(std::string_view s) {
    if (s == "distance")          return AICondition::DistanceToTarget;
    if (s == "hull")              return AICondition::HullFraction;
    if (s == "shield")            return AICondition::ShieldFraction;
    if (s == "target_in_front")   return AICondition::TargetInFront;
    if (s == "target_facing_me")  return AICondition::TargetFacingMe;
    if (s == "target_in_range")   return AICondition::TargetInRange;
    if (s == "random")            return AICondition::Random01;
    if (s == "closing_rate")      return AICondition::ClosingRate;
    if (s == "time_in_state")     return AICondition::TimeInState;
    return AICondition::Count;
}

const char* maneuver_name(AIManeuver m) {
    switch (m) {
        case AIManeuver::LeadPursuit:       return "lead_pursuit";
        case AIManeuver::AttackRun:         return "attack_run";
        case AIManeuver::BreakOff:          return "break_off";
        case AIManeuver::EvadeJink:         return "evade_jink";
        case AIManeuver::BarrelRoll:        return "barrel_roll";
        case AIManeuver::LoopAround:        return "loop_around";
        case AIManeuver::EvadeLeftRight:    return "evade_left_right";
        case AIManeuver::EvadeUpDown:       return "evade_up_down";
        case AIManeuver::FacePerpendicular: return "face_perpendicular";
        case AIManeuver::TurnAway:          return "turn_away";
        case AIManeuver::MatchSpeed:        return "match_speed";
        case AIManeuver::FleeHome:          return "flee_home";
        default:                            return "?";
    }
}
AIManeuver maneuver_from_name(std::string_view s) {
    if (s == "lead_pursuit")       return AIManeuver::LeadPursuit;
    if (s == "attack_run")         return AIManeuver::AttackRun;
    if (s == "break_off")          return AIManeuver::BreakOff;
    if (s == "evade_jink")         return AIManeuver::EvadeJink;
    if (s == "barrel_roll")        return AIManeuver::BarrelRoll;
    if (s == "loop_around")        return AIManeuver::LoopAround;
    if (s == "evade_left_right")   return AIManeuver::EvadeLeftRight;
    if (s == "evade_up_down")      return AIManeuver::EvadeUpDown;
    if (s == "face_perpendicular") return AIManeuver::FacePerpendicular;
    if (s == "turn_away")          return AIManeuver::TurnAway;
    if (s == "match_speed")        return AIManeuver::MatchSpeed;
    if (s == "flee_home")          return AIManeuver::FleeHome;
    return AIManeuver::Count;
}

AIScalarToken token_from_name(std::string_view s) {
    if (s == "f0")             return AIScalarToken::F0;
    if (s == "f0_radii")       return AIScalarToken::F0Radii;
    if (s == "f1")             return AIScalarToken::F1;
    if (s == "f2")             return AIScalarToken::F2;
    if (s == "f3")             return AIScalarToken::F3;
    if (s == "f6")             return AIScalarToken::F6;
    if (s == "pursue_switch")  return AIScalarToken::PursueSwitch;
    if (s == "gun_range")      return AIScalarToken::GunRange;
    if (s == "flee_threshold") return AIScalarToken::FleeThreshold;
    if (s == "sensor")         return AIScalarToken::Sensor;
    return AIScalarToken::Literal;
}
const char* token_name(AIScalarToken t) {
    switch (t) {
        case AIScalarToken::F0:            return "f0";
        case AIScalarToken::F0Radii:       return "f0_radii";
        case AIScalarToken::F1:            return "f1";
        case AIScalarToken::F2:            return "f2";
        case AIScalarToken::F3:            return "f3";
        case AIScalarToken::F6:            return "f6";
        case AIScalarToken::PursueSwitch:  return "pursue_switch";
        case AIScalarToken::GunRange:      return "gun_range";
        case AIScalarToken::FleeThreshold: return "flee_threshold";
        case AIScalarToken::Sensor:        return "sensor";
        default:                           return "literal";
    }
}

const char* maneuver_state_label(AIManeuver m) {
    switch (m) {
        case AIManeuver::FleeHome:       return "flee";
        case AIManeuver::BreakOff:
        case AIManeuver::EvadeJink:
        case AIManeuver::EvadeLeftRight:
        case AIManeuver::EvadeUpDown:
        case AIManeuver::TurnAway:       return "breakoff";
        default:                         return "engage";
    }
}

} // namespace ai_maneuver

// ====================================================================== //
//  Registry                                                              //
// ====================================================================== //
namespace {

std::vector<AILogicTable> g_tables;

// Parse a JSON scalar that may be a number (literal) or a string token.
AIScalar parse_scalar(const json::Value* v, float fallback) {
    AIScalar out;
    if (v && v->is_number()) { out.tok = AIScalarToken::Literal; out.lit = v->as_float(); return out; }
    if (v && v->is_string()) {
        out.tok = ai_maneuver::token_from_name(v->as_string());
        if (out.tok == AIScalarToken::Literal) {
            // unknown token string -> treat as 0, warn once at load
            std::fprintf(stderr, "[ai_brain] unknown scalar token '%s'\n", v->as_string().c_str());
        }
        return out;
    }
    out.tok = AIScalarToken::Literal; out.lit = fallback; return out;
}

AILogicItem parse_item(const json::Value& jv) {
    AILogicItem it;
    it.maneuver   = ai_maneuver::maneuver_from_name(jv.find("maneuver") && jv.find("maneuver")->is_string()
                                                    ? jv.find("maneuver")->as_string() : "");
    if (it.maneuver == AIManeuver::Count) it.maneuver = AIManeuver::LeadPursuit;
    if (const json::Value* p = jv.find("priority"); p && p->is_number()) it.priority = p->as_float();
    if (const json::Value* p = jv.find("duration"); p && p->is_number()) it.duration_s = p->as_float();
    if (it.duration_s <= 0.0f) it.duration_s = 1.0f;

    // Conditions: an array of {metric, min, max, not?}. Empty -> always passes.
    if (const json::Value* cs = jv.find("when"); cs && cs->is_array()) {
        for (const json::Value& cv : cs->as_array()) {
            if (!cv.is_object()) continue;
            AICondClause cl;
            const json::Value* mp = cv.find("metric");
            cl.cond = ai_maneuver::condition_from_name(mp && mp->is_string() ? mp->as_string() : "");
            if (cl.cond == AICondition::Count) continue;   // skip unknown metric
            cl.min = parse_scalar(cv.find("min"), 0.0f);
            cl.max = parse_scalar(cv.find("max"), 1e30f);
            if (const json::Value* n = cv.find("not"); n && n->is_bool()) cl.negate = n->as_bool();
            it.clauses.push_back(cl);
        }
    }
    return it;
}

void parse_channel(const json::Value* arr, std::vector<AILogicItem>& out) {
    if (!arr || !arr->is_array()) return;
    for (const json::Value& iv : arr->as_array())
        if (iv.is_object()) out.push_back(parse_item(iv));
}

void parse_tier(const json::Value& tv, AIMoraleTier& tier) {
    parse_channel(tv.find("logic"),     tier.logic);
    parse_channel(tv.find("interrupt"), tier.interrupt);
}

bool parse_table(const fs::path& path) {
    json::Value root = json::parse_file(path.string());
    if (!root.is_object()) {
        std::fprintf(stderr, "[ai_brain] '%s' is not a JSON object\n", path.string().c_str());
        return false;
    }
    AILogicTable t;
    t.name = (root.find("name") && root.find("name")->is_string())
           ? root.find("name")->as_string()
           : path.stem().string();
    // strip a trailing ".ai" if the stem carried it (e.g. "default.ai")
    if (t.name.size() > 3 && t.name.substr(t.name.size() - 3) == ".ai")
        t.name = t.name.substr(0, t.name.size() - 3);

    // tiers: object { timid:{}, steady:{}, fanatical:{} } OR array[3].
    if (const json::Value* tiers = root.find("tiers"); tiers && tiers->is_object()) {
        if (const json::Value* p = tiers->find("timid");     p) parse_tier(*p, t.tiers[0]);
        if (const json::Value* p = tiers->find("steady");    p) parse_tier(*p, t.tiers[1]);
        if (const json::Value* p = tiers->find("fanatical"); p) parse_tier(*p, t.tiers[2]);
    }
    g_tables.push_back(std::move(t));
    return true;
}

} // namespace

int ai_brain::load_all(const std::string& dir) {
    g_tables.clear();
    g_tables.reserve(16);
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "[ai_brain] dir '%s' is not a directory\n", dir.c_str());
        return 0;
    }
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string fn = entry.path().filename().string();
        if (fn.size() < 8 || fn.substr(fn.size() - 8) != ".ai.json") continue;
        if (parse_table(entry.path())) {
            const AILogicTable& t = g_tables.back();
            std::printf("[ai_brain] %-10s tiers: timid(%zu/%zu) steady(%zu/%zu) fanatical(%zu/%zu)\n",
                        t.name.c_str(),
                        t.tiers[0].logic.size(), t.tiers[0].interrupt.size(),
                        t.tiers[1].logic.size(), t.tiers[1].interrupt.size(),
                        t.tiers[2].logic.size(), t.tiers[2].interrupt.size());
        }
    }
    std::printf("[ai_brain] loaded %zu AI logic tables\n", g_tables.size());
    return (int)g_tables.size();
}

const AILogicTable* ai_brain::find(std::string_view name) {
    for (const auto& t : g_tables) if (t.name == name) return &t;
    return nullptr;
}
const AILogicTable* ai_brain::default_table() {
    if (const AILogicTable* d = find("default")) return d;
    return g_tables.empty() ? nullptr : &g_tables.front();
}
const AILogicTable* ai_brain::resolve_for_faction(Faction f) {
    if (const AILogicTable* t = find(faction::to_name(f))) return t;
    return default_table();
}

// ====================================================================== //
//  Shared health helpers                                                 //
// ====================================================================== //
float ai_brain::hp_fraction(const Ship& s) {
    if (!s.klass) return 1.0f;
    const float cur = s.armor_fore_cm  + s.armor_aft_cm  + s.armor_port_cm  + s.armor_starboard_cm
                    + s.shield_fore_cm + s.shield_aft_cm + s.shield_port_cm + s.shield_starboard_cm;
    float maxv = s.klass->armor_fore_cm + s.klass->armor_aft_cm + s.klass->armor_port_cm + s.klass->armor_starboard_cm;
    if (const ArmorType* fitted_armor = s.fitted_armor)
        maxv += fitted_armor->front_cm + fitted_armor->back_cm
              + fitted_armor->port_cm + fitted_armor->starboard_cm;
    if (s.klass->default_shield)
        maxv += (s.klass->default_shield->front_cm + s.klass->default_shield->back_cm
              +  s.klass->default_shield->port_cm + s.klass->default_shield->starboard_cm) * s.shield_mult;
    return (maxv > 0.0f) ? (cur / maxv) : 1.0f;
}

// f6 (morale/caution) -> flee HP fraction. LOW f6 = fanatical (Kilrathi 64 ->
// ~0.05), HIGH f6 = timid (merchant 128 -> ~0.50). docs/ai_model.md s0; [I]
// tuning (no resident consumer of f6, s7.13.5).
float ai_brain::flee_threshold_for(const Ship& s) {
    if (!s.klass) return 0.30f;
    constexpr float kF6Fanatical = 64.0f,  kFleeFanatical = 0.05f;
    constexpr float kF6Timid     = 128.0f, kFleeTimid     = 0.50f;
    const float t   = (s.klass->morale_f6 - kF6Fanatical) / (kF6Timid - kF6Fanatical);
    const float thr = kFleeFanatical + t * (kFleeTimid - kFleeFanatical);
    return std::clamp(thr, 0.02f, 0.60f);
}

namespace {

float shield_fraction(const Ship& s) {
    if (!s.klass || !s.klass->default_shield) return 1.0f;
    const float cur  = s.shield_fore_cm + s.shield_aft_cm + s.shield_port_cm + s.shield_starboard_cm;
    const float maxv = (s.klass->default_shield->front_cm + s.klass->default_shield->back_cm
                     +  s.klass->default_shield->port_cm + s.klass->default_shield->starboard_cm) * s.shield_mult;
    return (maxv > 0.0f) ? (cur / maxv) : 1.0f;
}

// Stable per-pilot seed (PGG personalityseed analog): hash of ship id.
uint32_t seed_for(uint32_t id) {
    uint32_t x = id * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15; x *= 0x85EBCA6Bu; x ^= x >> 13;
    return x ? x : 1u;
}
float seed01(uint32_t seed, float salt) {
    // deterministic-but-varied 0..1 from a seed + a float salt (e.g. t_now).
    const float v = std::sin((float)seed * 0.000113f + salt * 12.9898f) * 43758.5453f;
    return v - std::floor(v);
}

// ---- per-tick combat context ---------------------------------------------
struct Ctx {
    Ship*               self;
    const Ship*         target;
    const ShipRegistry* all;
    float               t_now;
    float               dist;       // world units self->target
    HMM_Vec3            to_target;  // world, self->target
    HMM_Vec3            fwd;        // self nose, world unit
    HMM_Vec3            lead_pos;   // intercept aim point, world
    HMM_Vec3            up;         // self up, world unit
    HMM_Vec3            right;      // self right, world unit
    float               gun_range;  // real reach of best equipped gun (range_m)
};

HMM_Vec3 body_axis(const Ship& s, HMM_Vec3 body) {
    const HMM_Mat4 R = HMM_QToM4(s.orientation);
    const HMM_Vec4 v = HMM_MulM4V4(R, HMM_V4(body.X, body.Y, body.Z, 0));
    return HMM_V3(v.X, v.Y, v.Z);
}

Ctx make_ctx(Ship& s, const Ship& t, const ShipRegistry& all, float t_now) {
    Ctx c{};
    c.self = &s; c.target = &t; c.all = &all; c.t_now = t_now;
    c.to_target = HMM_SubV3(t.position, s.position);
    c.dist      = std::sqrt(std::max(HMM_DotV3(c.to_target, c.to_target), 1e-6f));
    c.fwd       = body_axis(s, HMM_V3(0, 0, 1));
    c.up        = body_axis(s, HMM_V3(0, 1, 0));
    c.right     = body_axis(s, HMM_V3(1, 0, 0));

    // Lead / intercept (ported from the existing Engage math).
    float avg_proj_speed = 0.0f; int n = 0; float max_range = 0.0f;
    for (const auto& m : s.mounts) {
        if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
        const GunStats& gs = g_gun_stats[(int)m.type];
        if (!gs.complete) continue;
        avg_proj_speed += gs.speed_mps; ++n;
        max_range = std::max(max_range, gs.range_m);
    }
    avg_proj_speed = (n > 0) ? (avg_proj_speed / n) : 1100.0f;
    // Firing range = the ACTUAL reach of the best equipped gun (range_m,
    // ~3.7-5 km), NOT the weapons_range AI hint (was under-set at 3000 and
    // made the AI hold fire ~1.5-2 km inside its real reach). Fallback to the
    // class hint, then 4500.
    c.gun_range = (max_range > 0.0f) ? max_range
                : (s.klass && s.klass->weapons_range > 0.0f ? s.klass->weapons_range : 4500.0f);
    // Exact first-order interception using synchronized world velocities.
    // Projectiles inherit the shooter's velocity, so the shared solver uses
    // target-minus-shooter relative motion. This matters most when agile
    // fighters cross or circle each other at comparable speeds.
    c.lead_pos = aim::lead_point(s.position, s.world_velocity,
                                 t.position, t.world_velocity,
                                 avg_proj_speed);
    return c;
}

// ---- scalar token resolution ---------------------------------------------
float resolve_scalar(const AIScalar& sc, const Ctx& c) {
    const Ship& s = *c.self;
    const ShipClass* k = s.klass;
    switch (sc.tok) {
        case AIScalarToken::Literal:       return sc.lit;
        case AIScalarToken::F0:            return (k ? k->engage_f0 : 600.0f) * kPvtScale;
        case AIScalarToken::F0Radii:       return ((k ? k->engage_f0 : 600.0f) * kPvtScale)
                                                  + ship::hit_radius_m(s)
                                                  + ship::hit_radius_m(*c.target);
        case AIScalarToken::F1:            return (k ? k->comms_f1 : 1500.0f) * kPvtScale;
        case AIScalarToken::F2:            return k ? k->skill_f2 : 45.0f;
        case AIScalarToken::F3:            return k ? k->maneuver_jitter_f3 : 75.0f;
        case AIScalarToken::F6:            return k ? k->morale_f6 : 76.0f;
        case AIScalarToken::PursueSwitch:  return k_pursue_attack_switch;
        case AIScalarToken::GunRange:      return c.gun_range;
        case AIScalarToken::FleeThreshold: return ai_brain::flee_threshold_for(s);
        case AIScalarToken::Sensor:        return (k ? k->radar_range : k_default_radar_range_m) * kPvtScale;
    }
    return sc.lit;
}

// ---- condition metric evaluation -----------------------------------------
float eval_condition(const Ctx& c, AICondition cond) {
    const Ship& s = *c.self;
    switch (cond) {
        case AICondition::DistanceToTarget: return c.dist;
        case AICondition::HullFraction:     return ai_brain::hp_fraction(s);
        case AICondition::ShieldFraction:   return shield_fraction(s);
        case AICondition::TargetInFront: {
            const HMM_Vec3 u = HMM_DivV3F(c.to_target, c.dist);
            return HMM_DotV3(c.fwd, u);
        }
        case AICondition::TargetFacingMe: {
            const HMM_Vec3 tf = body_axis(*c.target, HMM_V3(0, 0, 1));
            const HMM_Vec3 u  = HMM_DivV3F(HMM_MulV3F(c.to_target, -1.0f), c.dist);
            return HMM_DotV3(tf, u);
        }
        case AICondition::TargetInRange:
            return (c.dist < c.gun_range) ? 1.0f : 0.0f;
        case AICondition::Random01: {
            // Quantize to the decision cadence so the roll is constant within a
            // ~1/k_decision_hz window (per-decision probability, not per-frame).
            const uint32_t bucket = (uint32_t)(c.t_now * k_decision_hz);
            return seed01(s.ai.personality_seed ^ (bucket * 2654435761u), (float)bucket);
        }
        case AICondition::ClosingRate: {
            const HMM_Vec3 rel = HMM_SubV3(s.world_velocity, c.target->world_velocity);
            const HMM_Vec3 u   = HMM_DivV3F(c.to_target, c.dist);
            return HMM_DotV3(rel, u);
        }
        case AICondition::TimeInState:
            return (s.ai.maneuver_started_at >= 0.0f) ? (c.t_now - s.ai.maneuver_started_at) : 0.0f;
        default: return 0.0f;
    }
}

bool clause_passes(const Ctx& c, const AICondClause& cl) {
    const float v = eval_condition(c, cl.cond);
    const float lo = resolve_scalar(cl.min, c);
    const float hi = resolve_scalar(cl.max, c);
    const bool inside = (v >= lo && v < hi);
    return cl.negate ? !inside : inside;
}
bool item_passes(const Ctx& c, const AILogicItem& it) {
    for (const AICondClause& cl : it.clauses)
        if (!clause_passes(c, cl)) return false;
    return true;   // empty clause list -> always passes (catch-all default)
}

// Logic channel: highest-priority passing item wins. Interrupt channel: same
// but only items strictly above the running priority are eligible.
const AILogicItem* select(const Ctx& c, const std::vector<AILogicItem>& items,
                          bool interrupt, float cur_priority) {
    const AILogicItem* best = nullptr;
    for (const AILogicItem& it : items) {
        if (interrupt && it.priority <= cur_priority) continue;
        if (!item_passes(c, it)) continue;
        if (!best || it.priority > best->priority) best = &it;
    }
    return best;
}

// ---- firing decision (firing upgrade) ------------------------------------
// Aggressivity/experience-scaled cone (f3) + skill-scaled reaction time (f2),
// on the ITTS lead vector we already compute. [I] tuning, docs s0/FAQ s5.1.
bool should_fire(Ctx& c) {
    Ship& s = *c.self;
    const HMM_Vec3 to_lead = HMM_SubV3(c.lead_pos, s.position);
    const float    d2      = HMM_DotV3(to_lead, to_lead);
    // Skill-scaled HOLD-FIRE range: pilots don't open up at full gun reach
    // (~8 km felt absurdly aggressive); they close to knife range first.
    // Novice (f2=40) opens at 3500 u, ace (f2=60) at 4500 u, clamped to the
    // actual gun reach. Approach/maneuver logic still uses the real
    // gun_range, so they close in normally and only START shooting here.
    const float f2  = s.klass ? s.klass->skill_f2 : 45.0f;
    const float fire_range = 3500.0f + std::clamp((f2 - 40.0f) / 20.0f, 0.0f, 1.0f) * 1000.0f;
    const float    wr      = std::min(c.gun_range, fire_range);
    if (d2 < 1e-6f || d2 >= wr * wr) { s.ai.fire_solution_at = -1.0f; return false; }

    const float f3  = s.klass ? s.klass->maneuver_jitter_f3 : k_evade_f3_default;
    const float agg = std::clamp((f3 - 30.0f) / 45.0f, 0.0f, 1.0f);     // 0..1
    constexpr float cos_tight = 0.985f;   // ~10 deg
    constexpr float cos_wide  = 0.951f;   // ~18 deg
    const float fcos = cos_tight + agg * (cos_wide - cos_tight);        // wider when aggressive

    const HMM_Vec3 lead_u = HMM_DivV3F(to_lead, std::sqrt(d2));
    if (HMM_DotV3(c.fwd, lead_u) < fcos) { s.ai.fire_solution_at = -1.0f; return false; }

    // Reaction-time gate: hold first shot until the solution has stood for the
    // per-pilot reaction time (skill f2: 40 -> 0.45 s, 60 -> 0.12 s).
    const float rt  = 0.45f - std::clamp((f2 - 40.0f) / 20.0f, 0.0f, 1.0f) * (0.45f - 0.12f);
    if (s.ai.fire_solution_at < 0.0f) s.ai.fire_solution_at = c.t_now;
    return (c.t_now - s.ai.fire_solution_at) >= rt;
}

// ---- steering primitive ---------------------------------------------------
// All maneuvers ultimately point the ship at a far world point and set the
// controller flags; the existing ChaseTarget flight controller turns the gap
// into angular velocity (no decel-on-arrival because the point is far).
void aim_at(Ship& s, HMM_Vec3 dir_world, float reach, bool afterburn,
            bool fire, float speed_scale) {
    const float l2 = HMM_DotV3(dir_world, dir_world);
    if (l2 > 1e-6f) dir_world = HMM_DivV3F(dir_world, std::sqrt(l2));
    s.behavior.kind          = ShipBehavior::ChaseTarget;
    s.behavior.target_pos    = HMM_AddV3(s.position, HMM_MulV3F(dir_world, reach));
    s.controller.afterburner = afterburn;
    s.controller.fire_guns   = fire;
    s.controller.speed_scale = speed_scale;
}

void maybe_bark(Ctx& c) {
    Ship& s = *c.self;
    const float f1 = (s.klass ? s.klass->comms_f1 : 1500.0f) * kPvtScale;
    if (c.dist < f1 && (c.t_now - s.ai.last_bark_at) > k_bark_cooldown_s) {
        comm::npc_engage_bark(s.faction, c.target->is_player, s.id);
        s.ai.last_bark_at = c.t_now;
    }
}

// Cache an evasive break direction at maneuver entry. `blend_away` mixes a
// 1-of-4 cardinal body axis (s9.4.3) with the away-from-target vector for a
// clean diagonal extend; otherwise it is a pure cardinal jink. Deterministic
// off the per-pilot seed so a given pilot's jink is stable-but-distinct.
void cache_break_dir(Ship& s, const Ctx& c, bool blend_away) {
    const uint32_t seed = s.ai.personality_seed;
    const int choice = (int)(seed % 4u);
    s.ai.jink_axis   = choice;
    HMM_Vec3 body;
    switch (choice) {
        case 0:  body = HMM_V3( 0,  1, 0); break;   // pitch up
        case 1:  body = HMM_V3( 0, -1, 0); break;   // pitch down
        case 2:  body = HMM_V3(-1,  0, 0); break;   // break left
        default: body = HMM_V3( 1,  0, 0); break;   // break right
    }
    s.ai.jink_roll = (float)((int)std::floor(seed01(seed, 3.1f) * 3.0f) - 1);  // -1/0/+1
    HMM_Vec3 lateral = body_axis(s, body);
    if (blend_away) {
        HMM_Vec3 away = HMM_MulV3F(HMM_DivV3F(c.to_target, c.dist), -1.0f);
        HMM_Vec3 mix  = HMM_AddV3(lateral, away);
        const float l2 = HMM_DotV3(mix, mix);
        lateral = (l2 > 1e-6f) ? HMM_DivV3F(mix, std::sqrt(l2)) : lateral;
    }
    s.ai.break_dir_world = lateral;
}

// ====================================================================== //
//  Maneuver handlers                                                     //
// ====================================================================== //
void run_maneuver(Ctx& c, AIManeuver m) {
    Ship& s = *c.self;
    const float t_now = c.t_now;
    switch (m) {

    case AIManeuver::LeadPursuit: {
        // Beyond the attack-run boundary: intercept close, and OPEN FIRE
        // whenever the target is lined up AND within real gun range
        // (should_fire) -- a pilot shoots from gun reach (~3.7-5 km) while
        // closing, not just inside the ~1000 u band. The 1000 u switch is a
        // MANEUVER boundary, not a fire gate.
        //
        // Throttle: drop to k_fire_throttle (~50%) WHILE actually shooting so
        // the guns stay on target; close at full speed when not firing.
        const bool fire = should_fire(c);
        aim_at(s, HMM_SubV3(c.lead_pos, s.position), k_aim_distance,
               /*ab*/false, /*fire*/fire, /*spd*/ fire ? k_fire_throttle : 1.0f);
        break;
    }

    case AIManeuver::AttackRun: {
        // Fire on GEOMETRY, promptly: should_fire gates on (lead in arc) +
        // (within gun range) + a short per-pilot reaction time -- NOT on the
        // decoded 76..153 tick window, whose seconds conversion is [?] (dt
        // unknown, s9.3) and was starving the guns. Throttle down a touch for
        // time-on-target; the close keeps tracking the intercept point.
        const bool fire = should_fire(c);
        aim_at(s, HMM_SubV3(c.lead_pos, s.position), k_aim_distance,
               /*ab*/false, fire, /*spd*/k_fire_throttle);
        maybe_bark(c);
        break;
    }

    case AIManeuver::BreakOff: {
        // Afterburn extension run. Blend the cached lateral jink (flavor)
        // with the LIVE away vector each frame so the ship keeps gaining
        // REAL separation even as the target maneuvers — weighted toward
        // away (0.7) so the range actually opens to the 3-5 km extension
        // distance before the state machine lets it turn back (run_combat
        // holds break-off until break_extend_dist is reached).
        HMM_Vec3 away = HMM_MulV3F(HMM_DivV3F(c.to_target, c.dist), -1.0f);
        HMM_Vec3 dir  = HMM_AddV3(HMM_MulV3F(away, 0.7f),
                                  HMM_MulV3F(s.ai.break_dir_world, 0.3f));
        const float l2 = HMM_DotV3(dir, dir);
        if (l2 > 1e-6f) dir = HMM_DivV3F(dir, std::sqrt(l2));
        aim_at(s, dir, k_aim_distance, /*ab*/true, false, 1.0f);
        break;
    }

    case AIManeuver::EvadeJink:
        // Pure cardinal jink (cached), afterburn, no fire.
        aim_at(s, s.ai.break_dir_world, k_aim_distance, /*ab*/true, false, 1.0f);
        break;

    case AIManeuver::BarrelRoll: {
        // Corkscrew toward the target: spiral the aim around the lead axis.
        const HMM_Vec3 f = HMM_DivV3F(HMM_SubV3(c.lead_pos, s.position),
                                      std::max(c.dist, 1.0f));
        const float ph = t_now * 4.0f + (float)s.ai.personality_seed * 0.013f;
        HMM_Vec3 spin = HMM_AddV3(HMM_MulV3F(c.right, std::cos(ph)),
                                  HMM_MulV3F(c.up,    std::sin(ph)));
        HMM_Vec3 dir  = HMM_AddV3(f, HMM_MulV3F(spin, 0.5f));
        aim_at(s, dir, k_aim_distance, /*ab*/true, should_fire(c), 1.0f);
        break;
    }

    case AIManeuver::LoopAround: {
        // Overshoot-and-curl: if the target is behind us, face + close;
        // otherwise curl out to an offset point beside it for a fresh pass.
        const HMM_Vec3 u = HMM_DivV3F(c.to_target, c.dist);
        if (HMM_DotV3(c.fwd, u) < 0.0f) {
            aim_at(s, HMM_SubV3(c.lead_pos, s.position), k_aim_distance, true, should_fire(c), 1.0f);
        } else {
            HMM_Vec3 side = (HMM_DotV3(c.right, c.right) > 1e-6f) ? c.right : c.up;
            const float swing = (seed01(s.ai.personality_seed, 1.7f) < 0.5f) ? 1.0f : -1.0f;
            HMM_Vec3 off = HMM_AddV3(c.target->position, HMM_MulV3F(side, swing * 1500.0f));
            aim_at(s, HMM_SubV3(off, s.position), k_aim_distance, true, false, 1.0f);
        }
        break;
    }

    case AIManeuver::EvadeLeftRight: {
        // Sign-flipping lateral weave (period from the seed).
        const float ph   = t_now * 1.6f + (float)s.ai.personality_seed * 0.011f;
        const float sign = (std::sin(ph) >= 0.0f) ? 1.0f : -1.0f;
        HMM_Vec3 dir = HMM_AddV3(c.fwd, HMM_MulV3F(c.right, sign * 0.8f));
        aim_at(s, dir, k_aim_distance, /*ab*/true, false, 1.0f);
        break;
    }

    case AIManeuver::EvadeUpDown: {
        const float ph   = t_now * 1.6f + (float)s.ai.personality_seed * 0.017f;
        const float sign = (std::sin(ph) >= 0.0f) ? 1.0f : -1.0f;
        HMM_Vec3 dir = HMM_AddV3(c.fwd, HMM_MulV3F(c.up, sign * 0.8f));
        aim_at(s, dir, k_aim_distance, /*ab*/true, false, 1.0f);
        break;
    }

    case AIManeuver::FacePerpendicular: {
        // Fly perpendicular to the target's facing -- a deflection-shot setup.
        const HMM_Vec3 tf = body_axis(*c.target, HMM_V3(0, 0, 1));
        HMM_Vec3 perp = HMM_Cross(tf, HMM_V3(0, 1, 0));
        if (HMM_DotV3(perp, perp) < 1e-6f) perp = HMM_Cross(tf, HMM_V3(1, 0, 0));
        // pick the sign that keeps the target roughly ahead
        if (HMM_DotV3(perp, c.to_target) < 0.0f) perp = HMM_MulV3F(perp, -1.0f);
        aim_at(s, perp, k_aim_distance, /*ab*/false, should_fire(c), 0.7f);
        break;
    }

    case AIManeuver::TurnAway:
        aim_at(s, HMM_MulV3F(HMM_DivV3F(c.to_target, c.dist), -1.0f),
               k_aim_distance, /*ab*/false, false, 1.0f);
        break;

    case AIManeuver::MatchSpeed: {
        // Hold aim on the target, match its speed (Kickstop-ish positioning).
        const float tgt_spd = c.target->sprite ? c.target->sprite->forward_speed : 0.0f;
        const float cruise  = s.klass ? s.klass->cruise_speed : 300.0f;
        const float scale   = (cruise > 1.0f) ? std::clamp(tgt_spd / cruise, 0.1f, 1.0f) : 1.0f;
        aim_at(s, HMM_SubV3(c.lead_pos, s.position), k_aim_distance,
               /*ab*/false, should_fire(c), scale);
        break;
    }

    case AIManeuver::FleeHome: {
        // Run directly away, blended toward patrol_anchor (home tether), with a
        // sinusoidal jink so the fleer isn't a clean tracking line. Ported from
        // the original Flee act.
        s.controller.fire_guns = false;
        HMM_Vec3 away = HMM_MulV3F(HMM_DivV3F(c.to_target, c.dist), -1.0f);
        if (s.ai.has_patrol_anchor) {
            const HMM_Vec3 to_home = HMM_SubV3(s.ai.patrol_anchor, s.position);
            const float dh2 = HMM_DotV3(to_home, to_home);
            if (dh2 > 1.0f) {
                const float dh = std::sqrt(dh2);
                const HMM_Vec3 home_dir = HMM_DivV3F(to_home, dh);
                const float blend = std::clamp(dh / 5000.0f, 0.0f, 0.7f);
                HMM_Vec3 mixed = HMM_AddV3(HMM_MulV3F(away, 1.0f - blend),
                                           HMM_MulV3F(home_dir, blend));
                const float m2 = HMM_DotV3(mixed, mixed);
                if (m2 > 1e-6f) away = HMM_DivV3F(mixed, std::sqrt(m2));
            }
        }
        HMM_Vec3 perp = HMM_Cross(away, HMM_V3(0, 1, 0));
        if (HMM_DotV3(perp, perp) < 1e-6f) perp = HMM_Cross(away, HMM_V3(1, 0, 0));
        perp = HMM_DivV3F(perp, std::sqrt(std::max(HMM_DotV3(perp, perp), 1e-12f)));
        const float period = 2.5f + (float)(s.id % 4) * 0.4f;
        const float jink   = std::sin(t_now * (2.0f * kPi / period) + (float)s.id * 1.3f) * 0.45f;
        HMM_Vec3 evade = HMM_AddV3(away, HMM_MulV3F(perp, jink));
        s.behavior.kind          = ShipBehavior::PursueTarget;   // kinematic far-point
        s.controller.afterburner = false;
        s.controller.speed_scale = 1.0f;
        const float el2 = HMM_DotV3(evade, evade);
        if (el2 > 1e-6f) evade = HMM_DivV3F(evade, std::sqrt(el2));
        s.behavior.target_pos = HMM_AddV3(s.position, HMM_MulV3F(evade, k_flee_distance));
        break;
    }

    default: break;
    }
}

// Per-maneuver entry actions (cache jink/break geometry once at selection).
void on_enter(Ship& s, const Ctx& c, AIManeuver m) {
    if (m == AIManeuver::BreakOff) {
        cache_break_dir(s, c, /*blend_away*/true);
        // Pick a fresh 3-5 km extension target so each break-off run opens
        // real range before turning back. Per-pilot seed keeps it varied
        // but deterministic; bucketed by entry time so repeated breaks
        // don't all pick the same number.
        const float r = seed01(s.ai.personality_seed,
                               c.t_now * 0.37f + (float)s.ai.jink_axis);
        s.ai.break_extend_dist = 3000.0f + r * 2000.0f;   // 3000..5000 m
    }
    if (m == AIManeuver::EvadeJink) cache_break_dir(s, c, /*blend_away*/false);
    if (m == AIManeuver::AttackRun) s.ai.fire_solution_at = -1.0f;
}

void start_maneuver(Ship& s, const Ctx& c, const AILogicItem& it, float t_now) {
    s.ai.cur_maneuver        = it.maneuver;
    s.ai.cur_priority        = it.priority;
    s.ai.cur_duration        = it.duration_s;
    s.ai.maneuver_started_at = t_now;
    on_enter(s, c, it.maneuver);
}

// Which maneuvers count as "active dogfight evasion" for the jink-stamina
// budget. FleeHome is deliberately excluded - that's a hull-critical run for
// the door, not a tireable dodge, and we don't want a wounded ship's escape
// gated by stamina. Pure positioning (LeadPursuit / AttackRun / MatchSpeed /
// FacePerpendicular) is not evasion.
bool is_evasive(AIManeuver m) {
    switch (m) {
        case AIManeuver::BreakOff:
        case AIManeuver::EvadeJink:
        case AIManeuver::BarrelRoll:
        case AIManeuver::LoopAround:
        case AIManeuver::EvadeLeftRight:
        case AIManeuver::EvadeUpDown:
        case AIManeuver::TurnAway:
            return true;
        default:
            return false;
    }
}

int pick_tier(const Ship& s) {
    const float f6 = s.klass ? s.klass->morale_f6 : 76.0f;
    if (f6 <= 70.0f)  return 2;   // fanatical
    if (f6 >= 110.0f) return 0;   // timid
    return 1;                     // steady
}

} // namespace

// ====================================================================== //
//  Orchestration                                                         //
// ====================================================================== //
void ai_brain::run_combat(Ship& s, const ShipRegistry& all, float t_now) {
    // Lazy table resolve + per-pilot seed stamp.
    if (!s.ai.brain) {
        // Per-class AI-table override wins (e.g. heavy gunships use "heavy");
        // otherwise fall back to the per-faction table, then default.
        if (s.klass && !s.klass->ai_table.empty()) s.ai.brain = find(s.klass->ai_table);
        if (!s.ai.brain)                           s.ai.brain = resolve_for_faction(s.faction);
    }
    if (s.ai.personality_seed == 0) s.ai.personality_seed = seed_for(s.id);
    const AILogicTable* tbl = s.ai.brain ? s.ai.brain : default_table();

    const Ship* target = all.find_by_id(s.ai.target_id);
    if (!tbl || !target || !target->alive) {
        s.ai.target_id           = 0;
        s.behavior.kind          = ShipBehavior::None;
        s.controller.fire_guns   = false;
        s.controller.afterburner = false;
        s.ai.cur_priority        = 0.0f;
        s.ai.maneuver_started_at = -1.0f;
        return;
    }

    Ctx c = make_ctx(s, *target, all, t_now);
    s.ai.morale_tier = pick_tier(s);
    const AIMoraleTier& tier = tbl->tiers[std::clamp(s.ai.morale_tier, 0, 2)];

    // --- Jink stamina ---------------------------------------------------
    // Tire the pilot out of evasion on a skill-scaled budget so the attacker
    // gets predictable windows to land shots. Accrues against the maneuver
    // that ran since last tick; when the budget is spent we open a recovery
    // cooldown during which evasive maneuvers are suppressed (see below).
    {
        const float dt = (s.ai.last_combat_t >= 0.0f)
                       ? std::clamp(t_now - s.ai.last_combat_t, 0.0f, 0.5f) : 0.0f;
        s.ai.last_combat_t = t_now;

        // Knife-range override: inside the break-off radius (f0 + hull radii)
        // a pilot ALWAYS has the juice to peel off. Hold stamina full so a
        // tired NPC breaks instead of jousting straight through the target
        // and ramming it (the player especially). Stamina only starts
        // draining again once it has opened back outside f0.
        const float f0_radius = (s.klass ? s.klass->engage_f0 : 600.0f) * kPvtScale
                              + ship::hit_radius_m(s)
                              + ship::hit_radius_m(*target);
        if (c.dist <= f0_radius) {
            s.ai.jink_cooldown_until = -1.0f;
            s.ai.jink_spent_s        = 0.0f;
        }

        // Recovery finished -> refresh stamina with a fresh rolled budget.
        if (s.ai.jink_cooldown_until >= 0.0f && t_now >= s.ai.jink_cooldown_until) {
            s.ai.jink_cooldown_until = -1.0f;
            s.ai.jink_spent_s        = 0.0f;
            s.ai.jink_budget_s       = -1.0f;
        }
        if (s.ai.jink_budget_s < 0.0f) {
            // budget = f2/8 +/- up to 3 s. f2=45 -> ~5.6 s (2.6..8.6).
            const float f2     = s.klass ? s.klass->skill_f2 : 45.0f;
            const float jitter = (seed01(s.ai.personality_seed, t_now) * 2.0f - 1.0f) * 3.0f;
            s.ai.jink_budget_s = std::max(1.0f, f2 / 8.0f + jitter);
        }
        // Burn stamina while actually evading (and not already recovering).
        if (s.ai.jink_cooldown_until < 0.0f && is_evasive(s.ai.cur_maneuver)) {
            s.ai.jink_spent_s += dt;
            if (s.ai.jink_spent_s >= s.ai.jink_budget_s) {
                // Tapped out: open a 5 +/- up to 3 s recovery (>=1 s floor).
                const float cd = std::max(1.0f,
                    5.0f + (seed01(s.ai.personality_seed, t_now * 1.7f) * 2.0f - 1.0f) * 3.0f);
                s.ai.jink_cooldown_until = t_now + cd;
            }
        }
    }
    const bool suppress_evasion = (s.ai.jink_cooldown_until >= 0.0f);

    // --- Sticky break-off extension ------------------------------------
    // Once a ship peels off (break-off selected within ~680 m), HOLD the
    // extension until it has actually opened break_extend_dist (3-5 km) of
    // range — instead of letting the ~1 km pursuit boundary yank it back
    // for another pass the instant it crosses. This makes attack/disengage
    // loops fly a proper extension out to 3-5 km, then return. A safety
    // timeout prevents a faster pursuer from trapping it forever, and a
    // hull-critical flee can still preempt the run.
    if (s.ai.cur_maneuver == AIManeuver::BreakOff && s.ai.break_extend_dist > 0.0f) {
        // A fleeing target (negative closing rate) doesn't deserve a 3-5 km
        // head start — commit and turn back to re-engage instead of flying
        // further from a runner who can outrun the chase anyway (#163).
        const bool target_fleeing =
            (c.dist > 1e-3f) &&
            (HMM_DotV3(HMM_SubV3(s.world_velocity, c.target->world_velocity),
                       HMM_DivV3F(c.to_target, c.dist)) < 0.0f);
        const float elapsed = (s.ai.maneuver_started_at >= 0.0f)
                            ? (t_now - s.ai.maneuver_started_at) : 999.0f;
        const bool reached = c.dist >= s.ai.break_extend_dist;
        const bool timeout = elapsed >= 12.0f;
        const AILogicItem* preempt =
            select(c, tier.interrupt, /*interrupt*/true, s.ai.cur_priority);
        const bool flee_now = preempt && preempt->maneuver == AIManeuver::FleeHome;
        // Out of stamina? Abandon the extension and fall through to a normal
        // (gated) reselection so the tired pilot turns back in and flies
        // predictably instead of afterburning away. Same when the target is
        // fleeing — cut the extension so we don't run away from a runner.
        if (!reached && !timeout && !flee_now && !suppress_evasion && !target_fleeing) {
            run_maneuver(c, AIManeuver::BreakOff);
            const AIState mapped = ship_ai::from_name(
                ai_maneuver::maneuver_state_label(AIManeuver::BreakOff));
            if (mapped != s.ai.state) { s.ai.state = mapped; s.ai.state_entered_at = t_now; }
            return;
        }
        s.ai.break_extend_dist = 0.0f;   // extension complete (reached/timeout/flee)
    }

    const bool expired = (s.ai.maneuver_started_at < 0.0f)
                      || ((t_now - s.ai.maneuver_started_at) >= s.ai.cur_duration);

    // (a) interrupt channel: a strictly-higher-priority rule preempts.
    if (const AILogicItem* hit = select(c, tier.interrupt, /*interrupt*/true, s.ai.cur_priority)) {
        if (hit->maneuver != s.ai.cur_maneuver || expired)
            start_maneuver(s, c, *hit, t_now);
    }
    // (b) else keep running the current maneuver until its duration expires.
    else if (!expired) {
        // hold
    }
    // (c) else logic channel: highest-priority passing rule (re)selects.
    else if (const AILogicItem* lg = select(c, tier.logic, /*interrupt*/false, 0.0f)) {
        start_maneuver(s, c, *lg, t_now);
    }
    else {
        // Nothing matched -> default to a close. priority 0 so any interrupt wins.
        s.ai.cur_maneuver = AIManeuver::LeadPursuit;
        s.ai.cur_priority = 0.0f; s.ai.cur_duration = 2.0f; s.ai.maneuver_started_at = t_now;
    }

    // Stamina gate: a tapped-out pilot can't evade — force a close-in lead
    // pursuit (flies a predictable line) until the recovery cooldown ends.
    if (suppress_evasion && is_evasive(s.ai.cur_maneuver)) {
        s.ai.cur_maneuver = AIManeuver::LeadPursuit;
    }

    run_maneuver(c, s.ai.cur_maneuver);

    // HUD/debug: collapse the running maneuver onto the legacy AIState enum.
    const AIState mapped = ship_ai::from_name(ai_maneuver::maneuver_state_label(s.ai.cur_maneuver));
    if (mapped != s.ai.state) { s.ai.state = mapped; s.ai.state_entered_at = t_now; }
}

void ai_brain::run_forced(Ship& s, const ShipRegistry& all, float t_now, AIManeuver m) {
    if (s.ai.personality_seed == 0) s.ai.personality_seed = seed_for(s.id);
    const Ship* target = all.find_by_id(s.ai.target_id);
    if (!target || !target->alive) {
        s.ai.target_id           = 0;
        s.behavior.kind          = ShipBehavior::None;
        s.controller.fire_guns   = false;
        s.controller.afterburner = false;
        s.ai.cur_priority        = 0.0f;
        s.ai.maneuver_started_at = -1.0f;
        return;
    }
    Ctx c = make_ctx(s, *target, all, t_now);
    if (s.ai.cur_maneuver != m) {
        s.ai.cur_maneuver = m; s.ai.cur_priority = 99.0f;
        s.ai.cur_duration = 1.0f; s.ai.maneuver_started_at = t_now;
        on_enter(s, c, m);
    }
    run_maneuver(c, m);
    const AIState mapped = ship_ai::from_name(ai_maneuver::maneuver_state_label(m));
    if (mapped != s.ai.state) { s.ai.state = mapped; s.ai.state_entered_at = t_now; }
}

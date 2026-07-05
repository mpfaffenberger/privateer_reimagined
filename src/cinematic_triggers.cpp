// -----------------------------------------------------------------------------
// cinematic_triggers.cpp — data-driven cutscene triggers (see header).
//
// Parsing follows cinematic_parse.cpp's hardening contract to the letter:
// every optional field goes through a defaulted find()-based accessor
// (json::Value::operator[] THROWS on a missing key — that bug bit us in
// Phase 6.2), and parse_triggers is additionally try-wrapped so ANY residual
// throw degrades to "log + zero triggers", never a crash.
//
// Runtime state (latches / cooldown stamps / the play hook) is file-static,
// mirroring scripted_encounters.cpp: triggers never change at runtime except
// via load(), so tick() is a flat evaluation loop with no gameplay surface.
// -----------------------------------------------------------------------------

#include "cinematic_triggers.h"

#include "plot.h"

#include <cstdio>

namespace cinematic::triggers {

namespace {

// ---- safe, non-throwing field accessors (cinematic_parse.cpp style) --------
std::string str(const json::Value& v, const char* key, const std::string& def) {
    const json::Value* f = v.find(key);
    return f ? f->string_or(def) : def;
}
float num(const json::Value& v, const char* key, float def) {
    const json::Value* f = v.find(key);
    return f ? (float)f->number_or(def) : def;
}
bool flag(const json::Value& v, const char* key, bool def) {
    const json::Value* f = v.find(key);
    return f ? f->bool_or(def) : def;
}
void read_flag_list(const json::Value& v, const char* key,
                    std::vector<std::string>& out) {
    if (const json::Value* a = v.find(key); a && a->is_array())
        for (const json::Value& s : a->as_array())
            if (s.is_string()) out.push_back(s.as_string());
}

// ---- module state -----------------------------------------------------------
struct TriggerState {
    bool  fired       = false;      // once-latch
    float last_fire_s = -1.0e9f;    // cooldown stamp
};

std::vector<Trigger>      g_triggers;
std::vector<TriggerState> g_state;
std::function<bool(const std::string&)> g_play;

// Evaluate one trigger's `when` block against the world. Pure; latch and
// cooldown gating happen in tick().
bool conditions_hold(const Trigger& t, const TriggerCtx& ctx) {
    if (!t.system.empty()     && t.system     != ctx.system_id)  return false;
    if (!t.ship_class.empty() && t.ship_class != ctx.ship_class) return false;
    if (t.missiles_max >= 0 && ctx.missiles_total > t.missiles_max) return false;
    if (t.missiles_min >= 0 && ctx.missiles_total < t.missiles_min) return false;
    for (const Trigger::CargoReq& c : t.cargo) {
        if (!ctx.cargo_units) return false;             // no lookup = can't hold
        if (ctx.cargo_units(c.commodity) < c.min_units) return false;
    }
    if (t.has_near_nav) {
        HMM_Vec3 pos;
        if (!ctx.nav_pos || !ctx.nav_pos(t.nav, pos)) return false;
        const HMM_Vec3 d = HMM_SubV3(pos, ctx.player_pos);
        if (HMM_DotV3(d, d) > t.radius_m * t.radius_m) return false;
    }
    if (!t.requires_flags.empty() || !t.forbids_flags.empty()) {
        if (!ctx.player) return false;                  // flag-gated needs a player
        for (const std::string& fl : t.requires_flags)
            if (!plot::has_flag(*ctx.player, fl)) return false;
        for (const std::string& fl : t.forbids_flags)
            if (plot::has_flag(*ctx.player, fl)) return false;
    }
    return true;
}

} // anonymous namespace

bool parse_triggers(const json::Value& root, std::vector<Trigger>& out,
                    std::string& err) {
    out.clear();
    if (!root.is_object()) {
        err = "triggers root is not a JSON object";
        return false;
    }
    try {
        const json::Value* list = root.find("triggers");
        if (!list || !list->is_array()) return true;    // valid, zero triggers
        for (const json::Value& tv : list->as_array()) {
            if (!tv.is_object()) continue;
            Trigger t;
            t.cinematic = str(tv, "cinematic", "");
            if (t.cinematic.empty()) {
                std::printf("[cine-trigger] entry without 'cinematic' id — "
                            "skipped\n");
                continue;
            }
            t.once       = flag(tv, "once", true);
            t.cooldown_s = num(tv, "cooldown_s", 0.0f);
            if (const json::Value* w = tv.find("when"); w && w->is_object()) {
                t.system     = str(*w, "system", "");
                t.ship_class = str(*w, "ship_class", "");
                t.missiles_max = (int)num(*w, "missiles_max", -1.0f);
                t.missiles_min = (int)num(*w, "missiles_min", -1.0f);
                if (const json::Value* cl = w->find("cargo"); cl && cl->is_array())
                    for (const json::Value& cv : cl->as_array()) {
                        if (!cv.is_object()) continue;
                        Trigger::CargoReq req;
                        req.commodity = str(cv, "commodity", "");
                        req.min_units = (int)num(cv, "min_units", 1.0f);
                        if (!req.commodity.empty())
                            t.cargo.push_back(std::move(req));
                    }
                if (const json::Value* nn = w->find("near_nav");
                    nn && nn->is_object()) {
                    t.nav      = str(*nn, "nav", "");
                    t.radius_m = num(*nn, "radius_m", 0.0f);
                    t.has_near_nav = !t.nav.empty() && t.radius_m > 0.0f;
                }
                read_flag_list(*w, "requires_flags", t.requires_flags);
                read_flag_list(*w, "forbids_flags",  t.forbids_flags);
            }
            out.push_back(std::move(t));
        }
    } catch (const std::exception& e) {
        err = std::string("trigger parse threw: ") + e.what();
        out.clear();
        return false;
    } catch (...) {
        err = "trigger parse threw (unknown)";
        out.clear();
        return false;
    }
    return true;
}

void load(const std::string& path) {
    g_triggers.clear();
    g_state.clear();
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
    } else {
        std::printf("[cine-trigger] no trigger file at %s — 0 triggers\n",
                    path.c_str());
        return;   // NON-fatal: the evaluator is a silent no-op
    }
    const json::Value root = json::parse_file(path);
    std::string err;
    if (!parse_triggers(root, g_triggers, err)) {
        std::printf("[cine-trigger] load failed (%s): %s — 0 triggers\n",
                    err.c_str(), path.c_str());
        g_triggers.clear();
    }
    g_state.assign(g_triggers.size(), TriggerState{});
    std::printf("[cine-trigger] loaded %zu trigger(s) from %s\n",
                g_triggers.size(), path.c_str());
}

void reset() {
    g_state.assign(g_triggers.size(), TriggerState{});
}

void set_play_hook(std::function<bool(const std::string& id)> fn) {
    g_play = std::move(fn);
}

void tick(const TriggerCtx& ctx, float now_s) {
    if (g_triggers.empty() || !g_play) return;
    for (size_t i = 0; i < g_triggers.size(); ++i) {
        const Trigger& t = g_triggers[i];
        TriggerState&  s = g_state[i];
        if (t.once && s.fired) continue;
        if (!t.once && (now_s - s.last_fire_s) < t.cooldown_s) continue;
        if (!conditions_hold(t, ctx)) continue;
        // A refused fire (not in Flight / cinematic already active / bad id)
        // does NOT latch — we retry next frame until the world is free.
        if (!g_play(t.cinematic)) continue;
        s.fired       = true;
        s.last_fire_s = now_s;
        std::printf("[cine-trigger] fired '%s' (once=%d cooldown=%.1fs)\n",
                    t.cinematic.c_str(), (int)t.once, t.cooldown_s);
        return;   // at most one fire per tick
    }
}

int count() { return (int)g_triggers.size(); }

} // namespace cinematic::triggers

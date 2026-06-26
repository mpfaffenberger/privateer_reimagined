// -----------------------------------------------------------------------------
// scripted_encounters.cpp — data-driven scenario director (Phase 2).
// See scripted_encounters.h for the design overview.
// -----------------------------------------------------------------------------

#include "scripted_encounters.h"

#include "comm.h"            // comm::push
#include "faction.h"         // faction::from_name / to_name
#include "json.h"            // json::parse_file + Value
#include "ship_class.h"      // ship_class::find (registry for class->name)

#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace scripted {

namespace {

// ---- tuning ---------------------------------------------------------------
// Per the Phase 2 spec. Kept file-static: nothing outside cares.
constexpr float k_trigger_range_m = 8000.0f;
constexpr float k_turn_gap_s      = 3.5f;

// ---- per-scenario runtime state ------------------------------------------
// One entry per loaded scenario. `triggered` short-circuits the trigger
// loop on once-per-system scenarios; `last_fire_s` paces the
// cooldown_s gate for non-locked scenarios. Both reset() clears.
struct ScenarioState {
    bool  triggered   = false;
    float last_fire_s = -1e9f;   // far-past so the first tick always passes
};

// ---- parsed scenario -----------------------------------------------------
struct Turn {
    std::string voice;   // raw "voice" id (faction name or voice_id)
    std::string line;    // author-written line text
};

struct Scenario {
    std::string id;

    // trigger
    std::vector<std::string> near_classes;     // empty = any class
    std::string              near_faction;     // faction::from_name key
    int     max_count      = 99;
    float   chance         = 0.0f;
    float   cooldown_s     = 0.0f;
    bool    once_per_system  = false;
    bool    ignore_player_rep = false;

    // dialogue
    std::vector<Turn> dialogue;

    // opaque later-wave payloads. We log presence but don't act on them
    // in Wave A — just record that an entry exists, so a follow-up wave
    // can switch on the boolean without re-parsing JSON.
    bool has_spawn_on_accept = false;
    bool has_reward          = false;
};

// ---- module state ---------------------------------------------------------
std::vector<Scenario>       g_scenarios;
std::vector<ScenarioState>  g_state;

// Active playback (one scenario at a time). All four fields are only
// meaningful while `active_idx >= 0`.
int      active_idx       = -1;
int      active_turn      = 0;    // next turn to emit
float    active_next_at   = 0.0f; // wall-clock seconds
uint32_t active_anchor_id = 0;    // nearest matching ship's id (informational)

// Deterministic RNG (hailing.cpp pattern). Cosmetically seeded so reruns
// stay reproducible for any "did the scenario fire this time" assertions
// in debug builds. A scene-level re-roll would require reseeding here.
std::mt19937& rng() {
    static std::mt19937 r{0xDEC1DEFu ^ 0xCAFEu};
    return r;
}

// ---- helpers --------------------------------------------------------------

// Pretty display name for the feed: "Confederation" instead of faction's
// raw lowercase catalogue id. Falls back to faction::to_name for any
// other faction (hunter -> "hunter", etc.) — same policy as hailing.cpp.
const char* display_name(Faction f) {
    switch (f) {
        case Faction::Militia: return "Militia";
        case Faction::Confed:  return "Confederation";
        default:               return faction::to_name(f);
    }
}

// Map a turn's `voice` string to a feed display name. We follow the
// spec: faction::from_name -> display_name; else capitalized raw string;
// else the raw string verbatim. Unknown voices (e.g. "confed_f" alias)
// fall through to capitalized() so they read naturally.
std::string capitalize_first(const std::string& s) {
    if (s.empty()) return s;
    std::string out = s;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

std::string speaker_display(const std::string& voice) {
    const Faction f = faction::from_name(voice);
    if (f != Faction::Count) return display_name(f);
    if (voice.empty())      return std::string{};
    return capitalize_first(voice);
}

// Format the speaker display + line as the HUD feed wants it. We build
// the string rather than snprintf so the speaker + body are always
// safely concatenated even with very long lines (the demo's hailing
// feed uses snprintf + a fixed buffer; that policy caps at 256 chars
// which is fine here because authored lines are short).
std::string format_feed(const std::string& speaker, const std::string& line) {
    std::string out;
    out.reserve(speaker.size() + 2 + line.size());
    out.append(speaker);
    out.append(": ");
    out.append(line);
    return out;
}

// ---- parsing --------------------------------------------------------------

// Pull a `near_classes` string list out of the JSON. Missing/non-array
// just leaves the scenario with an empty list (= any class).
void parse_near_classes(const json::Value& root, Scenario& s) {
    const json::Value* nc = root.find("near_classes");
    if (!nc || !nc->is_array()) return;
    for (const json::Value& c : nc->as_array()) {
        if (c.is_string()) s.near_classes.push_back(c.as_string());
    }
}

// Parse one scenario object. Best-effort: missing/wrong-typed fields
// fall through to defaults rather than aborting the whole load.
void parse_scenario(const json::Value& v, Scenario& s) {
    s.id = v.find("id") ? v["id"].as_string() : std::string{};

    if (const json::Value* tr = v.find("trigger"); tr && tr->is_object()) {
        parse_near_classes(*tr, s);
        if (const json::Value* nf = tr->find("near_faction"); nf && nf->is_string())
            s.near_faction = nf->as_string();
        if (const json::Value* mc = tr->find("max_count"); mc)
            s.max_count = mc->as_int();
        if (const json::Value* ch = tr->find("chance"); ch)
            s.chance = static_cast<float>(ch->as_number());
        if (const json::Value* cd = tr->find("cooldown_s"); cd)
            s.cooldown_s = static_cast<float>(cd->as_number());
        if (const json::Value* op = tr->find("once_per_system"); op)
            s.once_per_system = op->as_bool();
        if (const json::Value* ipr = tr->find("ignore_player_rep"); ipr)
            s.ignore_player_rep = ipr->as_bool();
    }

    if (const json::Value* d = v.find("dialogue"); d && d->is_array()) {
        for (const json::Value& tv : d->as_array()) {
            if (!tv.is_object()) continue;
            std::string voice, line;
            if (const json::Value* vv = tv.find("voice"); vv && vv->is_string())
                voice = vv->as_string();
            if (const json::Value* ll = tv.find("line"); ll && ll->is_string())
                line = ll->as_string();
            if (line.empty()) continue;  // empty line is no-op; skip it
            s.dialogue.push_back(Turn{voice, line});
        }
    }

    // Wave A: just record presence of the later-wave payloads. A
    // follow-up wave can switch on these flags without re-parsing JSON.
    if (const json::Value* sa = v.find("spawn_on_accept"); sa && !sa->is_null())
        s.has_spawn_on_accept = true;
    if (const json::Value* rw = v.find("reward"); rw && !rw->is_null())
        s.has_reward = true;
}

} // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void load(const std::string& path) {
    // Reset first so a reload doesn't accumulate duplicates.
    g_scenarios.clear();
    g_state.clear();
    active_idx       = -1;
    active_turn      = 0;
    active_next_at   = 0.0f;
    active_anchor_id = 0;

    json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr,
                     "[scripted] could not parse '%s' — scenarios disabled\n",
                     path.c_str());
        return;
    }
    const json::Value* arr = root.find("scenarios");
    if (!arr || !arr->is_array()) {
        std::fprintf(stderr, "[scripted] '%s': missing 'scenarios' array\n",
                     path.c_str());
        return;
    }

    for (const json::Value& sv : arr->as_array()) {
        if (!sv.is_object()) continue;
        Scenario s;
        parse_scenario(sv, s);
        if (s.id.empty()) {
            std::fprintf(stderr,
                         "[scripted] '%s': scenario missing 'id', skipped\n",
                         path.c_str());
            continue;
        }
        g_scenarios.push_back(std::move(s));
        g_state.push_back(ScenarioState{});
    }

    std::printf("[scripted] loaded %zu scenarios from '%s'\n",
                g_scenarios.size(), path.c_str());
}

void reset() {
    for (auto& s : g_state) {
        s.triggered   = false;
        s.last_fire_s = -1e9f;
    }
    active_idx       = -1;
    active_turn      = 0;
    active_next_at   = 0.0f;
    active_anchor_id = 0;
}

void tick(const ShipRegistry& ships, const Ship& player_ship, float now_s) {
    // ---- active playback -----------------------------------------------
    // Only ONE scenario plays at a time. While one is mid-dialogue we
    // emit its turns on schedule and short-circuit out — the trigger
    // loop runs again on the tick after playback completes.
    if (active_idx >= 0) {
        if (now_s < active_next_at) return;
        if (active_idx >= static_cast<int>(g_scenarios.size())) {
            // Stale index (e.g. scenario list shrank on reload) — just
            // drop the playback and resume on the next tick.
            active_idx       = -1;
            active_anchor_id = 0;
            return;
        }
        const Scenario& sc = g_scenarios[active_idx];

        // Emit the current turn (comm feed + debug log).
        const Turn& turn = sc.dialogue[active_turn];
        const std::string disp = speaker_display(turn.voice);
        comm::push(format_feed(disp, turn.line), /*taunt=*/true);
        std::printf("[scenario] %s turn %d: %s\n",
                    sc.id.c_str(), active_turn, turn.line.c_str());

        ++active_turn;

        if (active_turn >= static_cast<int>(sc.dialogue.size())) {
            // No more turns after this one — flush the completion
            // marker now. Set triggered + last_fire_s so once_per
            // scenarios retire cleanly and re-fireables stamp their
            // cooldown.
            std::printf("[scenario] %s complete\n", sc.id.c_str());
            g_state[active_idx].triggered   = true;
            g_state[active_idx].last_fire_s = now_s;
            active_idx       = -1;
            active_anchor_id = 0;
        } else {
            active_next_at = now_s + k_turn_gap_s;
        }
        return;
    }

    // ---- trigger evaluation --------------------------------------------
    // One scenario considered per tick (per spec). The order is the
    // load order of the JSON file — authors control priority by
    // ordering entries. Earliest-first keeps the director single-pass.
    for (size_t i = 0; i < g_scenarios.size(); ++i) {
        const Scenario& sc = g_scenarios[i];

        // ---- eligibility gates --------------------------------------
        // once_per_system scenarios are permanently retired after their
        // first fire (success OR miss) — they're a one-shot.
        if (sc.once_per_system && g_state[i].triggered) continue;
        // Cooldown gate for non-once_per_system scenarios. We use
        // last_fire_s == -1e9 sentinel so the very first tick always
        // passes the gate regardless of cooldown_s value.
        if (!sc.once_per_system) {
            if ((now_s - g_state[i].last_fire_s) < sc.cooldown_s) continue;
        }

        // ---- candidate scan ------------------------------------------
        // Resolved trigger faction. Unknown names skip silently rather
        // than spam warnings every tick (an authoring error should be
        // fixed in the JSON, not nagged in the log).
        const Faction target_fac = faction::from_name(sc.near_faction);
        if (target_fac == Faction::Count) continue;

        const float range_sq = k_trigger_range_m * k_trigger_range_m;

        int      match_count     = 0;
        uint32_t nearest_id      = 0;
        float    nearest_dist_sq = 1e30f;   // "infinity"

        for (const Ship& s : ships) {
            if (!s.alive)                continue;
            if (s.is_player)             continue;
            if (s.faction != target_fac) continue;

            const HMM_Vec3 d = HMM_SubV3(s.position, player_ship.position);
            const float dist_sq = HMM_DotV3(d, d);
            if (dist_sq > range_sq) continue;

            if (!sc.near_classes.empty()) {
                if (!s.klass) continue;
                bool matched = false;
                for (const std::string& want : sc.near_classes) {
                    if (s.klass->name == want) { matched = true; break; }
                }
                if (!matched) continue;
            }

            ++match_count;
            if (dist_sq < nearest_dist_sq) {
                nearest_dist_sq = dist_sq;
                nearest_id      = s.id;
            }
        }

        if (match_count == 0) continue;

        // ---- density gate --------------------------------------------
        // Spec: "if at least one match AND count <= max_count". Too
        // many matches = scenario doesn't fit; mark tried + break so
        // we don't loop trying other scenarios this tick.
        if (match_count > sc.max_count) {
            if (sc.once_per_system) g_state[i].triggered = true;
            else                    g_state[i].last_fire_s = now_s;
            break;
        }

        // ---- chance roll ---------------------------------------------
        // Uniform 0..1 draw; anything strictly below `chance` is a HIT.
        // A chance of 0.0 is therefore always a miss (use 1.0 for
        // guaranteed fire); a chance of 1.0 is always a hit. Mirrors
        // hailing.cpp's `r >= chance` guard.
        std::uniform_real_distribution<float> pick(0.0f, 1.0f);
        const float r = pick(rng());
        if (r >= sc.chance) {
            // MISS — retire the scenario for this system (once_per)
            // or stamp the cooldown.
            if (sc.once_per_system) g_state[i].triggered = true;
            else                    g_state[i].last_fire_s = now_s;
            break;
        }

        // HIT — start playback. Stamping active_next_at to now means
        // the very next call emits the first turn (and the guard
        // `now_s >= active_next_at` passes immediately).
        active_idx       = static_cast<int>(i);
        active_turn      = 0;
        active_next_at   = now_s;
        active_anchor_id = nearest_id;
        break;
    }
}

} // namespace scripted

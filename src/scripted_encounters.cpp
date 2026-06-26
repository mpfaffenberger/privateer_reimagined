// -----------------------------------------------------------------------------
// scripted_encounters.cpp — data-driven scenario director (Phase 2).
// See scripted_encounters.h for the design overview.
// -----------------------------------------------------------------------------

#include "scripted_encounters.h"

#include "comm.h"            // comm::push
#include "audio.h"           // audio::load / play, SampleId
#include "faction.h"         // faction::from_name / to_name
#include "json.h"            // json::parse_file + Value
#include "player.h"          // PlayerState, PlayerReputation, player::add_credits
#include "ship_class.h"      // ship_class::find (registry for class->name)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scripted { namespace {

// ---- tuning ---------------------------------------------------------------
constexpr float k_trigger_range_m = 8000.0f;
constexpr float k_turn_gap_s      = 3.5f;

// Wave B spawn placement. Tighter 3-5 km shell than the encounter
// director's 8-15 km — the dialogue already telegraphs the action so
// a closer spawn reads better.
constexpr float k_spawn_min_m     = 3000.0f;
constexpr float k_spawn_max_m     = 5000.0f;
constexpr float k_min_sep_m       = 600.0f;

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

// Wave B payloads. `spawn` is filled iff the JSON declared `spawn_on_accept`
// (else has_spawn_on_accept stays false and we ignore the struct); same
// for `reward`. Defaulted ints / vectors keep a degenerate JSON from crashing.
struct SpawnPayload {
    std::string faction;     // faction::from_name key, e.g. "kilrathi"
    std::string class_name;  // ship_class::find key, e.g. "dralthi"
    int         count    = 0;
    float       delay_s  = 0.0f;
};

struct RepDelta {
    std::string faction;  // faction::from_name key, e.g. "confed"
    int         delta  = 0;
};

struct RewardPayload {
    int64_t                  credits  = 0;
    std::vector<RepDelta>    rep;
    std::string              loot_roll;   // Phase-4 deferred
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

    // Wave B payloads. has_* flags are authoritative — a follow-up wave
    // can still branch on them without re-parsing JSON.
    bool            has_spawn_on_accept = false;
    SpawnPayload    spawn;
    bool            has_reward          = false;
    RewardPayload   reward;
};

// ---- module state ---------------------------------------------------------
std::vector<Scenario>       g_scenarios;
std::vector<ScenarioState>  g_state;

// Wave C (audio): scenario_id -> [clip paths], one entry per turn. Missing
// id or OOB turn means "no audio for this turn" and the text still prints.
std::unordered_map<std::string, std::vector<std::string>> g_voice_clips;

// Wave C (audio): path -> resolved SampleId. Caches 0 too so we don't
// retry a missing-file path every tick. Session-scoped (no unload).
std::unordered_map<std::string, SampleId> g_clip_cache;

// Active playback (one scenario at a time). All fields are only
// meaningful while `active_idx >= 0`. Wave B splits the old monolithic
// "dialogue loop" into a three-phase machine.
enum class ActivePhase : uint8_t { Dialogue, Spawning, AwaitingResolution };

int             active_idx       = -1;
ActivePhase     active_phase     = ActivePhase::Dialogue;
int             active_turn      = 0;    // next turn to emit (Dialogue only)
float           active_next_at   = 0.0f; // wall-clock seconds
uint32_t        active_anchor_id = 0;    // nearest matching ship's id
std::vector<uint32_t> active_wing;       // spawned ship ids (Spawning/Awaiting)

// Deterministic RNG (hailing.cpp pattern). Cosmetically seeded.
std::mt19937& rng() {
    static std::mt19937 r{0xDEC1DEFu ^ 0xCAFEu};
    return r;
}

// ---- helpers --------------------------------------------------------------

// Pretty display name for the feed: "Confederation" instead of faction's
// raw lowercase catalogue id. Same policy as hailing.cpp.
const char* display_name(Faction f) {
    switch (f) {
        case Faction::Militia: return "Militia";
        case Faction::Confed:  return "Confederation";
        default:               return faction::to_name(f);
    }
}

// Map a turn's `voice` string to a feed display name. faction::from_name
// -> display_name; else capitalized raw string; else the raw string
// verbatim. Unknown voices (e.g. "confed_f" alias) fall through to
// capitalized() so they read naturally.
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

// Wave C (audio): resolve a clip path to a SampleId via a lazy cache.
// First call hits audio::load(); subsequent calls reuse the cached id.
// Caches 0 too so we don't retry a missing-file path every tick.
SampleId resolve_clip(const std::string& path) {
    auto it = g_clip_cache.find(path);
    if (it != g_clip_cache.end()) return it->second;
    const SampleId sid = audio::load(path);
    g_clip_cache.emplace(path, sid);
    return sid;
}

// Wave C (audio): play the clip for one dialogue turn if the manifest
// has one. Resolves + caches via resolve_clip(); skips silently when
// the scenario has no clip for this turn or load() returned 0. 2D
// player-directed gain matches the comm chatter convention.
void play_turn_clip(const std::string& scenario_id, int turn_index) {
    auto it = g_voice_clips.find(scenario_id);
    if (it == g_voice_clips.end()) return;
    const std::vector<std::string>& clips = it->second;
    if (turn_index < 0 || turn_index >= static_cast<int>(clips.size())) return;
    const std::string& path = clips[turn_index];
    if (path.empty()) return;
    if (SampleId sid = resolve_clip(path); sid != 0) {
        audio::play(sid, /*gain=*/1.0f);
    }
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

// Parse one `spawn_on_accept` object into SpawnPayload. Best-effort:
// a missing faction/class/count fall through to defaults; the call
// site uses `has_spawn_on_accept` (set in the wrapper) to gate use.
void parse_spawn_on_accept(const json::Value& v, Scenario& s) {
    if (!v.is_object()) return;
    if (const json::Value* f = v.find("faction"); f && f->is_string())
        s.spawn.faction = f->as_string();
    if (const json::Value* c = v.find("class"); c && c->is_string())
        s.spawn.class_name = c->as_string();
    if (const json::Value* n = v.find("count"); n && n->is_number())
        s.spawn.count = std::max(0, (int)n->as_number());
    if (const json::Value* d = v.find("delay_s"); d && d->is_number())
        s.spawn.delay_s = (float)d->as_number();
}

// Parse one `reward` object into RewardPayload. `rep` is an object of
// {faction_name: delta} entries — materialise each into RepDelta so
// the payout path can iterate without re-resolving the faction.
void parse_reward(const json::Value& v, Scenario& s) {
    if (!v.is_object()) return;
    if (const json::Value* cr = v.find("credits"); cr && cr->is_number())
        s.reward.credits = (int64_t)cr->as_number();
    if (const json::Value* lp = v.find("loot_roll"); lp && lp->is_string())
        s.reward.loot_roll = lp->as_string();
    if (const json::Value* rep = v.find("rep"); rep && rep->is_object()) {
        for (const auto& kv : rep->as_object()) {
            const json::Value& rv = kv.second;
            if (!rv.is_number()) continue;
            s.reward.rep.push_back(RepDelta{ kv.first, (int)rv.as_number() });
        }
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
            if (line.empty()) continue;
            s.dialogue.push_back(Turn{voice, line});
        }
    }

    // Wave B: capture the full payload (not just presence) so the playback
    // path can branch on `has_*` AND read the parsed values off the struct.
    if (const json::Value* sa = v.find("spawn_on_accept"); sa && sa->is_object()) {
        s.has_spawn_on_accept = true;
        parse_spawn_on_accept(*sa, s);
    }
    if (const json::Value* rw = v.find("reward"); rw && rw->is_object()) {
        s.has_reward = true;
        parse_reward(*rw, s);
    }
}

// Wave C (audio): load the scenario-id -> [clip paths] manifest.
// Missing file = silent no-op (text still prints, player just doesn't
// hear it).
void load_voice_clips(const std::string& path) {
    g_voice_clips.clear();
    json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[scripted] no voice manifest at '%s' — text-only\n",
                     path.c_str());
        return;
    }
    for (const auto& kv : root.as_object()) {
        const json::Value& v = kv.second;
        if (!v.is_array()) continue;
        std::vector<std::string> clips;
        for (const json::Value& c : v.as_array()) {
            if (c.is_string()) clips.push_back(c.as_string());
        }
        g_voice_clips.emplace(kv.first, std::move(clips));
    }
    std::printf("[scripted] loaded voice clips for %zu scenarios from '%s'\n",
                g_voice_clips.size(), path.c_str());
}

// ---- playback helpers ----------------------------------------------------

// Resolve the world point a spawn should anchor on: live position of the
// scenario's anchor ship (looked up THIS call, not at dialogue time —
// it may have moved or despawned across the delay_s window), or the
// player's position as a safe fallback.
HMM_Vec3 spawn_anchor_pos(const ShipRegistry& ships,
                          uint32_t anchor_id,
                          const Ship& player_ship) {
    if (anchor_id != 0) {
        if (const Ship* s = ships.find_by_id(anchor_id)) {
            if (s->alive) return s->position;
        }
    }
    return player_ship.position;
}

// Pick a position for one spawn on the 3-5 km shell around `anchor`,
// biased to keep at least k_min_sep_m from every wingmate. Best-effort:
// falls back to the least-crowded candidate if 8 tries can't satisfy.
HMM_Vec3 pick_spawn_point(HMM_Vec3 anchor, std::mt19937& rng_used,
                          std::vector<HMM_Vec3>& placed) {
    std::uniform_real_distribution<float> uz(-1.0f, 1.0f);
    std::uniform_real_distribution<float> ua(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> ud(k_spawn_min_m, k_spawn_max_m);
    HMM_Vec3 best = anchor;
    float    best_slack = -1e30f;
    for (int tries = 0; tries < 8; ++tries) {
        // Marsaglia-style uniform unit vector (no trig-bias poles).
        const float z = uz(rng_used);
        const float a = ua(rng_used);
        const float s = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const HMM_Vec3 dir{ s * std::cos(a), z, s * std::sin(a) };
        const HMM_Vec3 p = {
            anchor.X + dir.X * ud(rng_used),
            anchor.Y + dir.Y * ud(rng_used),
            anchor.Z + dir.Z * ud(rng_used),
        };
        float worst = 1e30f;
        for (const HMM_Vec3& q : placed) {
            const float dx = p.X - q.X, dy = p.Y - q.Y, dz = p.Z - q.Z;
            const float d  = std::sqrt(dx*dx + dy*dy + dz*dz);
            worst = std::min(worst, d - k_min_sep_m);
        }
        if (worst >= 0.0f) {
            placed.push_back(p);
            return p;
        }
        if (worst > best_slack) { best_slack = worst; best = p; }
    }
    placed.push_back(best);
    return best;
}

} // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void load(const std::string& path) {
    g_scenarios.clear();
    g_state.clear();
    active_idx       = -1;
    active_phase     = ActivePhase::Dialogue;
    active_turn      = 0;
    active_next_at   = 0.0f;
    active_anchor_id = 0;
    active_wing.clear();

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

    // Wave C (audio): also load the per-scenario dialogue clip map.
    // Non-fatal on missing/unparseable — dialogue falls back to text.
    load_voice_clips("assets/data/scenario_voice.json");
}

void reset() {
    for (auto& s : g_state) {
        s.triggered   = false;
        s.last_fire_s = -1e9f;
    }
    active_idx       = -1;
    active_phase     = ActivePhase::Dialogue;
    active_turn      = 0;
    active_next_at   = 0.0f;
    active_anchor_id = 0;
    active_wing.clear();
}

void tick(ShipRegistry& ships, const Ship& player_ship, PlayerState& player,
          float now_s, const encounters::SpawnFn& spawn) {
    // ---- active playback -----------------------------------------------
    // Only ONE scenario plays at a time. While one is mid-playback we
    // dispatch by phase and short-circuit out — the trigger loop runs
    // again on the tick after playback completes.
    if (active_idx >= 0) {
        if (now_s < active_next_at) return;
        if (active_idx >= static_cast<int>(g_scenarios.size())) {
            // Stale index (scenario list shrank on reload) — drop playback.
            active_idx       = -1;
            active_phase     = ActivePhase::Dialogue;
            active_anchor_id = 0;
            active_wing.clear();
            return;
        }
        const Scenario& sc = g_scenarios[active_idx];

        // ---- PHASE: DIALOGUE ------------------------------------------
        if (active_phase == ActivePhase::Dialogue) {
            const Turn& turn = sc.dialogue[active_turn];
            const std::string disp = speaker_display(turn.voice);
            comm::push(format_feed(disp, turn.line), /*taunt=*/true);
            std::printf("[scenario] %s turn %d: %s\n",
                        sc.id.c_str(), active_turn, turn.line.c_str());
            play_turn_clip(sc.id, active_turn);

            ++active_turn;

            if (active_turn >= static_cast<int>(sc.dialogue.size())) {
                // Last turn done. Mark triggered + stamp cooldown so
                // once_per scenarios retire and re-fireables cool down.
                std::printf("[scenario] %s complete\n", sc.id.c_str());
                g_state[active_idx].triggered   = true;
                g_state[active_idx].last_fire_s = now_s;

                // spawn => Spawning (delay_s timer); reward-only =>
                // straight to AwaitingResolution; neither => retire.
                if (sc.has_spawn_on_accept && sc.spawn.count > 0) {
                    active_phase   = ActivePhase::Spawning;
                    active_next_at = now_s + sc.spawn.delay_s;
                } else if (sc.has_reward) {
                    active_phase   = ActivePhase::AwaitingResolution;
                    active_next_at = now_s;
                } else {
                    active_idx       = -1;
                    active_phase     = ActivePhase::Dialogue;
                    active_anchor_id = 0;
                }
                return;
            }

            active_next_at = now_s + k_turn_gap_s;
            return;
        }

        // ---- PHASE: SPAWNING -------------------------------------------
        // Delay timer elapsed: build the SpawnRequest per ship and pump
        // through the host SpawnFn. Each successful spawn id goes into
        // `active_wing` so AwaitingResolution can prune + detect "all
        // dead". Kilrathi vs Confed is hostile by stance, so default
        // Patrol AI engages on its own — no special initial_ai_state.
        if (active_phase == ActivePhase::Spawning) {
            const Faction fac = faction::from_name(sc.spawn.faction);
            // Unknown faction: degrade to Pirate (safe "outlaw" stance)
            // rather than refuse the spawn — JSON typo recovers gracefully.
            const Faction effective_fac = (fac == Faction::Count)
                ? Faction::Pirate : fac;

            const HMM_Vec3 anchor_pos =
                spawn_anchor_pos(ships, active_anchor_id, player_ship);

            int spawned_count = 0;
            std::vector<HMM_Vec3> placed;
            placed.reserve(sc.spawn.count);
            for (int i = 0; i < sc.spawn.count; ++i) {
                encounters::SpawnRequest req;
                req.class_name      = sc.spawn.class_name;
                req.faction         = effective_fac;
                req.position        = pick_spawn_point(anchor_pos, rng(), placed);
                req.initial_ai_state = AIState::Patrol;
                req.patrol_anchor   = req.position;
                const uint32_t id = spawn(req);
                if (id != 0) {
                    active_wing.push_back(id);
                    ++spawned_count;
                }
            }

            std::printf("[scenario] %s spawned %d %s\n",
                        sc.id.c_str(), spawned_count,
                        sc.spawn.class_name.c_str());

            active_phase   = ActivePhase::AwaitingResolution;
            active_next_at = now_s;
            return;
        }

        // ---- PHASE: AWAITING RESOLUTION -------------------------------
        // Prune dead/gone wing members, then grant the reward once the
        // wing is empty. "Wing empty" works for the no-spawn path too
        // (wing was never populated, resolves on first tick of phase).
        if (active_phase == ActivePhase::AwaitingResolution) {
            active_wing.erase(
                std::remove_if(active_wing.begin(), active_wing.end(),
                    [&](uint32_t id) {
                        const Ship* s = ships.find_by_id(id);
                        return !s || !s->alive;
                    }),
                active_wing.end());

            if (active_wing.empty()) {
                if (sc.has_reward) {
                    if (sc.reward.credits != 0) {
                        player::add_credits(player, sc.reward.credits);
                    }

                    // Apply each rep delta + build summary in the same
                    // pass so the printf + store stay in lockstep.
                    std::string rep_summary;
                    if (sc.reward.rep.empty()) rep_summary = "(none)";
                    for (const RepDelta& rd : sc.reward.rep) {
                        const Faction rf = faction::from_name(rd.faction);
                        if (rf == Faction::Count) continue;   // skip unknown keys silently
                        const int idx = (int)rf;
                        const int before = (int)player.rep.rep[idx];
                        const int after  = std::clamp(before + rd.delta, -100, 100);
                        player.rep.rep[idx] = (int8_t)after;
                        if (!rep_summary.empty()) rep_summary += ", ";
                        rep_summary += rd.faction;
                        rep_summary += ":";
                        rep_summary += std::to_string(rd.delta);
                    }

                    std::printf("[scenario] %s reward: +%lld cr, rep %s\n",
                                sc.id.c_str(),
                                (long long)sc.reward.credits,
                                rep_summary.c_str());

                    if (!sc.reward.loot_roll.empty()) {
                        std::printf("[scenario] %s loot_roll '%s' (deferred to Phase 4)\n",
                                    sc.id.c_str(),
                                    sc.reward.loot_roll.c_str());
                    }
                } else {
                    std::printf("[scenario] %s complete\n", sc.id.c_str());
                }

                // Free the slot: trigger evaluation will resume next tick.
                active_idx       = -1;
                active_phase     = ActivePhase::Dialogue;
                active_anchor_id = 0;
                active_wing.clear();
            }
            return;
        }
    }

    // ---- trigger evaluation --------------------------------------------
    // One scenario considered per tick (per spec). The order is the
    // load order of the JSON file — authors control priority by
    // ordering entries.
    for (size_t i = 0; i < g_scenarios.size(); ++i) {
        const Scenario& sc = g_scenarios[i];

        // ---- eligibility gates --------------------------------------
        // once_per_system scenarios are permanently retired after their
        // first fire (success OR miss) — they're a one-shot.
        if (sc.once_per_system && g_state[i].triggered) continue;
        // Cooldown gate for non-once_per_system. last_fire_s starts at
        // -1e9 so the first tick always passes regardless of cooldown_s.
        if (!sc.once_per_system) {
            if ((now_s - g_state[i].last_fire_s) < sc.cooldown_s) continue;
        }

        // ---- candidate scan ------------------------------------------
        // Resolved trigger faction. Unknown names skip silently — an
        // authoring error should be fixed in the JSON, not nagged.
        const Faction target_fac = faction::from_name(sc.near_faction);
        if (target_fac == Faction::Count) continue;

        const float range_sq = k_trigger_range_m * k_trigger_range_m;

        int      match_count     = 0;
        uint32_t nearest_id      = 0;
        float    nearest_dist_sq = 1e30f;

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
        // Spec: "if at least one match AND count <= max_count". Too many
        // matches = scenario doesn't fit; mark tried + break.
        if (match_count > sc.max_count) {
            if (sc.once_per_system) g_state[i].triggered = true;
            else                    g_state[i].last_fire_s = now_s;
            break;
        }

        // ---- chance roll ---------------------------------------------
        // Uniform 0..1 draw; anything strictly below `chance` is a HIT.
        // chance==0 -> always miss; chance==1 -> always hit. Mirrors
        // hailing.cpp's `r >= chance` guard.
        std::uniform_real_distribution<float> pick(0.0f, 1.0f);
        const float r = pick(rng());
        if (r >= sc.chance) {
            // MISS — retire (once_per) or stamp cooldown.
            if (sc.once_per_system) g_state[i].triggered = true;
            else                    g_state[i].last_fire_s = now_s;
            break;
        }

        // HIT — start playback. active_next_at = now so the very next
        // tick emits the first turn (now_s >= active_next_at passes).
        active_idx       = static_cast<int>(i);
        active_phase     = ActivePhase::Dialogue;
        active_turn      = 0;
        active_next_at   = now_s;
        active_anchor_id = nearest_id;
        break;
    }
}

} // namespace scripted

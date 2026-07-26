// -----------------------------------------------------------------------------
// music.cpp — the dynamic MUSIC DIRECTOR (np-ida): track table, the loop-layer
// crossfade, the one-shot stings, and the whole state->track policy.
//
// All main-thread (the mixer's threading contract is audio.cpp's problem).
// See music.h for the design; the per-number tuning notes live next to the
// constants below. The header's label mapping (docs/music_labels.json) is the
// spec; k_track_file is its concrete WAV-stem binding.
// -----------------------------------------------------------------------------

#include "music.h"

#include "audio.h"
#include "base_screens.h"
#include "json.h"
#include "threat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

// ---- track table --------------------------------------------------------------
// One SampleId per Track (0 = unavailable). The stems mirror the gitignored
// per-sub-song WAVs in assets/music/original/ (rendered by tools/render_music.py
// from the COMBAT/BASETUNE/OPENING containers). The names are the SAME positional
// stems the F8 labeler uses, so the label mapping in music.h binds 1:1 here.
// No committed placeholders — silence is the fallback (clean clone runs mute).
SampleId g_samples [(int)music::Track::Count] = {0};
float    g_duration[(int)music::Track::Count] = {0};   // seconds, for the outro

const char* k_track_file[(int)music::Track::Count] = {
    nullptr,        // None
    "combat_04",    // FlightMain       — main flight theme (no hostiles)
    "combat_05",    // CombatFar        — hostiles present, nearest > 5km
    "combat_06",    // CombatNear       — hostiles present, nearest <= 5km
    "combat_07",    // CombatResolve    — combat-ended outro, feeds the main loop
    "combat_08",    // StingJump        — jump executed (one-shot)
    "combat_09",    // StingLanding     — landing-zone approach (one-shot; deferred)
    "combat_10",    // StingDeath       — game over (one-shot)
    "basetune_00",  // BaseAgricultural — agricultural base tune
    "basetune_04",  // BaseMining       — mining base tune
    "bar_music_01", // BaseBar          — first track in the bar pool (14 total)
    "menu",         // Menu             — title/menu loop (assets/music/original/menu.wav, custom bed)
};

// Which tracks are LOOPING beds (the crossfade layer) vs short one-shot STINGS
// (fired straight onto the music bus over whatever loop is playing). Loops go
// through audio::play_loop (np-3va-uncullable); stings through audio::play.
bool is_loop(music::Track t) {
    switch (t) {
        case music::Track::StingJump:
        case music::Track::StingLanding:
        case music::Track::StingDeath:
            return false;          // one-shots
        case music::Track::BaseBar:
            return false;          // bar music is a non-looping pool
        default:
            return true;           // every bed (incl. None handled by caller)
    }
}

// ---- crossfade voices (LOOP layer) -------------------------------------------
// Two music slots, ping-pong. g_active is the incoming/holding bed; g_prev is
// an outgoing bed fading to silence (then stopped). At most these two music
// voices are ever live — both are play_loop voices, so np-3va never culls them
// and they never steal an SFX.
struct Slot {
    VoiceId voice = 0;
    float   gain  = 0.0f;   // current (lerped)
};
Slot         g_active;
Slot         g_prev;
music::Track g_target = music::Track::None;   // crossfade destination (loop)

// ---- master "bus" + mute ------------------------------------------------------
// Music gets its own master volume, independent of the SFX mix. Default a touch
// under the SFX (0.55) so the score underscores rather than buries the action.
// Muting fades the bed to silence but keeps the selection logic running.
float g_master = 0.55f;
bool  g_muted  = false;

// ---- flight combat-tier state machine ----------------------------------------
// The in-flight music is a 4-state machine driven by the NEAREST hostile's
// distance (threat::nearest_hostile_distance). Tiers, and the policy:
//   Main    — no hostiles in scope            -> FlightMain  (combat_04)
//   Far     — hostiles in scope, nearest >5km -> CombatFar   (combat_05)
//   Near    — hostiles in scope, nearest<=5km -> CombatNear  (combat_06)
//   Resolve — combat just ended; outro once   -> CombatResolve(combat_07) -> Main
enum class Tier { Main, Far, Near, Resolve };
Tier  g_tier          = Tier::Main;

// "In scope" = a hostile within this bubble flips us into combat at all. Matches
// the autopilot/jump danger radius so the music switches exactly when the danger
// gate does (autopilot::k_threat_radius_m == 8km).
constexpr float k_threat_radius_m = 8000.0f;

// The Far<->Near boundary, with hysteresis so a hostile hovering on the 5km
// line can't strobe the two combat loops. We only flip to Near below 4.5km and
// back to Far above 5.5km; in the 1km dead-band the current tier holds.
constexpr float k_tier_line_m = 5000.0f;
constexpr float k_tier_hyst_m = 500.0f;

// Once in combat, stay in combat for this grace after the LAST in-scope frame,
// so a hostile briefly dipping off the scope edge / dying doesn't instantly cut
// the music. When the grace runs out we play the Resolve outro.
constexpr float k_combat_hold_s = 6.0f;
float g_combat_hold = 0.0f;
bool  g_was_combat  = false;

// Resolve outro countdown — plays combat_07 once through (its natural length,
// read from the WAV at load; a safe fallback if unknown) and then feeds back to
// the FlightMain loop, exactly as the label describes ("feeds into main loop").
constexpr float k_resolve_fallback_s = 8.0f;
float g_resolve_timer = 0.0f;

// ---- landed per-base archetype cache -----------------------------------------
// Landed picks a bed by the docked base's market archetype (assets/bases/<id>/
// base.json -> market.archetype). Resolving that means a tiny JSON read, so we
// cache the last base_id we looked up and only re-read when it changes.
std::string  g_cached_base_id;
music::Track g_cached_base_track = music::Track::None;

// ---- mode-edge sting tracking ------------------------------------------------
// Stings fire on the FRAME we ENTER a mode (jump on Loading, death on Dying),
// not every frame we're in it. We remember the previous mode to detect the edge.
GameMode g_prev_mode = GameMode::Flight;
bool     g_have_prev = false;

// ---- active sting tracking (issue #23) --------------------------------------
// One short sting is on the music bus at a time (jump/landing/death are
// exclusive events). We track its VoiceId so a bed transition can FADE it
// out cleanly instead of leaving it to play over the new bed — the landing
// sting in particular would otherwise ride over the base music that just
// started. (Death-time transition to Track::None leaves the sting to finish
// naturally; only non-None transitions kick the fade.)
VoiceId g_active_sting      = 0;
float   g_active_sting_gain = 0.0f;     // current gain on the music bus
bool    g_active_sting_fading = false;  // true = bed swap marked us to fade

// Crossfade speed: exponential lerp ~1.5/s (~0.67s time constant) — a quick but
// click-free transition that matches Privateer's snappy combat swap without a
// hard cut. Same rate fades the master/mute changes.
constexpr float k_fade_rate = 1.5f;

// Bar tracks are a shuffled one-shot pool, separate from the looping bed.

VoiceId g_bar_voice = 0;             // current bar music one-shot voice
float   g_bar_elapsed = 0.0f;        // elapsed time on current bar track
int     g_bar_idx = 0;               // current index in shuffled pool
int     g_bar_count = 0;             // available files (0 means silent fallback)
int     g_bar_pool[14] = {};         // shuffled track indices (0–13)
int     g_bar_override = 0;          // 1..14 forces bar_music_NN; 0 = shuffle
music::Track g_bar_prior = music::Track::None;  // what was playing before bar

float wav_seconds(const char* path);

float clamp01(float v) { return std::fmax(0.0f, std::fmin(1.0f, v)); }

bool available(music::Track t) {
    const int i = (int)t;
    return i > 0 && i < (int)music::Track::Count && g_samples[i] != 0;
}

// Is the player in the Bar screen right now?
bool in_bar_screen() {
    auto st = base_screens::dev_state();
    if (st.stack.empty()) return false;
    return st.stack.back() == "Bar";
}

void play_next_bar_track() {
    if (g_bar_count == 0 && g_bar_override == 0) return;

    int track_idx;
    if (g_bar_override > 0) {
        track_idx = g_bar_override;             // held for the scene
    } else {
        track_idx = g_bar_pool[g_bar_idx] + 1;  // 1..14
        g_bar_idx = (g_bar_idx + 1) % g_bar_count;
    }

    char path[256];
    std::snprintf(path, sizeof path, "assets/music/original/bar_music_%02d.wav", track_idx);

    SampleId sid = audio::load(path); // audio::load deduplicates by path
    if (sid == 0) return;

    float dur = wav_seconds(path);

    if (g_bar_voice != 0) { audio::stop(g_bar_voice); g_bar_voice = 0; }

    float gain = clamp01(g_master);
    g_bar_voice = audio::play(sid, gain);
    g_bar_elapsed = 0.0f;
    std::printf("[music] bar track %02d (%.1fs)\n", track_idx, dur);
}

void update_bar_track(float dt) {
    if (g_bar_voice == 0) return;

    float fl = 0, fr = 0;
    if (!audio::voice_gains(g_bar_voice, &fl, &fr)) {
        play_next_bar_track();
        return;
    }

    // Defensive cap in case a malformed voice never retires.
    g_bar_elapsed += dt;
    if (g_bar_elapsed > 300.0f) play_next_bar_track();
}

// ---- WAV duration (seconds) --------------------------------------------------
// A hand-rolled RIFF peek (no decode): fmt chunk byte-rate + data chunk size.
// Same no-deps ethos as audio.cpp's WAV loader; used only to time the outro.
float wav_seconds(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0.0f;
    char riff[12];
    f.read(riff, 12);
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0)
        return 0.0f;
    uint32_t byte_rate = 0, data_size = 0;
    char id[4];
    uint32_t sz = 0;
    while (f.read(id, 4) && f.read(reinterpret_cast<char*>(&sz), 4)) {
        if (std::memcmp(id, "fmt ", 4) == 0) {
            char fmt[16] = {0};
            const uint32_t want = sz < 16 ? sz : 16;
            f.read(fmt, want);
            std::memcpy(&byte_rate, fmt + 8, 4);    // bytes/sec field
            if (sz > want) f.seekg(sz - want, std::ios::cur);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_size = sz;
            break;
        } else {
            f.seekg((sz + 1) & ~1u, std::ios::cur); // chunks are word-aligned
        }
    }
    if (byte_rate == 0) return 0.0f;
    return (float)data_size / (float)byte_rate;
}

// Begin a crossfade of the LOOP layer to `t`. Moves the current active voice to
// the outgoing slot (hard-stopping any older outgoing voice first so we never
// stack three) and starts the new bed on a fresh looping voice at gain 0.
void start_crossfade(music::Track t) {
    if (g_prev.voice != 0) {            // retire any still-fading previous voice
        audio::stop(g_prev.voice);
        g_prev.voice = 0;
        g_prev.gain  = 0.0f;
    }
    g_prev = g_active;                  // demote current -> outgoing (fades out)
    g_active.voice = 0;
    g_active.gain  = 0.0f;

    if (t != music::Track::None && available(t)) {
        g_active.voice = audio::play_loop(g_samples[(int)t], 0.0f);
        g_active.gain  = 0.0f;          // update() lerps it up
    }
    // Issue #23: when a bed swap brings in a NEW loop, fade out any active
    // sting cleanly. Without this the landing sting plays over the base
    // music we just started (the new bed is the BaseAgricultural/Mining
    // tune for the just-docked base). A None transition (death mode)
    // leaves the sting alone — it should play alone over the dropping loop.
    if (t != music::Track::None && g_active_sting != 0) {
        g_active_sting_fading = true;
    }
    std::printf("[music] -> %s\n", music::to_name(t));
}

// System nav data may suffix a base's folder id with its type
// (new_detroit_industrial, drake_pirate, ...). Resolve the same way the base
// renderer does so music and visuals cannot disagree about which definition
// the player docked at.
std::string resolve_base_folder(std::string id) {
    namespace fs = std::filesystem;
    auto exists = [](const std::string& candidate) {
        return fs::exists("assets/bases/" + candidate + "/base.json");
    };
    while (!id.empty() && !exists(id)) {
        const std::string::size_type split = id.rfind('_');
        if (split == std::string::npos) return {};
        id.resize(split);
    }
    return exists(id) ? id : std::string{};
}

// Resolve a docked base_id to its landed bed, caching the JSON read.
music::Track base_track_for(const char* base_id) {
    const std::string id = base_id ? base_id : "";
    if (id == g_cached_base_id) return g_cached_base_track;
    g_cached_base_id = id;
    g_cached_base_track = music::Track::BaseAgricultural;   // calm default
    const std::string folder = resolve_base_folder(id);
    if (!folder.empty()) {
        const json::Value root =
            json::parse_file("assets/bases/" + folder + "/base.json");
        if (root.is_object()) {
            if (const json::Value* m = root.find("market");
                m && m->is_object() && m->contains("archetype")) {
                const std::string arch = (*m)["archetype"].string_or("");
                // Issue #110: only "agricultural" should get the farm bed.
                // Refinery / pirate / military are industrial-feel, so they
                // share the mining bed for now (Option A — a dedicated bed
                // per archetype is a follow-up once BASETUNE sub-songs are
                // rendered). Unknown/missing archetype falls through to the
                // calm agricultural default, matching prior behaviour.
                if (arch == "mining"   || arch == "refinery" ||
                    arch == "pirate"   || arch == "military")
                    g_cached_base_track = music::Track::BaseMining;
                else
                    g_cached_base_track = music::Track::BaseAgricultural;
            }
        }
    }
    return g_cached_base_track;
}

music::Track tier_track(Tier tier) {
    switch (tier) {
        case Tier::Far:     return music::Track::CombatFar;
        case Tier::Near:    return music::Track::CombatNear;
        case Tier::Resolve: return music::Track::CombatResolve;
        case Tier::Main:
        default:            return music::Track::FlightMain;
    }
}

// Fire a one-shot sting onto the music bus (at the music gain), over the loop.
// No-op if it's unavailable or we're muted. The death sting also yanks the loop
// out from under it (caller handles that via the desired==None path).
void play_sting(music::Track t) {
    if (!available(t) || g_muted) return;
    const float g = clamp01(g_master);
    const VoiceId v = audio::play(g_samples[(int)t], g);
    if (v == 0) return;                         // pool full / init failed
    if (g_active_sting != 0) audio::stop(g_active_sting);   // 1 sting at a time
    g_active_sting       = v;
    g_active_sting_gain  = g;
    g_active_sting_fading = false;
    std::printf("[music] sting %s\n", music::to_name(t));
}

// Advance the flight combat-tier state machine one frame given the nearest
// hostile distance, returning the desired LOOP bed.
music::Track flight_desired(HMM_Vec3 pos, float dt) {
    const float d = threat::nearest_hostile_distance(pos);
    const bool  in_scope = d < k_threat_radius_m;

    if (in_scope) {
        g_combat_hold = k_combat_hold_s;
        g_was_combat  = true;
        // Far<->Near with a 1km dead-band around the 5km line.
        if (g_tier == Tier::Near) {
            if (d >= k_tier_line_m + k_tier_hyst_m) g_tier = Tier::Far;
        } else {
            g_tier = (d <= k_tier_line_m - k_tier_hyst_m) ? Tier::Near : Tier::Far;
        }
    } else if (g_combat_hold > 0.0f) {
        g_combat_hold = std::fmax(0.0f, g_combat_hold - dt);   // hold the tier
    } else if (g_was_combat) {
        g_was_combat   = false;                                 // combat ended
        g_tier         = Tier::Resolve;
        const float dur = g_duration[(int)music::Track::CombatResolve];
        g_resolve_timer = dur > 0.1f ? dur : k_resolve_fallback_s;
    } else if (g_tier == Tier::Resolve) {
        g_resolve_timer -= dt;
        if (g_resolve_timer <= 0.0f) g_tier = Tier::Main;       // feed the main loop
    } else {
        g_tier = Tier::Main;
    }
    return tier_track(g_tier);
}

void reset_combat_state() {
    g_tier          = Tier::Main;
    g_combat_hold   = 0.0f;
    g_was_combat    = false;
    g_resolve_timer = 0.0f;
}

} // namespace

namespace music {

void load_all() {
    int loaded = 0;
    for (int i = 1; i < (int)Track::Count; ++i) {
        // Skip BaseBar — it's a pool of 14 non-looping files, not a single
        // mapped stem. We handle it separately below.
        if ((Track)i == Track::BaseBar) continue;

        char path[256];
        std::snprintf(path, sizeof path,
                      "assets/music/original/%s.wav", k_track_file[i]);
        if (std::ifstream(path).good()) {
            const SampleId id = audio::load(path);
            g_samples [i] = id;
            g_duration[i] = (id != 0) ? wav_seconds(path) : 0.0f;
            if (id != 0) {
                ++loaded;
                std::printf("[music] %-16s <- %s (%.1fs)\n",
                            to_name((Track)i), k_track_file[i], g_duration[i]);
            } else {
                std::printf("[music] %-16s <- %s FAILED to load\n",
                            to_name((Track)i), k_track_file[i]);
            }
        } else {
            std::printf("[music] %-16s <- %s (none; silent)\n",
                        to_name((Track)i), k_track_file[i]);
        }
    }

    // ---- bar music pool (14 non-looping tracks) ----
    // Build and shuffle the pool. Tracks that exist on disk are included;
    // missing ones are skipped (clean clone has none and bar music is silent).
    {
        std::vector<int> pool;
        for (int i = 0; i < 14; ++i) {
            char path[256];
            std::snprintf(path, sizeof path, "assets/music/original/bar_music_%02d.wav", i + 1);
            if (std::ifstream(path).good()) pool.push_back(i);
        }
        if (!pool.empty()) {
            // Fisher-Yates shuffle.
            std::mt19937 rng(std::random_device{}());
            std::shuffle(pool.begin(), pool.end(), rng);
            g_bar_count = (int)pool.size();
            g_bar_idx = 0;
            for (int i = 0; i < g_bar_count; ++i)
                g_bar_pool[i] = pool[i];
            std::printf("[music] base_bar       <- bar_music_01..14 (%d tracks, shuffled)\n",
                        (int)pool.size());
        } else {
            g_bar_count = 0;
            std::memset(g_bar_pool, 0, sizeof(g_bar_pool));
            std::printf("[music] base_bar       <- (no bar music; silent)\n");
        }
    }

    std::printf("[music] loaded %d/%d beds/stings + %d bar tracks; master vol %.2f%s\n",
                loaded, (int)Track::Count - 2, g_bar_count, g_master,
                loaded == 0 && g_bar_count == 0 ? " (no music — clean clone)" : "");
}

void play_track(Track t) {
    if (t == g_target) return;                       // already heading there
    if (t != Track::None && !available(t)) return;   // unavailable — no-op
    if (t != Track::None && !is_loop(t)) return;      // stings aren't bed targets
    g_target = t;
    start_crossfade(t);
}

void stop() { play_track(Track::None); }

void request_bar_track(int idx) {
    if (idx < 0 || idx > 14) idx = 0;
    if (g_bar_override == idx) return;
    g_bar_override = idx;
    std::printf("[music] bar override -> %d%s\n", idx,
                idx == 0 ? " (shuffle)" : "");
    // Swap immediately if bar music is currently playing.
    if (g_bar_voice != 0) play_next_bar_track();
}

int bar_track_override() { return g_bar_override; }

void landing_approach() { play_sting(music::Track::StingLanding); }

void update(GameMode mode, HMM_Vec3 player_pos, const char* base_id, float dt) {
    // ---- 0. mode-edge stings ------------------------------------------------
    // Fire on the frame we ENTER Loading (jump) / Dying (death), once per edge.
    if (!g_have_prev) { g_prev_mode = mode; g_have_prev = true; }
    if (mode != g_prev_mode) {
        if (mode == GameMode::Loading)    play_sting(Track::StingJump);
        else if (mode == GameMode::Dying) play_sting(Track::StingDeath);
        g_prev_mode = mode;
    }

    // ---- 1. state -> desired LOOP bed ---------------------------------------
    Track desired = g_target;
    bool bar_open = false;

    if (mode == GameMode::Landed)
        bar_open = in_bar_screen();

    switch (mode) {
        case GameMode::Flight:
            desired = flight_desired(player_pos, dt);
            break;
        case GameMode::Landed:
            reset_combat_state();
            if (!bar_open && g_bar_voice == 0)
                desired = base_track_for(base_id);
            break;
        case GameMode::Dying:
            reset_combat_state();
            desired = Track::None;          // the loop drops out under the sting
            break;
        case GameMode::Menu:
            // Title / main menu (np-3dp.6). Plays the OPENING bed (loaded
            // but unused before this). dt=0 here so no lerps run; we just
            // pin the target.
            desired = Track::Menu;
            break;
        case GameMode::Loading:
        default:
            // Hold whatever bed is playing across the jump/load beat (the jump
            // sting already overlays it); don't reselect.
            break;
    }

    // Bar music replaces (never overlays) the landed bed.
    if (mode == GameMode::Landed) {
        if (bar_open && g_bar_voice == 0) {
            g_bar_prior = desired;
            if (g_active.voice != 0) { audio::stop(g_active.voice); g_active.voice = 0; }
            if (g_prev.voice != 0) { audio::stop(g_prev.voice); g_prev.voice = 0; }
            g_target = Track::None;
            play_next_bar_track();
            desired = Track::None;   // override switch result
        } else if (!bar_open && g_bar_voice != 0) {
            if (g_bar_voice != 0) { audio::stop(g_bar_voice); g_bar_voice = 0; }
            play_track(g_bar_prior);
            std::printf("[music] bar closed -> %s\n", to_name(g_bar_prior));
            desired = g_bar_prior;  // override switch result
        }
    } else if (g_bar_voice != 0) {
        if (g_bar_voice != 0) { audio::stop(g_bar_voice); g_bar_voice = 0; }
    }
    // Only switch to an available bed (or to silence). Falling back keeps the
    // current bed if the desired one wasn't rendered in this clone.
    if (desired == Track::None || available(desired))
        play_track(desired);

    // Bar is a one-shot voice, so advance it and apply bus gain explicitly.
    update_bar_track(dt);
    if (g_bar_voice != 0)
        audio::set_voice_gain(g_bar_voice, g_muted ? 0.0f : clamp01(g_master));

    // ---- 2. advance the crossfade lerps -------------------------------------
    const float k = 1.0f - std::exp(-k_fade_rate * dt);
    const float active_target =
        (g_muted || g_active.voice == 0) ? 0.0f : clamp01(g_master);

    if (g_active.voice != 0) {
        g_active.gain += (active_target - g_active.gain) * k;
        audio::set_voice_gain(g_active.voice, g_active.gain);
    }
    if (g_prev.voice != 0) {
        g_prev.gain += (0.0f - g_prev.gain) * k;
        audio::set_voice_gain(g_prev.voice, g_prev.gain);
        if (g_prev.gain < 0.003f) {        // faded out — retire the slot
            audio::stop(g_prev.voice);
            g_prev.voice = 0;
            g_prev.gain  = 0.0f;
        }
    }

    // Active sting fade (issue #23): if a bed swap marked the active sting
    // for fading, lerp its gain to 0 with the same k as the loop layer so
    // they finish together. Retired once it's silent.
    if (g_active_sting != 0 && g_active_sting_fading) {
        g_active_sting_gain += (0.0f - g_active_sting_gain) * k;
        audio::set_voice_gain(g_active_sting, g_active_sting_gain);
        if (g_active_sting_gain < 0.003f) {
            audio::stop(g_active_sting);
            g_active_sting        = 0;
            g_active_sting_gain   = 0.0f;
            g_active_sting_fading = false;
        }
    }
}

void  set_master_volume(float v) { g_master = clamp01(v); }
float master_volume() { return g_master; }

void set_muted(bool m) { g_muted = m; }
bool muted() { return g_muted; }

Track current() { return g_bar_voice != 0 ? Track::BaseBar : g_target; }

bool any_loaded() {
    if (g_bar_count > 0) return true;
    for (int i = 1; i < (int)Track::Count; ++i)
        if (g_samples[i] != 0) return true;
    return false;
}

const char* to_name(Track t) {
    switch (t) {
        case Track::None:             return "none";
        case Track::FlightMain:       return "flight_main";
        case Track::CombatFar:        return "combat_far";
        case Track::CombatNear:       return "combat_near";
        case Track::CombatResolve:    return "combat_resolve";
        case Track::StingJump:        return "sting_jump";
        case Track::StingLanding:     return "sting_landing";
        case Track::StingDeath:       return "sting_death";
        case Track::BaseAgricultural: return "base_agricultural";
        case Track::BaseMining:       return "base_mining";
        case Track::BaseBar:          return "base_bar";
        case Track::Menu:             return "menu";
        default:                      return "?";
    }
}

} // namespace music

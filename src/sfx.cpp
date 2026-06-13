// -----------------------------------------------------------------------------
// sfx.cpp — event->sound policy: sample table, gains, radii, rate limits.
//
// Tuning notes live next to the numbers they tune. Everything here is
// main-thread (the audio mixer's threading contract is audio.cpp's
// problem, not ours).
// -----------------------------------------------------------------------------

#include "sfx.h"

#include "audio.h"
#include "gun.h"            // GunType, kGunTypeCount, gun::to_name
#include "sokol_time.h"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace {

// Resolve+load one event's WAV, preferring the local-only original over
// the committed procedural placeholder. The originals live in the
// GITIGNORED assets/sfx/original/ (converted from the user's OWN
// SOUNDFX.PAK by tools/extract_soundfx_pak.py — local use only, never
// committed); the placeholders in assets/sfx/ stay the fallback so a
// clean clone still ships with audio. Only the load resolution changes
// here — the facade and every call site below are untouched.
SampleId load_pref(const char* name) {
    char orig[256], place[256];
    std::snprintf(orig,  sizeof orig,  "assets/sfx/original/%s.wav", name);
    std::snprintf(place, sizeof place, "assets/sfx/%s.wav",          name);
    // Probe for the original WITHOUT calling audio::load first — a missing
    // file there is the normal clean-clone case, not an error to log loudly.
    if (std::ifstream(orig).good()) {
        const SampleId id = audio::load(orig);
        if (id != 0) {
            std::printf("[sfx] %-16s <- original\n", name);
            return id;
        }
        std::printf("[sfx] %-16s <- placeholder (original failed to load)\n", name);
    } else {
        std::printf("[sfx] %-16s <- placeholder\n", name);
    }
    return audio::load(place);
}

// Resolve one gun's firing sample. Per-gun originals are LOCAL-ONLY
// (gitignored assets/sfx/original/gun_<name>.wav, converted from the
// user's OWN SOUNDFX.PAK). There is NO committed per-gun placeholder:
// when a gun's original is absent we fall back to the generic
// laser_fire SampleId (already loaded), so a clean clone still hears
// *something* for every gun. `fallback` is laser_fire; `name` is the
// gun's short name ("mass_driver", ...). Logs the source per gun.
SampleId load_gun(const char* name, SampleId fallback) {
    char orig[256];
    std::snprintf(orig, sizeof orig, "assets/sfx/original/gun_%s.wav", name);
    if (std::ifstream(orig).good()) {
        const SampleId id = audio::load(orig);
        if (id != 0) {
            std::printf("[sfx] gun %-18s <- original gun_%s\n", name, name);
            return id;
        }
    }
    std::printf("[sfx] gun %-18s <- placeholder laser_fire\n", name);
    return fallback;
}

// ---- sample table -------------------------------------------------------------
struct SfxTable {
    SampleId laser_fire      = 0;
    SampleId impact_shield   = 0;
    SampleId impact_armor    = 0;
    SampleId explosion_small = 0;
    SampleId explosion_big   = 0;
    SampleId engine_hum      = 0;
    SampleId cruise_windup   = 0;
    SampleId ui_click        = 0;
    SampleId missile_fire    = 0;
    SampleId lock_seeking    = 0;
    SampleId lock_acquired   = 0;
    // Per-GunType firing samples, indexed by (int)GunType. Populated in
    // load_all from per-gun originals with a laser_fire fallback. Guns
    // that share a canonical sound just point at the same SampleId.
    SampleId gun_sounds[kGunTypeCount] = {};
};
SfxTable g_sfx;

// ---- engine hum state ----------------------------------------------------------
VoiceId g_hum_voice    = 0;
float   g_hum_gain     = 0.0f;   // current, lerped
bool    g_cruise_armed = false;  // rising-edge detector for the windup one-shot

// ---- impact rate limiter --------------------------------------------------------
// Global gate: minimum spacing between impact sounds. 1/8s spacing =
// max ~8/sec. A simple last-timestamp gate (not a token bucket) — the
// goal is "don't stack 30 thunks during beam-spam", not fairness.
// Suppression logging is itself throttled to 1/sec so the log shows
// the gate working without becoming the new spam.
uint64_t g_last_impact_ticks         = 0;
uint64_t g_last_suppress_log_ticks   = 0;
int      g_suppressed_since_last_log = 0;

// ---- NPC gunfire coalescing (np-3va) -------------------------------------------
// The impact gate above already collapses thunk-spam; NPC gunfire had NO
// gate, so a 17-ship furball fired one play_world() per shot per mount
// per frame — dozens of near-identical laser zaps a frame, all stealing
// voices. They carry no extra information past the first few, so coalesce
// near-simultaneous NPC shots to a representative ~12/sec (1/12s spacing).
// Player gunfire is exempt: it's a single 2D source already paced by the
// gun refire cooldown, and "my guns are always audible" is the point.
uint64_t g_last_npc_gun_ticks        = 0;
uint64_t g_last_gun_suppress_log     = 0;
int      g_gun_suppressed_since_log  = 0;

// Attenuation radii (meters). Gunfire is a close-quarters cue — you
// care about shots near you, not across the system. Explosions carry:
// a kill should be audible from typical engagement standoff range.
constexpr float k_gun_ref_m       = 200.0f,  k_gun_max_m       = 6000.0f;
constexpr float k_impact_ref_m    = 150.0f,  k_impact_max_m    = 5000.0f;
constexpr float k_explosion_ref_m = 400.0f,  k_explosion_max_m = 20000.0f;

} // namespace

namespace sfx {

void load_all() {
    g_sfx.laser_fire      = load_pref("laser_fire");
    g_sfx.impact_shield   = load_pref("impact_shield");
    g_sfx.impact_armor    = load_pref("impact_armor");
    g_sfx.explosion_small = load_pref("explosion_small");
    g_sfx.explosion_big   = load_pref("explosion_big");
    g_sfx.engine_hum      = load_pref("engine_hum");
    g_sfx.cruise_windup   = load_pref("cruise_windup");
    g_sfx.ui_click        = load_pref("ui_click");
    g_sfx.missile_fire    = load_pref("missile_fire");
    g_sfx.lock_seeking    = load_pref("lock_seeking");
    g_sfx.lock_acquired   = load_pref("lock_acquired");

    const int loaded = (g_sfx.laser_fire != 0) + (g_sfx.impact_shield != 0)
                     + (g_sfx.impact_armor != 0) + (g_sfx.explosion_small != 0)
                     + (g_sfx.explosion_big != 0) + (g_sfx.engine_hum != 0)
                     + (g_sfx.cruise_windup != 0) + (g_sfx.ui_click != 0)
                     + (g_sfx.missile_fire != 0) + (g_sfx.lock_seeking != 0)
                     + (g_sfx.lock_acquired != 0);
    std::printf("[sfx] %d/11 gameplay samples loaded\n", loaded);

    // Per-gun firing sounds. Each GunType prefers its own local-only
    // original (assets/sfx/original/gun_<name>.wav) and falls back to
    // the generic laser_fire when absent (e.g. Laser/Steltek here, and
    // any gun on a clean clone). The source is logged per gun.
    for (int t = 0; t < kGunTypeCount; ++t) {
        g_sfx.gun_sounds[t] = load_gun(gun::to_name((GunType)t), g_sfx.laser_fire);
    }

    // Engine hum: start silent, looping, 2D (it's OUR engine — it has
    // no world position). update_engine_hum rides the gain from here on.
    if (g_sfx.engine_hum != 0) {
        g_hum_voice = audio::play_loop(g_sfx.engine_hum, 0.0f);
        std::printf("[sfx] engine hum loop started (voice %u, gain 0)\n", g_hum_voice);
    }
}

void gun_fired(GunType type, HMM_Vec3 world_pos, bool is_player) {
    // Pick this gun's sample; out-of-range or unbound types fall back to
    // the generic laser_fire. The coalescing/gating below is unchanged
    // (np-3va) - only WHICH sample plays now depends on the gun type.
    const int ti = (int)type;
    const SampleId s = (ti >= 0 && ti < kGunTypeCount && g_sfx.gun_sounds[ti] != 0)
                     ? g_sfx.gun_sounds[ti] : g_sfx.laser_fire;
    if (s == 0) return;
    VoiceId v;
    if (is_player) {
        // 2D, modest gain: always audible, never startling. 0.35 sits
        // under impacts/explosions so sustained fire doesn't fatigue.
        v = audio::play(s, 0.35f);
    } else {
        // Coalesce a furball's near-simultaneous NPC shots (see note).
        const uint64_t now = stm_now();
        constexpr double k_npc_gun_spacing_s = 1.0 / 12.0;
        if (g_last_npc_gun_ticks != 0 &&
            stm_sec(stm_diff(now, g_last_npc_gun_ticks)) < k_npc_gun_spacing_s) {
            ++g_gun_suppressed_since_log;
            if (g_last_gun_suppress_log == 0 ||
                stm_sec(stm_diff(now, g_last_gun_suppress_log)) > 1.0) {
                std::printf("[sfx] npc gun coalesce: suppressed %d in last window\n",
                            g_gun_suppressed_since_log);
                g_last_gun_suppress_log    = now;
                g_gun_suppressed_since_log = 0;
            }
            return;
        }
        g_last_npc_gun_ticks = now;
        v = audio::play_world(s, world_pos, k_gun_ref_m, k_gun_max_m);
    }
    // Throttled visibility: one log line per second summarizing the
    // volley rate — per-shot logging in a 17-ship furball is its own
    // kind of audio spam.
    static uint64_t s_last_log   = 0;
    static int      s_shots      = 0;
    ++s_shots;
    const uint64_t now = stm_now();
    if (s_last_log == 0 || stm_sec(stm_diff(now, s_last_log)) > 1.0) {
        float gl = 0, gr = 0; audio::voice_gains(v, &gl, &gr);
        std::printf("[sfx] gun_fired x%d this window (last: %s %s, sample %u, "
                    "voice %u, gain L/R %.2f/%.2f, pos %.0f,%.0f,%.0f)\n",
                    s_shots, is_player ? "player" : "npc", gun::to_name(type),
                    s, v, gl, gr, world_pos.X, world_pos.Y, world_pos.Z);
        s_last_log = now;
        s_shots    = 0;
    }
}

void impact(HMM_Vec3 world_pos, bool shield) {
    const SampleId s = shield ? g_sfx.impact_shield : g_sfx.impact_armor;
    if (s == 0) return;

    // Rate limit (see header + namespace note).
    const uint64_t now = stm_now();
    constexpr double k_min_spacing_s = 1.0 / 8.0;
    if (g_last_impact_ticks != 0 &&
        stm_sec(stm_diff(now, g_last_impact_ticks)) < k_min_spacing_s) {
        ++g_suppressed_since_last_log;
        if (g_last_suppress_log_ticks == 0 ||
            stm_sec(stm_diff(now, g_last_suppress_log_ticks)) > 1.0) {
            std::printf("[sfx] impact rate limit: suppressed %d in last window\n",
                        g_suppressed_since_last_log);
            g_last_suppress_log_ticks   = now;
            g_suppressed_since_last_log = 0;
        }
        return;
    }
    g_last_impact_ticks = now;
    const VoiceId v = audio::play_world(s, world_pos, k_impact_ref_m, k_impact_max_m);
    float gl = 0, gr = 0; audio::voice_gains(v, &gl, &gr);
    std::printf("[sfx] impact (%s) voice %u gain L/R %.2f/%.2f pos %.0f,%.0f,%.0f\n",
                shield ? "shield" : "armor", v, gl, gr,
                world_pos.X, world_pos.Y, world_pos.Z);
}

void ship_exploded(HMM_Vec3 world_pos, bool big) {
    const SampleId s = big ? g_sfx.explosion_big : g_sfx.explosion_small;
    if (s == 0) return;
    const VoiceId v = audio::play_world(s, world_pos, k_explosion_ref_m, k_explosion_max_m);
    float gl = 0, gr = 0; audio::voice_gains(v, &gl, &gr);
    std::printf("[sfx] explosion (%s) voice %u gain L/R %.2f/%.2f pos %.0f,%.0f,%.0f\n",
                big ? "big" : "small", v, gl, gr,
                world_pos.X, world_pos.Y, world_pos.Z);
}

void update_engine_hum(float speed_frac, float cruise_level,
                       bool flight_mode, float dt) {
    if (g_hum_voice == 0) return;

    // Gain mapping: idle floor 0.05 (a ship at rest still thrums) +
    // 0.30 * speed_frac (throttle presence) + 0.25 * cruise_level
    // (cruise roar), capped at 0.6 so the bed never crowds combat SFX.
    // Non-Flight modes target plain 0 — landed ships don't hum at you
    // through the concourse.
    float target = 0.0f;
    if (flight_mode) {
        const float sf = std::fmax(0.0f, std::fmin(1.0f, speed_frac));
        target = std::fmin(0.05f + 0.30f * sf + 0.25f * cruise_level, 0.6f);
    }

    // Exponential lerp toward target (~6/s rate => ~0.17s time
    // constant): fast enough to track throttle stabs, slow enough that
    // per-frame gain steps stay sub-perceptual — no zipper noise.
    const float k = 1.0f - std::exp(-6.0f * dt);
    const float prev = g_hum_gain;
    g_hum_gain += (target - g_hum_gain) * k;
    audio::set_voice_gain(g_hum_voice, g_hum_gain);

    // Log on significant change only (0.1 steps), not per frame.
    if ((int)(prev * 10.0f) != (int)(g_hum_gain * 10.0f)) {
        std::printf("[sfx] engine hum gain %.2f (target %.2f, cruise %.2f)\n",
                    g_hum_gain, target, cruise_level);
    }

    // Cruise windup on the rising edge: trigger as the spool passes
    // 10% engaged, re-arm once it drops back under. Matches the
    // hold-TAB cruise model — release mid-spool re-arms for the next
    // attempt without replaying mid-hold.
    if (!g_cruise_armed && cruise_level > 0.1f && flight_mode) {
        g_cruise_armed = true;
        if (g_sfx.cruise_windup != 0) {
            audio::play(g_sfx.cruise_windup, 0.5f);
            std::printf("[sfx] cruise windup\n");
        }
    } else if (g_cruise_armed && cruise_level < 0.05f) {
        g_cruise_armed = false;
    }
}

void ui_click() {
    if (g_sfx.ui_click == 0) return;
    audio::play(g_sfx.ui_click, 0.6f);
}

void missile_fired() {
    // 2D, healthy gain — a missile leaving the rail should punch through
    // the gun chatter. Falls back to the cruise-windup swell if the bespoke
    // sample is missing (degrade-to-something rather than silence).
    const SampleId s = g_sfx.missile_fire != 0 ? g_sfx.missile_fire : g_sfx.cruise_windup;
    if (s == 0) return;
    audio::play(s, 0.7f);
    std::printf("[sfx] missile fired\n");
}

void out_of_ammo() {
    // Dry click — reuse the UI tick at a lower gain so an empty trigger
    // pull gives tactile "nope" feedback without a new asset.
    if (g_sfx.ui_click == 0) return;
    audio::play(g_sfx.ui_click, 0.4f);
    std::printf("[sfx] out of ammo / no lock\n");
}

void lock_seeking() {
    if (g_sfx.lock_seeking == 0) return;
    audio::play(g_sfx.lock_seeking, 0.45f);
    std::printf("[sfx] lock seeking beep\n");
}

void lock_acquired() {
    if (g_sfx.lock_acquired == 0) return;
    audio::play(g_sfx.lock_acquired, 0.6f);
    std::printf("[sfx] lock acquired tone\n");
}

void jump() {
    // Reuse the cruise-windup swell as the hyperspace sting (see header).
    // Full gain so the jump reads as a bigger event than a normal cruise
    // engage. Silent no-op if the sample failed to load.
    if (g_sfx.cruise_windup == 0) return;
    audio::play(g_sfx.cruise_windup, 0.9f);
    std::printf("[sfx] jump sting\n");
}

} // namespace sfx

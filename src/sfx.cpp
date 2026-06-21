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

// Resolve one gun-firing sample variant. Per-gun originals are LOCAL-ONLY
// (gitignored assets/sfx/original/gun_<name>[suffix].wav, regenerated from
// the user's OWN SOUNDFX.PAK by tools/remap_sfx_originals.sh). `suffix` is
// "" for the LOUD player clip, "_npc" for the QUIET twin. There is NO
// committed per-gun placeholder: a missing file falls back to `fallback`
// (loud -> generic laser_fire; quiet -> the gun's own loud clip) so a
// clean clone still hears *something* for every gun. The caller logs the
// resolved per-gun binding in one combined line.
SampleId load_gun(const char* name, const char* suffix, SampleId fallback) {
    char orig[256];
    std::snprintf(orig, sizeof orig, "assets/sfx/original/gun_%s%s.wav", name, suffix);
    if (std::ifstream(orig).good()) {
        const SampleId id = audio::load(orig);
        if (id != 0) return id;
    }
    return fallback;
}

// ---- sample table -------------------------------------------------------------
struct SfxTable {
    SampleId laser_fire        = 0;
    SampleId impact_shield     = 0;   // shield hit, PLAYER victim  (sfx_25)
    SampleId impact_shield_npc = 0;   // shield hit, NPC victim     (sfx_26)
    SampleId impact_armor      = 0;   // armor hit,  PLAYER victim  (sfx_23)
    SampleId impact_armor_npc  = 0;   // armor hit,  NPC victim     (sfx_24)
    SampleId explosion_small   = 0;
    SampleId explosion_big     = 0;
    SampleId engine_hum        = 0;
    SampleId cruise_windup     = 0;
    SampleId ui_click          = 0;
    SampleId missile_fire      = 0;
    SampleId lock_seeking      = 0;
    SampleId lock_acquired     = 0;
    SampleId jump_sting        = 0;   // sfx_41 — plays first on jump
    SampleId jump_sting2       = 0;   // sfx_42 — follows ~3.3s later
    // Per-GunType firing samples, indexed by (int)GunType. Player guns use
    // the LOUD clip (gun_sounds); NPC guns use the QUIET twin
    // (gun_sounds_npc) — straight from the user's F7 labels (sfx_00..08
    // loud / sfx_09..17 quiet). A missing twin falls back to the loud clip.
    SampleId gun_sounds[kGunTypeCount]     = {};   // loud  (player)
    SampleId gun_sounds_npc[kGunTypeCount] = {};   // quiet (NPC)
};
SfxTable g_sfx;

// Per-gun SOURCE labels (sfx_NN), kept ONLY for the boot log so the
// canonical binding is verifiable at a glance — the actual audio comes
// from the gitignored gun_<name>[_npc].wav files. Order MUST match
// enum GunType. Derived from the user's F7 ground truth
// (docs/sound_labels.json); see docs/sfx_gun_mapping.md.
// sfx_07 -> Tachyon is USER-CONFIRMED (np-4dr). sfx_04 is the particle
// cannon; sfx_07 sits exactly at Tachyon's slot in gun order and the
// user confirmed by ear that it IS the Tachyon clip. No ambiguity flag
// anymore — locked in. (No mapping change: it was already sfx_07.)
struct GunSrcLabel { const char* player; const char* npc; };
constexpr GunSrcLabel k_gun_src[kGunTypeCount] = {
    /* Laser            */ { "sfx_05", "sfx_14" },
    /* MassDriver       */ { "sfx_03", "sfx_12" },
    /* MesonBlaster     */ { "sfx_01", "sfx_10" },
    /* NeutronGun       */ { "sfx_02", "sfx_11" },
    /* ParticleCannon   */ { "sfx_04", "sfx_13" },
    /* TachyonCannon    */ { "sfx_07", "sfx_16" },   // user-confirmed (np-4dr)
    /* IonicPulseCannon */ { "sfx_00", "sfx_09" },
    /* PlasmaGun        */ { "sfx_06", "sfx_15" },
    /* SteltekGun       */ { "sfx_08", "sfx_17" },
};

// ---- engine hum state ----------------------------------------------------------
VoiceId g_hum_voice    = 0;
float   g_hum_gain     = 0.0f;   // current, lerped
bool    g_cruise_armed = false;  // rising/falling-edge detector for the afterburner loop

// ---- afterburner held-loop state (np-4dr) --------------------------------------
// The afterburner (sfx_22) is no longer a one-shot on cruise engage — the
// user wants it to LOOP for as long as TAB (cruise) is held. We own a
// dedicated looping voice, started on the cruise rising edge and stopped
// on the falling edge (or any Flight-mode exit). Like the engine hum it's
// a play_loop voice, so np-3va's cull logic never steals it (loops are
// exempt). g_afterburner_gain is lerped for a smooth spool-up/down.
VoiceId g_afterburner_voice = 0;
float   g_afterburner_gain  = 0.0f;   // current, lerped

// ---- deferred jump second clip -------------------------------------------------
// The real Privateer jump is TWO clips back-to-back: sfx_41 then sfx_42
// (user F7 label: "plays immediately followed by 42 when you press j inside
// a jump gate"). jump() plays the first and stamps a start tick here;
// update_engine_hum (which runs EVERY frame in EVERY mode — flight path +
// frame_stub) fires the second once the first has had time to play. We
// can't sleep on the main thread, and there's no audio-side scheduler, so
// this tiny frame-pumped one-shot is the cheapest honest "sequence".
// 0 = nothing pending.
uint64_t g_jump2_start_ticks = 0;
constexpr double k_jump_clip1_s = 3.30;   // ~length of sfx_41 (the first clip)

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
// PER-GUN-TYPE coalescer (was a single global timer): with one shared
// timer, mount[0]'s mass driver locked out mount[1..N] within 83 ms, so a
// 3-mount Talon firing 2x mass driver + 1x particle cannon only ever
// played the mass driver -- the particle cannon (and second mass driver)
// got swallowed every burst. One timer PER GunType lets each gun keep its
// own 12 Hz cap, so you actually hear the full loadout's audio texture
// (low thump + high zap) instead of a single repeating sample. Cross-NPC
// rate-limiting still applies per type, so a furball of identical guns
// still caps at ~12 Hz for that type.
uint64_t g_last_npc_gun_ticks_by_type[kGunTypeCount] = {};
uint64_t g_last_gun_suppress_log     = 0;
int      g_gun_suppressed_since_log  = 0;

// Attenuation radii (meters). Gunfire is a close-quarters cue — you
// care about shots near you, not across the system. Explosions carry:
// a kill should be audible from typical engagement standoff range.
// NPC gun distance attenuation: a STEPPED zone curve (user-authored), not
// SoLoud's smooth inverse-distance rolloff.  We compute distance in
// gun_fired() and call play() with the zone gain, trading 3D pan for an
// exact volume profile that matches the radar/combat realities:
//
//   0 .. 5 km    -> 100% (close brawl: full presence)
//   5 ..10 km    ->  50% (mid range: distinct but quieter)
//  10 ..15 km    ->  25% (long range: faint pops at the edge of radar)
//   >15 km       -> dropped (past targetable range; nothing to hear)
//
// The 1/12 Hz coalescer below still rate-limits a furball so the audio
// doesn't compress into a wall of pew-pew.
constexpr float k_gun_zone_full_m  =  5000.0f;
constexpr float k_gun_zone_med_m   = 10000.0f;
constexpr float k_gun_zone_quiet_m = 15000.0f;
constexpr float k_gun_zone_gain_full  = 1.00f;
constexpr float k_gun_zone_gain_med   = 0.50f;
constexpr float k_gun_zone_gain_quiet = 0.25f;
constexpr float k_impact_ref_m    = 150.0f,  k_impact_max_m    = 5000.0f;
constexpr float k_explosion_ref_m = 400.0f,  k_explosion_max_m = 20000.0f;

} // namespace

namespace sfx {

void load_all() {
    g_sfx.laser_fire        = load_pref("laser_fire");
    g_sfx.impact_shield     = load_pref("impact_shield");
    g_sfx.impact_shield_npc = load_pref("impact_shield_npc");
    g_sfx.impact_armor      = load_pref("impact_armor");
    g_sfx.impact_armor_npc  = load_pref("impact_armor_npc");
    g_sfx.explosion_small   = load_pref("explosion_small");
    g_sfx.explosion_big     = load_pref("explosion_big");
    g_sfx.engine_hum        = load_pref("engine_hum");
    g_sfx.cruise_windup     = load_pref("cruise_windup");
    g_sfx.ui_click          = load_pref("ui_click");
    g_sfx.missile_fire      = load_pref("missile_fire");
    g_sfx.lock_seeking      = load_pref("lock_seeking");
    g_sfx.lock_acquired     = load_pref("lock_acquired");
    g_sfx.jump_sting        = load_pref("jump");    // sfx_41
    g_sfx.jump_sting2       = load_pref("jump2");   // sfx_42

    // Boot-log the canonical EVENT bindings (source sfx_NN from the user's
    // F7 labels) so one glance at the log verifies the remap. engine_hum +
    // lock_seeking stay procedural — SOUNDFX.PAK has no canonical idle-
    // engine loop or clean seeking-beep clip (documented in the header).
    std::printf("[sfx] canonical event bindings (docs/sound_labels.json):\n");
    std::printf("[sfx]   impact_armor  player<-sfx_23 npc<-sfx_24\n");
    std::printf("[sfx]   impact_shield player<-sfx_25 npc<-sfx_26\n");
    std::printf("[sfx]   explosion_big<-sfx_27 explosion_small<-sfx_28\n");
    std::printf("[sfx]   missile_fire<-sfx_18 afterburner<-sfx_22(HELD LOOP) jump<-sfx_41(+sfx_42)\n");
    std::printf("[sfx]   ui_click<-sfx_34 lock_acquired<-sfx_31\n");
    std::printf("[sfx]   engine_hum<-procedural(de-buzzed idle loop) lock_seeking<-procedural (no canon original)\n");

    // Per-gun firing sounds: LOUD(player) + QUIET(NPC) twin per GunType.
    // Each variant prefers its own local-only original
    // (assets/sfx/original/gun_<name>[_npc].wav); a missing loud clip
    // falls back to generic laser_fire, a missing twin to the loud clip.
    // The source sfx_NN is logged from k_gun_src (verifiable boot line).
    for (int t = 0; t < kGunTypeCount; ++t) {
        const char* nm = gun::to_name((GunType)t);
        const SampleId loud = load_gun(nm, "", g_sfx.laser_fire);
        g_sfx.gun_sounds[t]     = loud;
        g_sfx.gun_sounds_npc[t] = load_gun(nm, "_npc", loud);
        std::printf("[sfx] gun %-18s player<-%s npc<-%s\n",
                    nm, k_gun_src[t].player, k_gun_src[t].npc);
    }

    // Engine hum: start silent, looping, 2D (it's OUR engine — it has
    // no world position). update_engine_hum rides the gain from here on.
    if (g_sfx.engine_hum != 0) {
        g_hum_voice = audio::play_loop(g_sfx.engine_hum, 0.0f);
        std::printf("[sfx] engine hum loop started (voice %u, gain 0)\n", g_hum_voice);
    }
}

void gun_fired(GunType type, HMM_Vec3 world_pos, bool is_player) {
    // Pick this gun's sample: player guns get the LOUD clip, NPC guns the
    // QUIET twin (user's F7 labels). Out-of-range or unbound types fall
    // back to the generic laser_fire. The coalescing/gating below is
    // unchanged (np-3va) — only WHICH sample plays depends on gun + who.
    const int ti = (int)type;
    const bool in_range = (ti >= 0 && ti < kGunTypeCount);
    const SampleId* table = is_player ? g_sfx.gun_sounds : g_sfx.gun_sounds_npc;
    const SampleId s = (in_range && table[ti] != 0) ? table[ti] : g_sfx.laser_fire;
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
        // Per-type slot: a particle cannon burst doesn't lock out a
        // mass driver burst (or vice versa), so a multi-gun NPC plays
        // all its weapon types audibly.
        const int slot = in_range ? ti : 0;
        uint64_t& last_ticks = g_last_npc_gun_ticks_by_type[slot];
        if (last_ticks != 0 &&
            stm_sec(stm_diff(now, last_ticks)) < k_npc_gun_spacing_s) {
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
        last_ticks = now;
        // Stepped distance zone -> 2D play with computed gain. We lose 3D
        // pan vs play_world, but the user-authored curve is exact (see the
        // constants above) and pan was a minor effect at combat distances.
        const float dist = HMM_LenV3(
            HMM_SubV3(world_pos, audio::listener_position()));
        float zone_gain;
        if      (dist < k_gun_zone_full_m)  zone_gain = k_gun_zone_gain_full;
        else if (dist < k_gun_zone_med_m)   zone_gain = k_gun_zone_gain_med;
        else if (dist < k_gun_zone_quiet_m) zone_gain = k_gun_zone_gain_quiet;
        else                                return;   // past 15 km -- silent
        v = audio::play(s, zone_gain);
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

void impact(HMM_Vec3 world_pos, bool shield, bool victim_is_player) {
    // Four variants from the user's F7 labels: shield-vs-armor x player-
    // vs-NPC victim. The damage pass knows which ship took the hit.
    const SampleId s = shield
        ? (victim_is_player ? g_sfx.impact_shield : g_sfx.impact_shield_npc)
        : (victim_is_player ? g_sfx.impact_armor  : g_sfx.impact_armor_npc);
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
    std::printf("[sfx] impact (%s %s) voice %u gain L/R %.2f/%.2f pos %.0f,%.0f,%.0f\n",
                shield ? "shield" : "armor", victim_is_player ? "player" : "npc",
                v, gl, gr, world_pos.X, world_pos.Y, world_pos.Z);
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
    // Pump the deferred jump second clip FIRST (independent of the hum
    // voice existing): jump() armed g_jump2_start_ticks; once sfx_41 has
    // had time to play, fire sfx_42 to complete the two-clip jump sound.
    if (g_jump2_start_ticks != 0 &&
        stm_sec(stm_diff(stm_now(), g_jump2_start_ticks)) >= k_jump_clip1_s) {
        g_jump2_start_ticks = 0;
        if (g_sfx.jump_sting2 != 0) {
            audio::play(g_sfx.jump_sting2, 0.9f);
            std::printf("[sfx] jump second clip (sfx_42)\n");
        }
    }

    // Exponential lerp factor (~6/s rate => ~0.17s time constant): fast
    // enough to track throttle stabs, slow enough that per-frame gain
    // steps stay sub-perceptual — no zipper noise. Shared by the hum bed
    // and the afterburner spool below.
    const float k = 1.0f - std::exp(-6.0f * dt);

    // --- idle engine-hum bed -------------------------------------------------
    // Gain mapping (np-4dr): idle floor 0.04 (a ship at rest still thrums,
    // quietly) + 0.25 * speed_frac (throttle presence), capped at 0.4 so
    // the bed stays a BED. The cruise-roar contribution was REMOVED from
    // the hum — the afterburner now has its own layered loop (below), so
    // folding cruise into the hum too would double-roar. Non-Flight modes
    // target plain 0 — landed ships don't hum at you through the concourse.
    if (g_hum_voice != 0) {
        float target = 0.0f;
        if (flight_mode) {
            const float sf = std::fmax(0.0f, std::fmin(1.0f, speed_frac));
            target = std::fmin(0.04f + 0.25f * sf, 0.4f);
        }
        const float prev = g_hum_gain;
        g_hum_gain += (target - g_hum_gain) * k;
        audio::set_voice_gain(g_hum_voice, g_hum_gain);
        // Log on significant change only (0.1 steps), not per frame.
        if ((int)(prev * 10.0f) != (int)(g_hum_gain * 10.0f)) {
            std::printf("[sfx] engine hum gain %.2f (target %.2f)\n",
                        g_hum_gain, target);
        }
    }

    // --- afterburner held loop (np-4dr) --------------------------------------
    // Rising edge of cruise engage (spool past 10%, in Flight): fire a
    // brief windup one-shot AND start a SUSTAINED looping voice of the
    // afterburner clip (sfx_22). Falling edge (spool back under 5%, OR any
    // Flight-mode exit): STOP the loop. Hysteresis (0.10 up / 0.05 down)
    // stops flicker at the engage threshold. The loop is a play_loop voice
    // => protected from np-3va culling exactly like the hum, so a furball
    // can't steal the afterburner out from under a held TAB.
    const bool want_loop = flight_mode && cruise_level > 0.10f;
    if (!g_cruise_armed && want_loop) {
        g_cruise_armed = true;
        if (g_sfx.cruise_windup != 0) {
            audio::play(g_sfx.cruise_windup, 0.5f);   // short spool-up stab
            // Held loop plays the ORIGINAL afterburner clip (sfx_22 ->
            // cruise_windup) on a sustained play_loop voice. If its
            // head/tail amplitudes differ it may tick once per loop period;
            // acceptable — it's the original sound (np-3dp.17: reverted
            // from the procedural afterburner_loop sample).
            g_afterburner_voice = audio::play_loop(g_sfx.cruise_windup, 0.0f);
            g_afterburner_gain  = 0.0f;
            std::printf("[sfx] afterburner ENGAGE -> windup + loop start (voice %u)\n",
                        g_afterburner_voice);
        }
    } else if (g_cruise_armed && (!flight_mode || cruise_level < 0.05f)) {
        g_cruise_armed = false;
        if (g_afterburner_voice != 0) {
            audio::stop(g_afterburner_voice);
            std::printf("[sfx] afterburner RELEASE -> loop stop (voice %u)\n",
                        g_afterburner_voice);
            g_afterburner_voice = 0;
            g_afterburner_gain  = 0.0f;
        }
    }

    // Spool the afterburner loop gain toward a sustained roar while held.
    // Track cruise_level so a partial spool is proportionally quieter and
    // the windown fades the roar out before the falling edge stops it.
    if (g_afterburner_voice != 0) {
        const float ab_target =
            0.55f * std::fmax(0.0f, std::fmin(1.0f, cruise_level));
        const float prev = g_afterburner_gain;
        g_afterburner_gain += (ab_target - g_afterburner_gain) * k;
        audio::set_voice_gain(g_afterburner_voice, g_afterburner_gain);
        // Log on 0.1 steps only (mirror the hum) so we can verify the
        // held loop actually spools up rather than dropping out.
        if ((int)(prev * 10.0f) != (int)(g_afterburner_gain * 10.0f)) {
            std::printf("[sfx] afterburner gain %.2f (target %.2f, voice %u)\n",
                        g_afterburner_gain, ab_target, g_afterburner_voice);
        }
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
    // Canonical jump sound is sfx_41 then sfx_42 (user F7 label). Play the
    // first now at full gain (a bigger event than a cruise engage) and arm
    // the deferred second clip; update_engine_hum fires sfx_42 once the
    // first has played. Silent no-op if the first sample failed to load.
    if (g_sfx.jump_sting == 0) return;
    audio::play(g_sfx.jump_sting, 0.9f);
    g_jump2_start_ticks = stm_now();   // pump fires sfx_42 ~k_jump_clip1_s later
    std::printf("[sfx] jump sting (sfx_41; sfx_42 to follow)\n");
}

} // namespace sfx

// -----------------------------------------------------------------------------
// cinematic.cpp — in-engine cutscene director (Phase 1). Playback + overlay.
// Design overview: cinematic.h. DSL: docs/cinematic_format.md. Data model +
// parser: cinematic_parse.{h,cpp}. Both tick + draw no-op when nothing plays.
// -----------------------------------------------------------------------------

#include "cinematic.h"
#include "cinematic_parse.h"  // Cue / Cinematic data model + non-throwing parser

#include "audio.h"          // audio::play_file / play_file_world
#include "camera.h"         // Camera (director-owned render pose)
#include "json.h"           // json::parse_file + Value
#include "look_rotation.h"  // orientation from (forward, up)
#include "material.h"       // TextureSlot + load_texture_png (portrait PNGs)
#include "player.h"         // PlayerState (end-cue plot actions)
#include "plot.h"           // plot::run_actions
#include "ship.h"           // Ship + sprite (actor_path drives the sprite pose)
#include "ship_sprite.h"    // ShipSpriteObject fields (pose the director drives)
#include "ship_registry.h"  // ShipRegistry
#include "faction.h"        // faction::from_name (spawn cue)

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"    // simgui_imtextureid

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace cinematic { namespace {

// ---- pinned portrait dimensions (docs/cinematic_format.md) -----------------
// Phase 3's generator produces PNGs at exactly this size; the on-screen
// panel is a fraction of framebuffer HEIGHT so it scales with resolution.
constexpr float k_portrait_src_w   = 512.0f;
constexpr float k_portrait_src_h   = 640.0f;
constexpr float k_portrait_aspect  = k_portrait_src_w / k_portrait_src_h;  // 0.8
constexpr float k_panel_h_frac     = 0.34f;   // panel height / screen height
constexpr float k_panel_inset_frac = 0.04f;   // gap from the screen edge
constexpr float k_slide_s          = 0.35f;   // slide in/out duration
constexpr float k_letterbox_frac   = 0.12f;   // bar height / screen height (~2.35:1)
constexpr float k_letterbox_anim_s = 0.5f;    // bars ease in/out over this

const std::string k_asset_root = "assets/cinematics/";

// The Cue / Cinematic / Cmd / CamKey data model + the JSON->Cue translation
// live in cinematic_parse.{h,cpp} (engine-free, headlessly unit-tested).

// ---- module state ----------------------------------------------------------
Cinematic g_cin;
bool      g_active = false;
float     g_time   = 0.0f;   // seconds since play()
float     g_end_at = 0.0f;   // when the last cue's effect elapses
bool      g_ending = false;  // an `end` cue fired / natural end reached

Camera    g_cam;             // director-owned render pose
int       g_cam_cue = -1;    // index of the camera_path currently driving g_cam
int       g_prev_cam_cue = -1; // last frame's g_cam_cue (detect hard cuts)
HMM_Vec3  g_cam_vel{};       // world-space velocity of g_cam (drives warp streaks)

VoiceId   g_music_voice = 0;

std::string g_last_error;    // "" once a load succeeds; set on any failure
std::function<void(const std::string&)> g_event_tap;   // Phase-2 beat sink

std::unordered_map<std::string, uint32_t>    g_actors;    // actor name -> ship id
std::unordered_map<std::string, TextureSlot> g_portraits; // path -> texture (cached)

// Live registry pointer, cached by every tick()/seek()/reload() so the
// single teardown path (end_cinematic) can DESPAWN the actors the director
// spawned even on the stop()/skip() routes, which don't carry a registry.
// The registry is a program-lifetime global in main, so this never dangles.
ShipRegistry* g_reg = nullptr;
// Host despawn recipe: frees the sprite slot AND the registry ship (the
// registry alone leaves an orphaned sprite in g.placed_ship_sprites that keeps
// rendering -- the "ghost" bug). main wires this to encounter_despawn.
encounters::DespawnFn g_despawn;
// Host outcome applier (Studio Phase A2): teleport-to-nav + spawn groups.
// Fired from end_cinematic for natural/skipped ends only (never "stopped").
std::function<void(const Outcome&)> g_outcome_hook;

// Live follow-cam offset override (Studio panel). When set, replaces the
// cam_keys[0].pos offset for the currently-active follow camera_path.
bool      g_offset_override_active = false;
HMM_Vec3  g_offset_override{};

// Despawn one actor by id through the host recipe when available (frees the
// sprite too), else fall back to the registry-only path.
void despawn_one(uint32_t id) {
    if (g_despawn)     g_despawn(id);
    else if (g_reg)    g_reg->despawn(g_reg->find_handle_by_id(id));
}

// ---- Phase-2 event beat helpers --------------------------------------------
// Fire a compact, machine-parseable beat at the registered tap (wired to
// dev_remote::push_event in main.cpp). No-op until a sink is registered.
void emit(const std::string& text) { if (g_event_tap) g_event_tap(text); }

const char* cmd_name(Cmd c) {
    switch (c) {
        case Cmd::FadeIn:     return "fade_in";
        case Cmd::FadeOut:    return "fade_out";
        case Cmd::Music:      return "music";
        case Cmd::Sfx:        return "sfx";
        case Cmd::CameraPath: return "camera_path";
        case Cmd::Spawn:      return "spawn";
        case Cmd::ActorPath:  return "actor_path";
        case Cmd::Line:       return "line";
        case Cmd::Subtitle:   return "subtitle";
        case Cmd::End:        return "end";
    }
    return "?";
}

// Generic per-cue beat: "cue cmd=<name> t=<t>". Emitted the frame a cue
// fires so the agent gets a complete beat stream to assert timing against.
void emit_cue(const Cue& c) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "cue cmd=%s t=%.2f", cmd_name(c.cmd), c.t);
    emit(buf);
}

// ---- helpers ---------------------------------------------------------------
// Catmull-Rom through p1..p2 with p0/p3 as tangent neighbours.
HMM_Vec3 catmull(HMM_Vec3 p0, HMM_Vec3 p1, HMM_Vec3 p2, HMM_Vec3 p3, float t) {
    const float t2 = t * t, t3 = t2 * t;
    HMM_Vec3 r;
    r.X = 0.5f * ((2*p1.X) + (-p0.X + p2.X)*t +
                  (2*p0.X - 5*p1.X + 4*p2.X - p3.X)*t2 +
                  (-p0.X + 3*p1.X - 3*p2.X + p3.X)*t3);
    r.Y = 0.5f * ((2*p1.Y) + (-p0.Y + p2.Y)*t +
                  (2*p0.Y - 5*p1.Y + 4*p2.Y - p3.Y)*t2 +
                  (-p0.Y + 3*p1.Y - 3*p2.Y + p3.Y)*t3);
    r.Z = 0.5f * ((2*p1.Z) + (-p0.Z + p2.Z)*t +
                  (2*p0.Z - 5*p1.Z + 4*p2.Z - p3.Z)*t2 +
                  (-p0.Z + 3*p1.Z - 3*p2.Z + p3.Z)*t3);
    return r;
}

// Evaluate a position spline (vector of points) at global u in [0,1].
// `smooth` = Catmull-Rom, else piecewise-linear. Endpoints are duplicated
// for the tangents so the path starts/ends exactly on the first/last key.
HMM_Vec3 eval_spline(const std::vector<HMM_Vec3>& k, float u, bool smooth) {
    const int n = (int)k.size();
    if (n == 0) return HMM_V3(0, 0, 0);
    if (n == 1) return k[0];
    u = std::clamp(u, 0.0f, 1.0f);
    const float fseg = u * (float)(n - 1);
    int seg = (int)std::floor(fseg);
    if (seg > n - 2) seg = n - 2;
    const float lt = fseg - (float)seg;
    const HMM_Vec3 p1 = k[seg];
    const HMM_Vec3 p2 = k[seg + 1];
    if (!smooth) return HMM_LerpV3(p1, lt, p2);
    const HMM_Vec3 p0 = (seg > 0)     ? k[seg - 1] : p1;
    const HMM_Vec3 p3 = (seg + 2 < n) ? k[seg + 2] : p2;
    return catmull(p0, p1, p2, p3, lt);
}

// Lazy-load + cache a portrait texture by path. Missing/undecodable PNG =>
// stored as an invalid slot (so we log/try exactly once, never per-frame)
// and callers draw the neutral frame. Never crashes.
const TextureSlot& portrait_texture(const std::string& rel_path) {
    auto it = g_portraits.find(rel_path);
    if (it != g_portraits.end()) return it->second;
    TextureSlot slot;
    const std::string full = k_asset_root + rel_path;
    if (!load_texture_png(full, slot)) {
        std::printf("[cinematic] portrait missing/undecodable: %s (neutral frame)\n",
                    full.c_str());
        slot.valid = false;   // negative-cache: don't retry every frame
    }
    auto [ins, ok] = g_portraits.emplace(rel_path, slot);
    (void)ok;
    return ins->second;
}

// End time = latest cue start + its dur (End cues end immediately at t).
// Shared by play_file() and reload() — one source of truth for duration.
void recompute_end() {
    g_end_at = 0.0f;
    for (const Cue& c : g_cin.cues) {
        const float e = (c.cmd == Cmd::End) ? c.t : c.t + c.dur;
        g_end_at = std::max(g_end_at, e);
    }
}

// Despawn every actor the director spawned, returning the world to its
// pre-cinematic population. Idempotent (double-despawn on a stale handle is
// a harmless no-op per ShipRegistry) and clears the tracking map so a replay
// starts from a clean slate. No-op when we never cached a registry (nothing
// was ever spawned in that case, since spawns only happen inside tick/seek).
void despawn_actors() {
    for (const auto& [name, id] : g_actors) {
        (void)name;
        despawn_one(id);
    }
    g_actors.clear();
}

// Single teardown path so every way a cinematic can end (natural, skip,
// external stop) emits exactly one "ended reason=..." beat + kills music +
// despawns the actors it spawned (so repeated plays don't pile up ships).
void end_cinematic(const char* reason) {
    if (!g_active) return;
    if (g_music_voice) { audio::stop(g_music_voice); g_music_voice = 0; }
    despawn_actors();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "ended reason=%s t=%.2f", reason, g_time);
    emit(buf);
    std::printf("[cinematic] END '%s' (%s) at %.2fs\n",
                g_cin.id.c_str(), reason, g_time);
    g_active = false;
    g_ending = false;
    // Outcome (Studio Phase A2): apply the authored post-cinematic world
    // state on a natural end OR a skip (a skip must not strand the story) —
    // but NEVER on an external stop, which is an abort, not a resolution.
    // Fired last, after g_active is cleared and our actors are despawned, so
    // the hook's teleport + spawns land in a clean, non-reentrant world.
    if (g_cin.outcome.present && g_outcome_hook &&
        (std::strcmp(reason, "natural") == 0 ||
         std::strcmp(reason, "skipped") == 0)) {
        g_outcome_hook(g_cin.outcome);
    }
}

} // anonymous namespace

// ---- public API ------------------------------------------------------------
void load(const std::string& path) {
    g_cin = Cinematic{};
    g_last_error.clear();
    // Distinguish "file missing" from "parse error" for a useful HTTP reply
    // (the raw json::parse_file only logs a line:col to stderr).
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
    } else {
        g_last_error = "file not found: " + path;
        std::printf("[cinematic] load failed (missing): %s\n", path.c_str());
        return;   // NON-fatal — g_cin stays invalid, play() will refuse
    }
    const json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        g_last_error = "parse error in " + path + " (see game log for line:col)";
        std::printf("[cinematic] load failed (unparseable): %s\n", path.c_str());
        return;   // NON-fatal — g_cin stays invalid, play() will refuse
    }
    // Default id = filename stem. Accept BOTH separators so a Windows path
    // (assets\cinematics\intro.json) yields "intro" (#8).
    const size_t slash = path.find_last_of("/\\");
    const size_t dot   = path.find_last_of('.');
    const std::string stem =
        path.substr(slash == std::string::npos ? 0 : slash + 1,
                    dot == std::string::npos ? std::string::npos
                                             : dot - (slash + 1));
    // All field reads go through the non-throwing parser (Phase 6.2).
    std::string perr;
    if (!parse_document(root, stem, g_cin, perr)) {
        g_last_error = perr + " in " + path;
        std::printf("[cinematic] load failed (%s): %s\n", perr.c_str(), path.c_str());
        g_cin = Cinematic{};
        return;
    }
    std::printf("[cinematic] loaded '%s': %zu cue(s), letterbox=%d skippable=%d\n",
                g_cin.id.c_str(), g_cin.cues.size(), g_cin.letterbox, g_cin.skippable);
}

bool play_file(const std::string& path) {
    // Play-while-playing policy (#6): REJECT, don't replace (a silent replace
    // orphans the prior cinematic's actors + fights the camera handoff). The
    // caller surfaces g_last_error in the /cinematic/play reply.
    if (g_active) {
        g_last_error = "a cinematic is already playing ('" + g_cin.id +
                       "') - stop it first";
        std::printf("[cinematic] play refused: '%s' already active\n", g_cin.id.c_str());
        return false;
    }
    load(path);
    if (!g_cin.valid) return false;   // g_last_error set by load()

    g_active = true;
    g_ending = false;
    g_time   = 0.0f;
    g_cam_cue = -1;
    g_prev_cam_cue = -1;
    g_cam_vel = HMM_V3(0, 0, 0);
    g_actors.clear();
    g_music_voice = 0;

    recompute_end();

    // Seed the camera from the first camera_path's first key so frame 0
    // isn't a jarring default pose (before any camera cue fires).
    g_cam = Camera{};
    for (const Cue& c : g_cin.cues)
        if (c.cmd == Cmd::CameraPath && !c.cam_keys.empty()) {
            g_cam.position = c.cam_keys.front().pos;
            break;
        }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "started id=%s dur=%.2f", g_cin.id.c_str(), g_end_at);
    emit(buf);
    std::printf("[cinematic] PLAY '%s' (%.1fs)\n", g_cin.id.c_str(), g_end_at);
    return true;
}

bool play(const std::string& id) { return play_file(k_asset_root + id + ".json"); }

bool peek_location(const std::string& id, std::string& out_system,
                   std::string& out_nav, std::string& err) {
    out_system.clear();
    out_nav.clear();
    err.clear();
    // Parse into a THROWAWAY document — peeking must never disturb the
    // active slot (a peek mid-play would otherwise clobber g_cin).
    const std::string path = k_asset_root + id + ".json";
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
    } else {
        err = "file not found: " + path;
        return false;
    }
    const json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        err = "parse error in " + path + " (see game log for line:col)";
        return false;
    }
    Cinematic tmp;
    std::string perr;
    if (!parse_document(root, id, tmp, perr)) {
        err = perr + " in " + path;
        return false;
    }
    out_system = tmp.location.system;
    out_nav    = tmp.location.nav;
    return true;
}

void stop() { end_cinematic("stopped"); }

bool active() { return g_active; }
const char* current_id() { return g_active ? g_cin.id.c_str() : ""; }
float time() { return g_active ? g_time : 0.0f; }
float duration() { return g_end_at; }
const char* last_error() { return g_last_error.c_str(); }
const Camera& camera() { return g_cam; }

HMM_Vec3 camera_velocity() { return g_active ? g_cam_vel : HMM_V3(0, 0, 0); }

void set_event_tap(std::function<void(const std::string&)> fn) {
    g_event_tap = std::move(fn);
}

void set_despawn_hook(encounters::DespawnFn fn) {
    g_despawn = std::move(fn);
}

void set_outcome_hook(std::function<void(const Outcome&)> fn) {
    g_outcome_hook = std::move(fn);
}

void set_follow_offset_override(HMM_Vec3 offset) {
    g_offset_override = offset;
    g_offset_override_active = true;
}
void clear_follow_offset_override() {
    g_offset_override_active = false;
}

void skip() {
    if (!g_active) return;
    if (!g_cin.skippable) {
        std::printf("[cinematic] skip ignored — '%s' is not skippable\n", g_cin.id.c_str());
        return;
    }
    std::printf("[cinematic] SKIP '%s' at %.2fs\n", g_cin.id.c_str(), g_time);
    end_cinematic("skipped");
}

// ---- seek / reload (Phase-2 iterate primitives) ----------------------------
bool seek(float t, ShipRegistry& ships, PlayerState& player,
          const encounters::SpawnFn& spawn, std::string& err) {
    (void)player;   // reserved: `end` actions are intentionally NOT run on a scrub
    g_reg = &ships;   // cache for end_cinematic's despawn on any end path
    if (!g_active) { err = "no cinematic playing"; return false; }
    const float target = std::clamp(t, 0.0f, g_end_at);

    // Idempotent reconcile: despawn any live actor whose spawn cue is now in
    // the FUTURE (seeking backward past its spawn) so the world matches the
    // intended set for `target`. The spawn loop below re-establishes any
    // actor due by `target` that we don't already have — together these make
    // seek create the correct set exactly, never a duplicate/stacked pile.
    {
        std::unordered_map<std::string, float> spawn_t;
        for (const Cue& c : g_cin.cues)
            if (c.cmd == Cmd::Spawn) spawn_t[c.actor] = c.t;
        for (auto it = g_actors.begin(); it != g_actors.end();) {
            auto st = spawn_t.find(it->first);
            if (st == spawn_t.end() || st->second > target) {
                despawn_one(it->second);
                it = g_actors.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Rewind latches + music, then silently fast-forward to `target`.
    g_cam_cue = -1;
    g_ending  = false;
    if (g_music_voice) { audio::stop(g_music_voice); g_music_voice = 0; }
    for (Cue& c : g_cin.cues) c.fired = false;

    int last_music = -1;
    for (int i = 0; i < (int)g_cin.cues.size(); ++i) {
        Cue& c = g_cin.cues[i];
        if (c.t > target) break;   // cues are time-sorted
        c.fired = true;
        switch (c.cmd) {
            case Cmd::CameraPath:
                g_cam_cue = i;   // last camera_path <= target wins
                break;
            case Cmd::Spawn:
                // Re-establish actors we don't already have live (a scrub
                // must not duplicate ships already spawned this play).
                if (g_actors.find(c.actor) == g_actors.end()) {
                    encounters::SpawnRequest req;
                    req.class_name    = c.klass;
                    req.faction       = faction::from_name(c.faction);
                    req.position      = c.pos;
                    req.patrol_anchor = c.pos;
                    const uint32_t id = spawn ? spawn(req) : 0;
                    if (id) {
                        g_actors[c.actor] = id;
                        if (Ship* s = ships.get(ships.find_handle_by_id(id))) {
                            s->ai.enabled = false;
                            if (s->sprite) { s->sprite->forward_speed = 0.0f;
                                             s->sprite->angular_velocity = HMM_V3(0,0,0); }
                        }
                    }
                }
                break;
            case Cmd::Music:
                last_music = i;   // only the most recent bed should resume
                break;
            default: break;       // sfx / line / subtitle / fade / end: skipped
        }
    }
    if (last_music >= 0)
        g_music_voice = audio::play_file(k_asset_root + g_cin.cues[last_music].file,
                                         0.8f, /*loop=*/true);
    g_time = target;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "seeked t=%.2f", target);
    emit(buf);
    std::printf("[cinematic] SEEK '%s' -> %.2fs\n", g_cin.id.c_str(), target);
    return true;
}

bool reload(const std::string& id, ShipRegistry& ships, PlayerState& player,
            const encounters::SpawnFn& spawn, std::string& err) {
    const std::string target_id = id.empty() ? g_cin.id : id;
    if (target_id.empty()) { err = "no cinematic to reload"; return false; }
    const bool  was_active = g_active;
    const float saved_t    = g_time;

    load(k_asset_root + target_id + ".json");   // re-parse from disk
    if (!g_cin.valid) { err = g_last_error; return false; }
    recompute_end();

    if (was_active) {
        // Keep playing: re-establish the world at the saved position. Note
        // g_actors survives the reload, so seek() won't re-spawn live actors.
        g_active = true;
        std::string serr;
        seek(std::min(saved_t, g_end_at), ships, player, spawn, serr);
    }
    emit(std::string("reloaded id=") + g_cin.id);
    std::printf("[cinematic] RELOAD '%s' (%s)\n", g_cin.id.c_str(),
                was_active ? "live" : "slot");
    return true;
}

void tick(float dt, ShipRegistry& ships, PlayerState& player,
          const encounters::SpawnFn& spawn) {
    g_reg = &ships;   // cache for end_cinematic's despawn on any end path
    if (!g_active) return;
    g_time += dt;

    // ---- fire due one-shot cues + latch the active camera cue -------------
    for (int i = 0; i < (int)g_cin.cues.size(); ++i) {
        Cue& c = g_cin.cues[i];
        if (g_time < c.t) continue;

        if (c.cmd == Cmd::CameraPath) {
            // Latch as the driving camera cue the frame it starts (they're
            // time-sorted, so a later camera_path supersedes an earlier one).
            if (!c.fired) { c.fired = true; g_cam_cue = i; emit_cue(c); }
            continue;
        }
        if (c.fired) continue;   // remaining cmds are one-shot
        c.fired = true;
        emit_cue(c);   // generic beat for every one-shot cue

        char ebuf[512];
        switch (c.cmd) {
            case Cmd::Music:
                if (g_music_voice) audio::stop(g_music_voice);
                g_music_voice = audio::play_file(k_asset_root + c.file, 0.8f, /*loop=*/true);
                std::snprintf(ebuf, sizeof(ebuf), "music file=%s t=%.2f", c.file.c_str(), c.t);
                emit(ebuf);
                break;
            case Cmd::Sfx:
                if (c.has_pos) audio::play_file_world(k_asset_root + c.file, c.pos, 1000.0f, 40000.0f);
                else           audio::play_file(k_asset_root + c.file, 1.0f);
                std::snprintf(ebuf, sizeof(ebuf), "sfx file=%s t=%.2f", c.file.c_str(), c.t);
                emit(ebuf);
                break;
            case Cmd::Spawn: {
                encounters::SpawnRequest req;
                // "$player" = whatever the player is ACTUALLY flying: the
                // hero actor should show the real ship (centurion, orion...),
                // not a class hardcoded at author time.
                req.class_name = (c.klass == "$player")
                                     ? player.ship_class_name : c.klass;
                req.faction    = faction::from_name(c.faction);
                req.position   = c.pos;
                req.patrol_anchor = c.pos;
                const uint32_t id = spawn ? spawn(req) : 0;
                if (id) {
                    g_actors[c.actor] = id;
                    // Freeze it: the director drives the pose, not the AI.
                    if (Ship* s = ships.get(ships.find_handle_by_id(id))) {
                        s->ai.enabled = false;
                        if (s->sprite) { s->sprite->forward_speed = 0.0f;
                                         s->sprite->angular_velocity = HMM_V3(0,0,0); }
                    }
                } else {
                    std::printf("[cinematic] spawn '%s' (class %s) failed — actor skipped\n",
                                c.actor.c_str(), c.klass.c_str());
                }
                break;
            }
            case Cmd::Line:
                if (!c.voice_file.empty())
                    audio::play_file(k_asset_root + c.voice_file, 1.0f);
                std::snprintf(ebuf, sizeof(ebuf),
                    "line speaker=%s portrait=%s side=%s t=%.2f",
                    c.speaker.c_str(), c.portrait.c_str(),
                    c.side_left ? "left" : "right", c.t);
                emit(ebuf);
                break;
            case Cmd::End:
                plot::run_actions(player, c.actions);
                g_ending = true;
                break;
            default: break;   // FadeIn/FadeOut/Subtitle are drawn, not fired
        }
    }

    // ---- drive the camera --------------------------------------------------
    if (g_cam_cue >= 0) {
        const Cue& c = g_cin.cues[g_cam_cue];
        const float u = (c.dur > 0.0f) ? std::clamp((g_time - c.t) / c.dur, 0.0f, 1.0f) : 1.0f;

        const HMM_Vec3 prev = g_cam.position;

        if (!c.follow_actor.empty() && !c.cam_keys.empty()) {
            // FOLLOW MODE: camera sits at a fixed offset from the tracked
            // ship(s), matching velocity and heading every frame.
            // follow_actor may be a comma-separated list (e.g. "talon1,talon2")
            // — the camera targets the CENTROID of all listed ships, framing
            // an entire wing / formation.
            HMM_Vec3 target_pos = HMM_V3(0, 0, 0);
            int found = 0;
            std::string follow_str = c.follow_actor;
            size_t pos = 0;
            while (pos < follow_str.size()) {
                size_t comma = follow_str.find(',', pos);
                std::string name = (comma == std::string::npos)
                    ? follow_str.substr(pos)
                    : follow_str.substr(pos, comma - pos);
                auto it = g_actors.find(name);
                if (it != g_actors.end())
                    if (Ship* s = ships.get(ships.find_handle_by_id(it->second))) {
                        target_pos = HMM_AddV3(target_pos, s->position);
                        ++found;
                    }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (found > 0)
                target_pos = HMM_DivV3F(target_pos, (float)found);
            else
                target_pos = g_cam.position;   // all ships gone — hold last

            g_cam.position = HMM_AddV3(target_pos, c.cam_keys[0].pos);
            // Live override from the Studio panel.
            if (g_offset_override_active)
                g_cam.position = HMM_AddV3(target_pos, g_offset_override);
            const bool cut = (g_cam_cue != g_prev_cam_cue);
            g_cam_vel = (!cut && dt > 1e-5f)
                      ? HMM_DivV3F(HMM_SubV3(g_cam.position, prev), dt)
                      : HMM_V3(0, 0, 0);

            // look_at: the centroid.
            const HMM_Vec3 dir = HMM_SubV3(target_pos, g_cam.position);
            if (HMM_LenV3(dir) > 1e-3f) {
                const HMM_Quat base = look_rotation::make(
                    HMM_MulV3F(dir, -1.0f), HMM_V3(0, 1, 0));
                g_cam.orientation = HMM_NormQ(HMM_MulQ(base,
                    HMM_QFromAxisAngle_RH(HMM_V3(0, 0, 1), 3.14159265358979f)));
            }
        } else if (!c.cam_keys.empty()) {
            // SPLINE MODE (original): Catmull-Rom through key positions.
            std::vector<HMM_Vec3> pts;
            pts.reserve(c.cam_keys.size());
            for (const CamKey& k : c.cam_keys) pts.push_back(k.pos);
            g_cam.position = eval_spline(pts, u, c.ease_smooth);
            const bool cut = (g_cam_cue != g_prev_cam_cue);
            g_cam_vel = (!cut && dt > 1e-5f)
                      ? HMM_DivV3F(HMM_SubV3(g_cam.position, prev), dt)
                      : HMM_V3(0, 0, 0);

        // look_at: linear across keys; resolve ship: targets to live pos.
        if (!c.cam_keys.empty()) {
            const int n = (int)c.cam_keys.size();
            const float fseg = u * (float)std::max(1, n - 1);
            int seg = (int)std::floor(fseg);
            if (seg > n - 2) seg = std::max(0, n - 2);
            const float lt = fseg - (float)seg;
            auto key_look = [&](const CamKey& k) -> HMM_Vec3 {
                if (k.look_is_ship) {
                    auto it = g_actors.find(k.look_actor);
                    if (it != g_actors.end())
                        if (Ship* s = ships.get(ships.find_handle_by_id(it->second)))
                            return s->position;
                }
                return k.look_pt;
            };
            const CamKey& ka = c.cam_keys[seg];
            const CamKey& kb = c.cam_keys[std::min(seg + 1, n - 1)];
            HMM_Vec3 look = (ka.has_look || kb.has_look)
                          ? HMM_LerpV3(key_look(ka), lt, key_look(kb))
                          : g_cam.position;   // no look_at authored -> keep facing
            const HMM_Vec3 dir = HMM_SubV3(look, g_cam.position);
            if (HMM_LenV3(dir) > 1e-3f) {
                // Camera looks down LOCAL -Z, so aim -dir (see camera.h / autopilot).
                const HMM_Quat base = look_rotation::make(HMM_MulV3F(dir, -1.0f),
                                                          HMM_V3(0, 1, 0));
                // The ship-sprite atlas is authored for a camera ROLLED 180deg
                // about its view axis (the sprite screen-roll aligns cap-up to
                // cam-up, so cam orientation directly drives on-screen roll).
                // The flight/orbit + title chase-cam paths add this same 180deg
                // local-Z roll (see main.cpp scene_cam + title_scene::camera_look_at
                // + wants_camera_roll); without it the whole cinematic renders
                // upside down. Compose on the RIGHT = local-frame (forward) roll.
                // NOTE: a per-keyframe / per-cinematic roll or up-vector override
                // could later be layered on top of `base` here for dutch angles;
                // deliberately NOT added now — default orientation is upright.
                g_cam.orientation = HMM_NormQ(HMM_MulQ(base,
                    HMM_QFromAxisAngle_RH(HMM_V3(0, 0, 1), 3.14159265358979f)));
            }
        }
        }  // end spline mode
    } else {
        // No camera cue driving the pose this frame -> no motion, no streaks.
        g_cam_vel = HMM_V3(0, 0, 0);
    }
    // Clear the live offset override when the camera cue changes (hard cut)
    // so a new shot starts fresh from its authored offset.
    if (g_cam_cue != g_prev_cam_cue)
        g_offset_override_active = false;
    g_prev_cam_cue = g_cam_cue;

    // ---- drive actor paths -------------------------------------------------
    for (const Cue& c : g_cin.cues) {
        if (c.cmd != Cmd::ActorPath || g_time < c.t) continue;
        auto it = g_actors.find(c.actor);
        if (it == g_actors.end()) continue;
        Ship* s = ships.get(ships.find_handle_by_id(it->second));
        if (!s || !s->sprite) continue;
        const float u = (c.dur > 0.0f) ? std::clamp((g_time - c.t) / c.dur, 0.0f, 1.0f) : 1.0f;
        const HMM_Vec3 prev = s->sprite->position;
        const HMM_Vec3 next = eval_spline(c.actor_keys, u, /*smooth=*/true);
        // Drive BOTH the sprite (render pose) AND the ship's physics position
        // so the follow camera (which reads s->position) tracks correctly.
        s->sprite->position = next;
        s->position         = next;
        const HMM_Vec3 vel  = HMM_SubV3(next, prev);
        if (HMM_LenV3(vel) > 1e-3f)   // face direction of travel
            s->sprite->orientation = look_rotation::make(vel, HMM_V3(0, 1, 0));
        s->sprite->forward_speed = 0.0f;   // director owns the pose, not physics
    }

    // ---- natural end -------------------------------------------------------
    if (g_ending || g_time >= g_end_at + 0.25f) end_cinematic("natural");
}

// ---- overlay ---------------------------------------------------------------
namespace {

float fade_alpha() {
    // Alpha from the most-recently-started fade cue. Default 0 (clear).
    float alpha = 0.0f;
    for (const Cue& c : g_cin.cues) {
        if (g_time < c.t) break;   // time-sorted
        if (c.cmd == Cmd::FadeIn) {
            const float u = (c.dur > 0.0f) ? std::clamp((g_time - c.t) / c.dur, 0.0f, 1.0f) : 1.0f;
            alpha = 1.0f - u;
        } else if (c.cmd == Cmd::FadeOut) {
            const float u = (c.dur > 0.0f) ? std::clamp((g_time - c.t) / c.dur, 0.0f, 1.0f) : 1.0f;
            alpha = u;
        }
    }
    return alpha;
}

// 0..1 slide factor for a line cue (0 = fully off-edge, 1 = fully in).
float line_slide(const Cue& c) {
    const float local = g_time - c.t;
    const float in  = std::clamp(local / k_slide_s, 0.0f, 1.0f);
    const float out = std::clamp((c.dur - local) / k_slide_s, 0.0f, 1.0f);
    return std::min(in, out);
}

void draw_centered_text(ImDrawList* dl, float size, float cx, float cy,
                        ImU32 col, const char* txt) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, txt);
    dl->AddText(font, size, ImVec2(cx - sz.x * 0.5f, cy - sz.y * 0.5f), col, txt);
}

void draw_portrait(ImDrawList* dl, float fb_w, float fb_h, const Cue& c) {
    const float ph = fb_h * k_panel_h_frac;
    const float pw = ph * k_portrait_aspect;
    const float inset = fb_h * k_panel_inset_frac;
    const float slide = line_slide(c);
    // Off-screen edge -> resting position, eased by slide.
    const float rest_x = c.side_left ? inset : (fb_w - inset - pw);
    const float off_x  = c.side_left ? (-pw - inset) : (fb_w + inset);
    const float x = off_x + (rest_x - off_x) * slide;
    const float y = fb_h - inset - ph;   // bottom-anchored

    const ImVec2 pmin(x, y), pmax(x + pw, y + ph);
    const ImU32 frame_col = IM_COL32(0x33, 0xcc, 0xff, (int)(255 * slide));
    const ImU32 fill_col  = IM_COL32(8, 14, 22, (int)(230 * slide));

    // Neutral backing fill (also the fallback when the PNG is missing).
    dl->AddRectFilled(pmin, pmax, fill_col);
    const TextureSlot& tex = c.portrait.empty() ? TextureSlot{}
                                                : portrait_texture(c.portrait);
    if (tex.valid) {
        const ImU32 tint = IM_COL32(255, 255, 255, (int)(255 * slide));
        dl->AddImage(simgui_imtextureid(tex.view), pmin, pmax,
                     ImVec2(0, 0), ImVec2(1, 1), tint);
    }
    // Comm-VDU frame.
    dl->AddRect(pmin, pmax, frame_col, 0.0f, 0, 3.0f);
    // Nameplate bar across the bottom of the panel.
    const float nh = ph * 0.11f;
    dl->AddRectFilled(ImVec2(x, y + ph - nh), pmax,
                      IM_COL32(0x11, 0x22, 0x33, (int)(220 * slide)));
    draw_centered_text(dl, nh * 0.7f, x + pw * 0.5f, y + ph - nh * 0.5f,
                       IM_COL32(0x99, 0xdd, 0xff, (int)(255 * slide)),
                       c.speaker.c_str());
}

} // anonymous namespace

void draw_overlay(float fb_w, float fb_h) {
    if (!g_active) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Letterbox bars (animate in over the first k_letterbox_anim_s, and
    // back out over the final one).
    if (g_cin.letterbox) {
        const float in   = std::clamp(g_time / k_letterbox_anim_s, 0.0f, 1.0f);
        const float out  = std::clamp((g_end_at - g_time) / k_letterbox_anim_s, 0.0f, 1.0f);
        const float frac = std::min(in, out);   // ease in at start, out at end
        const float bar  = fb_h * k_letterbox_frac * frac;
        const ImU32 black = IM_COL32(0, 0, 0, 255);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(fb_w, bar), black);
        dl->AddRectFilled(ImVec2(0, fb_h - bar), ImVec2(fb_w, fb_h), black);
    }

    // Portrait panels + line subtitles for the currently-active line cues.
    // Two overlapping lines on opposite sides render both panels (exchange).
    for (const Cue& c : g_cin.cues) {
        if (c.cmd != Cmd::Line) continue;
        if (g_time < c.t || g_time > c.t + c.dur) continue;
        draw_portrait(dl, fb_w, fb_h, c);
        if (!c.text.empty()) {
            const float alpha = line_slide(c);
            draw_centered_text(dl, fb_h * 0.028f, fb_w * 0.5f,
                               fb_h * (1.0f - k_letterbox_frac) - fb_h * 0.04f,
                               IM_COL32(255, 255, 255, (int)(255 * alpha)),
                               c.text.c_str());
        }
    }

    // Bare subtitles (no portrait).
    for (const Cue& c : g_cin.cues) {
        if (c.cmd != Cmd::Subtitle) continue;
        if (g_time < c.t || g_time > c.t + c.dur) continue;
        draw_centered_text(dl, fb_h * 0.030f, fb_w * 0.5f,
                           fb_h * (1.0f - k_letterbox_frac) - fb_h * 0.05f,
                           IM_COL32(230, 230, 255, 255), c.text.c_str());
    }

    // Full-screen fade (drawn last so it covers everything, letterbox incl.).
    const float a = fade_alpha();
    if (a > 0.001f)
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(fb_w, fb_h),
                          IM_COL32(0, 0, 0, (int)(255 * a)));
}

} // namespace cinematic

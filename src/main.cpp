// -----------------------------------------------------------------------------
// new_privateer — entry point.
//
// Stage 3 of N: flight + star + dust.
//
// Controls:
//   mouse          — aim (fly-by-wire: the nose chases the cursor)
//   + / - (hold)   — throttle: ramp cruising speed up / down
//   Tab (hold)     — afterburner: snap to full afterburn speed; release
//                    returns to the throttle setting
//   , / . (hold)   — roll: rotate around the view axis. , rolls left,
//                    . rolls right. Held (release levels out via the
//                    camera's angular damping). Coexists with mouse
//                    aim — pitch/yaw by mouse + roll by keys.
//   N              — cycle selected nav point; opens the navmap if
//                      closed (Esc / X to close). N cycles inside an
//                      already-open navmap.
//   A              — autopilot to selected nav (hostile-gated; any input cancels)
//   D              — dock at selected base when cleared
//   Escape (×2)    — quit (double-tap within 1s so accidental taps are safe)
//
//   Open / unbound (free for new bindings — W, S, Q, E, R, F, Z, C, X):
//     W/S — no longer forward/back throttle
//     Q/E — no longer strafe
//     R/F — no longer pitch (up/down)
//     Z/C — no longer roll
//     X   — no longer brake / fire (now used by gun cycle as alt-fire)
//   Bind them to new features (shield level toggle, weapon group cycling,
//   etc.) and update this header + the in-game reminder.
// -----------------------------------------------------------------------------

#include "sokol_log.h"
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_time.h"
#include "sokol_debugtext.h"

#include "armor.h"
#include "asteroid.h"
#include "atlas_grid_viewer.h"
#include "audio.h"
#include "sound_labeler.h"
#include "music_labeler.h"
#include "speech_labeler.h"
#include "camera.h"
#include "cockpit_hud.h"
#include "comm.h"
#include "commodity.h"
#include "economy.h"
#include "outfitting.h"
#include "missions.h"
#include "mission_tracker.h"
#include "player.h"
#include "savegame.h"
#include "base_screens.h"
#include "debug_panel.h"
#include "dev_remote.h"
#include "docking.h"
#include "autopilot.h"
#include "threat.h"
#include "jump.h"
#include "encounters.h"
#include "dust.h"
#include "warp_streaks.h"
#include "jump_gate.h"
#include "faction.h"
#include "game_state.h"
#include "gun.h"
#include "mesh_render.h"
#include "explosion.h"
#include "firing.h"

// Defined in mesh_render.cpp (external linkage). Forward-declared at global
// scope so the dev_remote /project publish in frame() resolves to the
// real symbol instead of an anonymous-namespace ghost.
HMM_Mat4 model_matrix(HMM_Vec3 pos, HMM_Vec3 euler_deg, float s);
#include "perception.h"
#include "world_scale.h"
#include "bolt_art.h"
#include "projectile.h"
#include "missile.h"
#include "sfx.h"
#include "music.h"
#include "ship.h"
#include "ai_brain.h"
#include "ship_ai.h"
#include "tracelog.h"
#include "ship_registry.h"
#include "ship_class.h"
#include "shield.h"
#include "hazards.h"
#include "sprite.h"
#include "ship_sprite.h"
#include "sprite_light_editor.h"
#include "sprite_generation_tool.h"
#include "mesh_orient_editor.h"
#include "navmap_auditor.h"

#include <unordered_map>
#include "obj_loader.h"
#include "planet_texture.h"
#include "postprocess.h"
#include "render_config.h"
#include "rendertargets.h"
#include "skybox.h"
#include "sky_family.h"
#include "star_presets.h"
#include "sun.h"
#include "system_def.h"
#include "galaxy.h"
#include "title_screen.h"
#include "title_scene.h"


#include "imgui.h"   // ImGui::GetIO() for WantCaptureMouse handoff

#include <algorithm>   // std::clamp, std::min, std::max, std::sort
#include <array>
#include <cmath>       // std::sin/cos/sqrt; needs _USE_MATH_DEFINES for M_PI on MSVC
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// --ship <name> CLI override for the player's starting ShipClass. Empty ->
// default (tarsus). Looked up via ship_class::find(); unknown names fall back
// to tarsus with a warning. Lets us fly a Talon to verify multi-gun fire,
// stress-test heavy handling, etc., without editing the spawn code. Declared
// up here (not next to sokol_main) because the player spawn block ~line 955
// also needs to see it.
std::string g_player_ship_override;

// Pre-static-init trace + crash-trap installers. On Windows, runtime
// failures during static init (bad CRT param, terminate, purecall, etc.)
// invoke __fastfail with code 0xC0000409, killing the process before any
// of our code runs and printing nothing. Install handlers as the FIRST
// static initializer in this TU so anything failing later — here or in
// another TU's globals — hits our trap with file/line info before dying.
#ifdef _WIN32
#include <crtdbg.h>
#include <stdlib.h>
#include <exception>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
static void _np_print_stack() {
    static bool sym_inited = false;
    if (!sym_inited) {
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        sym_inited = true;
    }
    void* frames[24] = {};
    USHORT n = CaptureStackBackTrace(0, 24, frames, nullptr);
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    IMAGEHLP_LINE64 line = {}; line.SizeOfStruct = sizeof(line);
    for (USHORT i = 0; i < n; ++i) {
        DWORD64 disp = 0; DWORD line_disp = 0;
        const char* name = "<unknown>"; const char* file = ""; DWORD ln = 0;
        if (SymFromAddr(GetCurrentProcess(), (DWORD64)frames[i], &disp, sym)) {
            name = sym->Name;
        }
        if (SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)frames[i], &line_disp, &line)) {
            file = line.FileName; ln = line.LineNumber;
        }
        std::fprintf(stderr, "  [%u] %p %s  (%s:%lu)\n", i, frames[i], name, file, ln);
    }
    std::fflush(stderr);
}
static void _np_invalid_parameter(const wchar_t* expr, const wchar_t* func,
                                  const wchar_t* file, unsigned int line,
                                  uintptr_t /*reserved*/) {
    std::fprintf(stderr,
        "[trace] CRT invalid_parameter: expr=%ls func=%ls file=%ls:%u\n",
        expr ? expr : L"<null>", func ? func : L"<null>",
        file ? file : L"<null>", line);
    _np_print_stack();
}
static void _np_terminate() {
    std::fprintf(stderr, "[trace] std::terminate() called\n");
    _np_print_stack();
    std::abort();
}
static LONG WINAPI _np_unhandled_exception(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "[trace] unhandled SEH exception code=0x%08lx addr=%p\n",
                 info->ExceptionRecord->ExceptionCode,
                 info->ExceptionRecord->ExceptionAddress);
    _np_print_stack();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
struct _StaticInitTrace {
    _StaticInitTrace() {
        std::fprintf(stderr, "[trace] static-init phase reached\n");
#ifdef _WIN32
        _set_invalid_parameter_handler(_np_invalid_parameter);
        _CrtSetReportMode(_CRT_ASSERT, 0);   // don't pop dialogs
        std::set_terminate(_np_terminate);
        SetUnhandledExceptionFilter(_np_unhandled_exception);
#endif
        std::fflush(stderr);
    }
};
static _StaticInitTrace g_trace_static_init;

struct AppState {
    sg_pass_action scene_pass_action{};

    // Top-level game-mode machine (Flight | Landed | Dying | Loading).
    // Transitions are requested via game_state::request_mode (from the
    // debug panel combo or the non-Flight Escape handler) and applied
    // at the top of frame_cb — never mid-frame. See game_state.h.
    GameState      game{};

    // Request-landing + autodock approach (np-9cu.1). Owns the Flight↔
    // Landed bridge; tick'd inside the Flight path, drives the camera
    // during the auto-approach and requests the Landed mode flip.
    Docking        docking{};
    // Auto-land zone latch (np-3dp.22): true once we've announced the
    // 'entering an automatic landing zone' comms for the current approach;
    // re-armed when the player leaves the announce radius so re-entering
    // re-announces.
    bool           landing_zone_announced = false;

    // Nav-point cruise autopilot (np-opa.3). Press A to fly to the
    // selected nav. Like docking it owns the camera while engaged
    // (controls_locked mutes pilot input), but it's a traversal
    // controller — it ends in free flight at the nav, not a mode flip.
    // Hostile-gated through threat:: (stub today; live with np-ma2.3).
    Autopilot      autopilot{};

    Camera         camera{};
    Skybox         skybox{};
    Sun            sun{};
    DustField      dust{};
    WarpStreaks    warp_streaks{};   // autopilot cruise overlay (np-streaks)
    JumpGate       jump_gate{};      // pulsing translucent gate spheres
    RenderTargets  rt{};
    PostProcess    post{};

    // Zero-or-more asteroid fields, one per entry in the system's JSON.
    std::vector<AsteroidField> asteroid_fields;

    // Statically-placed meshes (ships, stations, debris). Loaded from the
    // system's `placed_meshes` list at startup.
    MeshRenderer            mesh_render{};
    std::vector<PlacedMesh> placed_meshes;

    // Sprite-with-lights renderer (np-0kv epic). Sprites are placed via
    // the system JSON's `placed_sprites` array (np-0kv.4). `sprite_art`
    // caches SpriteArt by stem path so the same PNG pair loads exactly
    // once even when multiple instances reference it.
    SpriteRenderer                                       sprite_render{};
    BoltArtSet                                           bolt_art;
    std::vector<sg_view>                                 bolt_textures;
    int                                                  bolt_tex_offsets[kGunTypeCount]{};
    std::unordered_map<std::string, SpriteArt>           sprite_art;
    std::vector<SpriteObject>                            placed_sprites;
    std::unordered_map<std::string, ShipSpriteAtlas>      ship_sprite_atlases;
    // NPC ship sprites. std::deque (NOT vector) so growth never
    // relocates elements — Ship::sprite holds pointers into this
    // container for the ship's whole lifetime, and runtime spawning
    // (debug buttons today, encounter generation per np-ma2.3 later)
    // appends without dangling every existing back-pointer. Slots are
    // never erased: a despawned ship's sprite slot gets atlas=nullptr
    // (renderer + integrator both skip it) and its index parks in
    // free_sprite_slots for the next spawn to reuse. Chosen over a
    // second slot-map because sprites need no handles — the OWNING
    // Ship's lifetime is the sprite's lifetime, nothing else refers
    // to a sprite slot across frames.
    std::deque<ShipSpriteObject>                         placed_ship_sprites;
    std::vector<size_t>                                  free_sprite_slots;
    // Per-instance Ship objects, slot-map storage (ship_registry.h).
    // The player spawns FIRST at startup and therefore always occupies
    // slot 0 (ShipRegistry::player_handle()); NPCs fill subsequent
    // slots in JSON order, exactly matching the old vector's layout.
    // The old ships[1+i] <-> placed_ship_sprites[i] positional lockstep
    // is GONE — the only sprite linkage is the explicit Ship::sprite
    // pointer, which deque storage keeps stable (see above).
    ShipRegistry                                         ships;

    // Persistent player state — credits, cargo, owned ship + equipment,
    // per-faction reputation, location (player.h). Initialized by
    // player::new_game() in init_cb (honoring the --system override);
    // the trading / dealer / save-load features all mutate this struct
    // and nothing else. Reputation lives at g.player.rep now — default
    // zero ("unknown stranger") so faction baselines determine starting
    // stance: Pirates jump you (-30 baseline), Confeds tolerate you (0),
    // Kilrathi attack on sight (-100).
    PlayerState                                          player;

    // Live projectiles. Spawned by firing::tick (when controller.fire_guns
    // is set on a ship with off-cooldown mounts), advanced by
    // projectile::tick, drawn by SpriteRenderer::draw_tracers. Empty in
    // the steady state until someone pulls a trigger.
    std::vector<Projectile>                              projectiles;

    // Live guided missiles (np-zte.2). Spawned on the missile-fire key
    // (finite ammo in g.player.missiles), steered + detonated by
    // missile::tick / missile::collide_and_damage, drawn alongside the
    // projectile tracers. Empty until the player launches one.
    std::vector<Missile>                                 missiles;
    // Currently-selected missile type for the fire key (index into
    // MissileType: 0=DF,1=HS,2=IR). Cycled with the missile-type key.
    // Transient UI state — NOT persisted (ammo counts are, in PlayerState).
    int                                                  selected_missile = 0;
    // Edge-triggered fire request: the missile-fire key sets this in
    // event_cb (one per press, key-repeat suppressed); frame_cb consumes
    // it where the player ship's pose is fresh so the missile spawns from
    // the right muzzle and gets its first integration step.
    bool                                                 missile_fire_request = false;
    // Target-lock state machine for HS/IR missiles. progress_s grows while
    // the player holds a target with a lock-requiring missile selected;
    // IR needs the build-up (k_ir_lock_time), HS locks instantly. `locked`
    // flips true on completion; `locked_id` is the ship we're locked onto;
    // seek_beep_s paces the seeking tone.
    struct MissileLock {
        float    progress_s   = 0.0f;
        bool     locked       = false;
        uint32_t locked_id    = 0;
        float    seek_beep_s  = 0.0f;
    } missile_lock;

    // Active explosion FX. One Explosion per ship death, lifetime ~1.2s.
    // Drawn additively via the same spot pipeline as tracers, just with
    // a per-explosion size+color curve (see explosion.h for the
    // flash + shockwave layering rationale).
    std::vector<Explosion>                               explosions;

    // Shield-impact flashes. One per ship-facing that took shield damage
    // this frame; brief cyan bubble around the ship — reads as shield
    // lighting up under fire. Spawned in the damage-detection pass via
    // a before/after shield-value comparison.
    struct ShieldFlash {
        HMM_Vec3 position;
        float    radius;
        float    age_s;
        float    lifetime_s;
    };
    std::vector<ShieldFlash>                             shield_flashes;

    // Armor-impact flashes. Same structure as ShieldFlash but rendered
    // smaller (no shield bubble — hits land ON the hull) and orange-red
    // (sparking metal). Triggered when armor cm drops, NOT shields —
    // signals "shields down, hull taking damage" without needing a
    // separate UI cue.
    struct ArmorFlash {
        HMM_Vec3 position;
        float    radius;
        float    age_s;
        float    lifetime_s;
    };
    std::vector<ArmorFlash>                              armor_flashes;

    // Player damage indicator — screen-edge red vignette intensity. Set
    // to 1.0 whenever the player ship takes any damage (shield or armor),
    // decays exponentially each frame. Renders as a red gradient on the
    // screen border, fading toward center. The classic FPS "you're being
    // hit" cue without needing a dedicated full-screen shader.
    float                                                player_hit_intensity = 0.0f;
    std::vector<SpriteObject>                            frame_sprites;

    // Held-key flags, indexed by sapp key code. Flight physics reads these
    // every frame so we don't rely on key-repeat timing.
    std::array<bool, SAPP_MAX_KEYCODES> keys_down{};

    // Fly-by-wire aim mode. true = mouse cursor is hidden and its
    // screen position drives ship yaw/pitch (Freelancer feel). false =
    // cursor is visible and the ship freezes its turn input — used
    // when the player wants to click ImGui panels. SPACE toggles.
    bool     fly_by_wire = false;

    // Latest mouse position in *logical* (HiDPI-corrected) pixels,
    // populated on SAPP_EVENTTYPE_MOUSE_MOVE. We store it once and
    // reuse it from frame_cb (for ship aim) and cockpit_hud (for
    // drawing the on-screen aim cursor).
    float    mouse_x = 0.0f;
    float    mouse_y = 0.0f;
    bool     mouse_left_held  = false;
    bool     mouse_right_held = false;

    uint64_t last_frame_ticks = 0;
    uint64_t last_fps_ticks   = 0;
    uint32_t frames_since     = 0;

    // Anti-Boodler escape-hatch: one tap arms it, a second tap within 1s
    // actually quits. Single strays just flash a reminder in the terminal.
    uint64_t escape_armed_ticks = 0;

    std::string system_name = "troy";   // assets/systems/<name>.json
    // Dev affordance (np-9cu.4): --dev-land <base_id> boots straight into
    // Landed mode at that base so you can iterate on concourse art / base
    // screens without flying 150 km and autodocking every launch. "" = off.
    std::string dev_land_base;
    // Save-load boot (np-ymp.1): --load <slot> / --continue (slot 0) ask
    // init_cb to deserialize that slot over the fresh new_game() state.
    // -1 = no load requested (default = new game, as before). When set and
    // --system was NOT explicitly passed, we adopt the save's recorded
    // system so we load the world the player saved in (see init_cb).
    int         load_slot       = -1;
    // Death-test affordance (np-ma2.2): --dev-kill-at <secs> raises the
    // "kill player" request once after that many seconds of Flight, so the
    // death->Dying->respawn cycle can be exercised headlessly (the GUI
    // button needs a human clicking ImGui). <0 = off.
    float       dev_kill_at_s   = -1.0f;
    float       dev_run_clock_s = 0.0f;    // wall-ish clock for dev timers
    bool        system_explicit = false;   // true once --system is seen on the CLI
    bool        capture_clean = false;  // hide HUD/cockpit overlay for atlas screenshots
    // Ship-sprite frame HUD: prints camera az/el and picked atlas cell az/el
    // for every placed_ship_sprites entry, every frame. F3 toggles. Hidden by
    // --capture-clean so screenshots stay HUD-free without extra flags.
    bool        show_ship_frame_hud = false;
    StarSystem  system{};

    // Galaxy graph (np-6al.1) — the system catalog + jump topology, parsed
    // once at boot from assets/galaxy.json. Drives runtime system switching
    // (load_and_build_system) and, later, the jump mechanic (np-6al.3).
    galaxy::Galaxy galaxy{};
    bool           system_loaded = false;   // true once a scene has been built

    // Deferred (frame-boundary) system switch. The timers / debug dropdown
    // set `pending_goto`; it's consumed at the TOP of frame_cb so a switch
    // never tears down GPU/scene state mid-frame (same deferral discipline
    // as game-mode transitions + the debug spawn requests). Empty = none.
    std::string pending_goto;
    // Pending jump (np-6al.3). Set when the player presses J at an eligible
    // gate; consumed during the Loading cinematic by execute_jump(), which
    // rebuilds the destination system and drops the player at arrival_nav
    // (the reciprocal gate on the far side). Both empty = no jump in flight.
    std::string pending_jump_system;   // destination galaxy id
    std::string pending_jump_nav;      // arrival gate name in that system
    // --goto <id>: fire a single deferred switch after goto_at_s of Flight,
    // proving runtime switching WITHOUT the jump mechanic (np-6al.3).
    std::string goto_system;
    float       goto_at_s     = 2.0f;
    // --goto-soak <n>: cycle the galaxy's systems n times on an interval —
    // the leak/crash soak. Quits cleanly afterward so cleanup_cb runs.
    int         soak_remaining = 0;
    float       soak_interval  = 1.0f;
    float       switch_clock_s = 0.0f;
    int         soak_index     = 0;
    bool        soak_quit      = false;

    // --dev-jump-soak <n> (np-6al.3): headless J-press driver. Auto-jumps
    // through the first surveyed gate n times on dev_jump_interval, then
    // quits cleanly (leak audit). dev_jump_quit lingers one interval after
    // the last jump lands so the final arrival renders before shutdown.
    int         dev_jump_remaining = 0;
    float       dev_jump_interval  = 2.5f;
    float       dev_jump_clock_s   = 0.0f;
    bool        dev_jump_quit      = false;

    // Targeting / nav-cycling. -1 = no target. Press N to advance through
    // g.system.nav_points. Persists for the lifetime of the loaded system
    // (we don't currently swap systems at runtime; if/when we do, reset
    // this when the new system loads).
    int selected_nav = -1;

    // Player ship target. Cycled by the T key through every contact in
    // perception.visible[] sorted by distance ascending. 0 = no target.
    // Stored as a Ship::id rather than an index so it stays valid across
    // any future ship-array reshuffles. Resolved to a pointer each frame
    // when we need to display it.
    uint32_t player_target_id = 0;

    // Navmap overlay. N opens it when closed; while open, N cycles
    // through nav points in place (no close). Esc or the X button
    // closes. Big top-down view of the system's nav points + ship
    // contacts; clicking a nav selects it (matches the N-key cycle's
    // effect).
    bool show_navmap = false;

    // Welcome / alpha-intro overlay. Starts true so a fresh launch opens
    // paused on the briefing; dismissed with SPACE/ENTER (handled at the
    // very top of event_cb so it doesn't collide with fly-by-wire /
    // missile-fire). While true, frame_cb forces dt=0 so the world is
    // frozen behind the text and the player can read before the brawl
    // unfreezes.
    bool show_welcome = false;   // welcome briefing REMOVED in np-3dp; the title screen owns the chrome
    bool show_title   = true;   // title/loading screen at app launch

    // Flight pause toggle (np-pau.28): P in Flight mode freezes the sim
    // (dt=0, no AI, no fire requests) without changing GameMode. ImGui
    // and dev overlays stay live so the player can still inspect panels,
    // retarget, etc. The P handler is a KEY_DOWN edge trigger, so holding
    // the key can't strobe the state.
    bool paused       = false;
    // Lazy-init latch for the title SCENE (patrol/chase ships, sky, music).
    // Reset to false whenever we (re)enter the title — at launch and again
    // when the player dies and we bounce back to the menu (np-3dp.18) — so
    // the scene re-rolls a fresh variant + ship set each visit.
    bool title_scene_inited = false;
    bool skip_title_at_boot = false;   // --skip-title dev flag
    bool dev_seed_missions  = false;   // --dev-missions: one-shot accept a few jobs (dev/test)
    // Set when a save is loaded (np-3dp.19): the next Flight frame copies
    // the loaded PlayerState hp_* snapshot onto the live player Ship
    // BEFORE the per-frame ship->PlayerState mirror, so reloading a
    // damaged save keeps you damaged. One-shot; cleared after apply.
    bool apply_health_pending = false;
    bool show_load_menu       = false;   // title LOAD picker open (np-3dp.19)

    // Title 'galaxy tour' (np-3dp.13): during the ChaseCam variant the hero
    // ship cruises toward a jump hole and 'jumps' (sky reskin + sun repos)
    // on arrival, every 90s. All of the timing + the flash live in
    // title_scene now (jump_flash() / consume_jump_event() / jump_hole_pos());
    // main just applies them. No per-frame State needed here.

    // 3rd-person orbit/freelook camera, active while the nav autopilot is
    // engaged (the ship flies itself, so the player is free to look around).
    // The ship keeps flying via g.camera; orbit_cam is a SEPARATE render
    // camera that orbits the ship at orbit_dist, aimed by the mouse
    // (offset-from-centre drives yaw/pitch rate; scroll changes distance).
    // player_ship_sprite renders the player's hull (no sprite in 1st person)
    // so there's actually something to look at.
    bool      orbit_active   = false;
    bool      orbit_was_active = false;   // edge detect to seed angles on engage
    float     orbit_yaw      = 0.0f;      // world-frame, radians
    float     orbit_pitch    = 0.20f;     // world-frame, radians (+ = above)
    float     orbit_dist     = 600.0f;    // camera distance from the ship
    Camera    orbit_cam{};
    ShipSpriteAtlas*               player_atlas = nullptr;   // resolved player hull atlas
    ShipSpriteObject               player_ship_sprite{};
    std::deque<ShipSpriteObject>   player_sprite_scratch;    // 1-elem feed for frame select

    // Deferred ship spawn/despawn requests from the debug panel's
    // registry smoke-test buttons. Applied at the top of frame_cb —
    // never mid-frame — because half the frame's systems hold Ship&s
    // and slot indices across the sim pass. Same deferral discipline
    // as game-mode transitions.
    debug_panel::ShipDebugRequests  ship_debug;
    // Audio smoke-test requests (np-3gw.1), consumed alongside.
    debug_panel::AudioDebugRequests audio_debug;

    // Test SFX sample ids, loaded at init right after audio::init.
    // 0 = load failed (audio runs silent, buttons no-op with a log).
    SampleId sfx_blip  = 0;
    SampleId sfx_burst = 0;
    SampleId sfx_hum   = 0;
};

// Trace before g's ctor (whose static-init order vs. g_trace_static_init
// is deterministic within this TU — top-to-bottom).
struct _PreG { _PreG()  { std::fprintf(stderr, "[trace] pre-g\n");  std::fflush(stderr); } };
static _PreG g_trace_pre_g;
AppState g;
// Dev gun-mount tuner overlay — hidden by default, F4 toggles it. Kept in
// the build for future per-ship muzzle tuning; off so it doesn't clutter.
static bool g_show_mount_tuner = false;
struct _PostG { _PostG() { std::fprintf(stderr, "[trace] post-g\n"); std::fflush(stderr); } };
static _PostG g_trace_post_g;

// ---- ship-sprite pool helpers -----------------------------------------------
//
// Claim/park a slot in g.placed_ship_sprites. The encounter director
// (spawn/despawn), the debug spawn/despawn buttons, and the NPC death-reap
// path (np-zte.1) all need EXACTLY this dance — reuse a parked slot if one's
// free, else append (deque growth keeps every Ship::sprite pointer valid);
// to park, linear-scan for the slot a sprite pointer aliases, wipe it to
// atlas=nullptr (the "unoccupied" marker renderer + integrator both skip)
// and push its index onto the free-list. Extracted to kill the 4x copy.
size_t claim_sprite_slot() {
    size_t slot;
    if (!g.free_sprite_slots.empty()) {
        slot = g.free_sprite_slots.back();
        g.free_sprite_slots.pop_back();
    } else {
        slot = g.placed_ship_sprites.size();
        g.placed_ship_sprites.emplace_back();
    }
    g.placed_ship_sprites[slot] = ShipSpriteObject{};   // clear any prior occupant
    return slot;
}

void free_sprite_slot(ShipSpriteObject* sprite) {
    if (!sprite) return;
    for (size_t i = 0; i < g.placed_ship_sprites.size(); ++i) {
        if (&g.placed_ship_sprites[i] == sprite) {
            g.placed_ship_sprites[i] = ShipSpriteObject{};   // atlas=nullptr -> parked
            g.free_sprite_slots.push_back(i);
            return;
        }
    }
}

// ---- input → camera mapping -------------------------------------------------
//
// The player's cruising speed setting (m/s), dialed by the + / - keys.
// This is the ONLY manual flight input now: the ship aims by mouse and
// holds whatever speed this says. Afterburner (Tab) temporarily overrides
// it to full afterburn speed; on release we snap back to this value.
// File-scope so event_cb (which edits it) and frame_cb (which reads it)
// share the same value.
static float g_speed_input_ref = 0.0f;

// ---- system (re)load orchestration (np-6al.1) -------------------------------
// Forward decls so init_cb + frame_cb can drive the runtime switch. Defined
// just below init_cb.
void unload_current_system();
void build_system_scene(bool first_time);
bool load_and_build_system(const std::string& id, bool first_time);
static uint32_t encounter_spawn(const encounters::SpawnRequest& req);   // defined below

// ---- sokol callbacks --------------------------------------------------------

void init_cb() {
    sg_desc desc{};
    desc.environment = sglue_environment();
    desc.logger.func = slog_func;
    // Sokol pool sizes. Defaults (128 images / 128 views / 128 buffers) blew
    // up the moment we shipped a second view-sphere atlas: each ship cell
    // burns *both* an image and a view slot (80 + 80 per ship), so two ships
    // alone need 160+160 before we count standalone sprites, skybox, offscreen
    // render targets, ImGui's font atlas, etc. Pool overflow is silent —
    // sg_make_image returns SG_INVALID_ID which downstream code reads as
    // 'failed to load', not a hard crash, so the HUD just disappears.
    //
    // 8k slots is generous headroom for the current per-cell-image scheme
    // (~50 ships' worth) and the slot tracking structs themselves are small
    // (the real GPU cost is the textures behind them, not the slots).
    //
    // SCALING NOTE: this number can't grow forever — see
    // docs/ARCHITECTURE_TEXTURES.md. Once we approach ~50 ships' worth of
    // assets we should switch from one-image-per-cell to atlas-page-per-ship
    // (single 8192² sg_image holding all 80 cells as UV rects), which cuts
    // image-pool pressure 80x and is the precondition for LRU residency.
    desc.image_pool_size  = 8192;
    desc.view_pool_size   = 8192;
    desc.buffer_pool_size = 1024;
    sg_setup(&desc);
    stm_setup();

    // Buffered logger (issue #26). Bring it up before any other subsystem
    // so its buffered flush thread doesn't catch a tear-down race at exit.
    // Cheap to leave enabled; the flush thread sleeps when the queue is
    // empty and the producer never blocks (drops oldest on overflow).
    tracelog::init();

    // --- on-screen text HUD -----------------------------------------------
    // sokol_debugtext gives us several built-in bitmap fonts. We enable a
    // couple so we can style the HUD blocks differently later if we want.
    sdtx_desc_t sdt_desc{};
    sdt_desc.fonts[0]   = sdtx_font_kc854();    // chunky C64 vibe, readable
    sdt_desc.fonts[1]   = sdtx_font_oric();     // thinner for secondary lines
    // Pin the debugtext internal pipeline to the swapchain format — HUD is
    // drawn in the swapchain pass, not the offscreen scene pass.
    sdt_desc.context.color_format = kSwapchainColorFormat;
    sdt_desc.context.depth_format = SG_PIXELFORMAT_NONE;
    sdt_desc.context.sample_count = kSceneSampleCount;
    sdt_desc.logger.func = slog_func;
    sdtx_setup(&sdt_desc);

    // Clear color only shows if everything else fails to draw.
    g.scene_pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g.scene_pass_action.colors[0].clear_value = g.capture_clean
        ? sg_color{0.0f, 0.0f, 0.0f, 1.0f}
        : sg_color{0.02f, 0.02f, 0.06f, 1.0f};
    g.scene_pass_action.depth.load_action     = SG_LOADACTION_CLEAR;
    g.scene_pass_action.depth.clear_value     = 1.0f;

    // Offscreen scene target + bloom ping-pong. Framebuffer size comes from
    // sokol_app, which knows the HiDPI-scaled physical resolution.
    if (!g.rt.init(sapp_width(), sapp_height())) {
        std::fprintf(stderr, "[main] render targets init failed\n");
        std::exit(1);
    }
    if (!g.post.init()) {
        std::fprintf(stderr, "[main] post pipeline init failed\n");
        std::exit(1);
    }

    // ---- save-load: adopt the saved system (np-ymp.1) ---------------------
    // When resuming a save (--load/--continue), load the world the player
    // saved IN — unless --system was explicitly passed, in which case the
    // explicit override wins (documented precedence: explicit CLI > save >
    // default "troy"). We only need the recorded system name here, so peek()
    // reads it without applying the whole save; the full PlayerState load
    // happens after new_game() below.
    if (g.load_slot >= 0 && !g.system_explicit) {
        // --continue (slot 0) resumes the MOST RECENT timestamped save
        // (np-3dp.19); an explicit --load N peeks that legacy slot.
        savegame::SlotInfo info;
        if (g.load_slot == savegame::k_autosave_slot) {
            const auto saves = savegame::list_saves();
            if (!saves.empty()) info = saves.front();
        } else {
            info = savegame::peek(g.load_slot);
        }
        if (info.exists && !info.system.empty()) {
            std::printf("[save] resume recorded system '%s' — loading it\n",
                        info.system.c_str());
            g.system_name = info.system;
        }
    }

    // ---- galaxy graph (np-6al.1) -----------------------------------------
    // The system catalog + jump topology, parsed once before any system.
    // Non-fatal if missing — single-system play still works, you just
    // can't switch / jump.
    if (!galaxy::load("assets/galaxy.json", g.galaxy)) {
        std::fprintf(stderr, "[main] galaxy load failed — single-system mode\n");
    }

    // ---- build the initial system ----------------------------------------
    // Everything past this point (skybox, sun, belts, sprites, ships,
    // encounters) lives in build_system_scene so the EXACT same path can
    // tear down + rebuild for a runtime jump (load_and_build_system).
    // first_time=true runs the genuinely one-time work — GPU renderer
    // inits, design-data tables, the player's PlayerState + slot-0 Ship —
    // which the switch path must NOT repeat (the player persists across a
    // jump, carrying ship, cargo, credits and rep).
    if (!load_and_build_system(g.system_name, /*first_time=*/true)) {
        std::fprintf(stderr, "[main] could not load system '%s' — quitting\n",
                     g.system_name.c_str());
        std::exit(1);
    }
}

// ---- runtime-rerunnable scene build (np-6al.1) ------------------------------
//
// Builds the live scene from g.system (already parsed by the caller). Split
// out of init_cb so a runtime jump can re-run it after unload_current_system().
// `first_time` gates the one-time work (GPU renderer inits, design-data table
// loads, the player's slot-0 Ship + PlayerState) that must persist across a
// switch rather than be rebuilt.
// Fit the player's persistent loadout (PlayerState) onto the live slot-0
// Ship (np-3dp.25): bind the hull class, heal to full, and mount the guns
// named in p.gun_mounts. Mount POSITIONS + default types come straight from
// the ship class default_guns (authored in assets/ships/<hull>/ship.json) --
// the single source of truth for muzzle geometry. p.gun_mounts only decides
// how many hardpoints are FILLED and with what gun, in list order: a brand-
// new Tarsus fills slot 0 with its single laser, a bought second gun fills
// slot 1. Tune muzzle placement in the JSON, never here. Shared by the boot
// spawn AND the title NEW handler. Unknown / empty gun names fall back to a Laser.
static void apply_player_loadout(Ship& pl, const PlayerState& p, bool heal = true) {
    if (const ShipClass* k = ship_class::find(p.ship_class_name)) pl.klass = k;
    // Shield generator upgrade (np-3dp.26, np-3dp.28): each dealer shield
    // level adds the stock generator's cm again (Privateer's Shield
    // Generator 1/2/3 ladder). L0 = NO shields (mult 0); L1+ = scale of
    // the class default. Set BEFORE heal so heal_to_full fills to the
    // upgraded max (and doesn't leave phantom shield on a sold shield gen).
    pl.shield_mult = (float)p.shield_level;
    pl.fitted_armor = p.armor_name.empty() ? nullptr : armor::find(p.armor_name);
    if (!p.armor_name.empty() && !pl.fitted_armor) {
        std::printf("[outfit] WARN: armor '%s' not found; using hull default\n",
                    p.armor_name.c_str());
    }
    // Energy regen bookkeeping (np-3dp.27, np-3dp.28). Engine upgrade
    // ADDS GJ/s to the recharge rate (more power); shield gen DRAINS GJ/s
    // from it whenever installed (running cost). Net effect on energy_gj
    // is computed in firing::tick. engine_recharge_mult is a legacy
    // multiplicative field; keep it at 1.0 for NPCs.
    pl.engine_recharge_mult   = 1.0f;
    pl.engine_recharge_add_gj = outfitting::engine_recharge_bonus_for(p.engine_level);
    pl.shield_recharge_drain_gj = outfitting::shield_recharge_drain_for(p.shield_level);
    // heal=true on a fresh hull (boot / NEW / respawn / ship-swap); false on
    // a normal land->launch so battle damage you didn't pay to repair
    // persists across the base visit.
    if (heal) ship::heal_to_full(pl);
    const ShipClass* k = pl.klass;
    pl.mounts.clear();
    // Fill hardpoints from the ship class in list order. Count follows the
    // player's loadout (1 for a new Tarsus, 2 after buying a second), capped
    // at the number of hardpoints the hull actually has; with no loadout set
    // (--ship dev override) fill every hardpoint with its default gun.
    const size_t slots = k ? k->default_guns.size() : size_t{0};
    const size_t n = p.gun_mounts.empty()
                       ? slots
                       : std::min(p.gun_mounts.size(), slots);
    for (size_t i = 0; i < n; ++i) {
        GunMount m = k->default_guns[i];   // position + default type from ship.json
        if (i < p.gun_mounts.size() && !p.gun_mounts[i].empty()) {
            const GunType t = gun::from_name(p.gun_mounts[i]);
            m.type = (t == GunType::Count) ? GunType::Laser : t;   // player gun overrides type only
        }
        m.cone_half_angle_deg = 1.0f;
        pl.mounts.push_back(m);
    }
    pl.gun_cooldowns.assign(pl.mounts.size(), 0.0f);
    pl.gun_armed.assign(pl.mounts.size(), true);
}

static void apply_pending_player_health_snapshot(Ship& player) {
    if (!g.apply_health_pending) return;
    g.apply_health_pending = false;
    if (!g.player.hp_valid) return;

    player.armor_fore_cm      = g.player.hp_armor_fore;
    player.armor_aft_cm       = g.player.hp_armor_aft;
    player.armor_port_cm      = g.player.hp_armor_port;
    player.armor_starboard_cm = g.player.hp_armor_starboard;
    player.shield_fore_cm     = g.player.hp_shield_fore;
    player.shield_aft_cm      = g.player.hp_shield_aft;
    player.shield_port_cm     = g.player.hp_shield_port;
    player.shield_starboard_cm = g.player.hp_shield_starboard;
    player.energy_gj          = g.player.hp_energy;
    std::printf("[save] applied loaded ship damage to hull\n");
}

void build_system_scene(bool first_time) {
    g.camera.position = g.system.player_start;

    // Optional spawn aim. The camera's default forward is -Z (identity
    // orientation). To point it at an arbitrary world position, build
    // the shortest-arc rotation from -Z to the desired direction. World
    // +Y is the implicit up reference; if the look_at is exactly above
    // or below the spawn the resulting orientation has an arbitrary
    // roll, which the player can correct with mouse input — fine for a
    // first-frame nudge, not worth a full "keep up vector vertical"
    // pipeline yet.
    if (g.system.player_look_at_set) {
        const HMM_Vec3 to_target = HMM_SubV3(g.system.player_look_at, g.camera.position);
        const float    dist2     = HMM_DotV3(to_target, to_target);
        if (dist2 > 1e-6f) {
            const HMM_Vec3 dir = HMM_DivV3F(to_target, std::sqrt(dist2));
            const HMM_Vec3 def_fwd = HMM_V3(0.0f, 0.0f, -1.0f);
            const HMM_Vec3 axis = HMM_Cross(def_fwd, dir);
            const float sin2 = HMM_DotV3(axis, axis);
            if (sin2 > 1e-10f) {
                const float sin_a = std::sqrt(sin2);
                const float cos_a = std::clamp(HMM_DotV3(def_fwd, dir), -1.0f, 1.0f);
                const float angle = std::atan2(sin_a, cos_a);
                const HMM_Vec3 unit = HMM_DivV3F(axis, sin_a);
                g.camera.orientation = HMM_QFromAxisAngle_RH(unit, angle);
            }
            // else: dir parallel to default forward — identity is already correct.
        }
    }

    // ---- load the design-data tables (factions, guns, shields, armor, ----
    // ship classes). Order matters: ship_class::load_all resolves
    // default_shield strings against the shield/armor
    // tables, so those must be populated first. None of these touch the
    // GPU; they're pure data loaders, safe to run after system_def and
    // before any rendering init.
    // np-6al.1: one-time only — design-data catalogs + player state survive
    // a system switch, so the jump path must not reload or reset them.
    if (first_time) {
    faction::init();
    {
        const std::string ship_data = "assets/data/privateer_ship_data.json";
        gun::load_table(ship_data);
        shield::load_table(ship_data);
        armor::load_table(ship_data);
    }
    ship_class::load_all("assets/ships");
    // Data-driven combat AI logic tables (condition->maneuver). Same JSON
    // pattern as ship.json; see docs/ai_maneuver_system.md. Ships resolve
    // their table lazily by faction (fallback "default") on first combat tick.
    ai_brain::load_all("assets/ai");
    // Commodity catalog — pure data, same family as the tables above.
    // Trading screens (np-9cu.2) consume it; today it just proves the
    // canonical 1995 cargo list round-trips into the engine.
    commodity::load("assets/data/privateer_db/cargo.toml");
    // Per-base commodity pricing (np-9cu.2). Reads canonical category prices
    // + archetypes from assets/data, and each base's market archetype. Then
    // register the Commodity Exchange screen body via the np-9cu.4 hook seam.
    economy::load("assets/data/commodity_prices.json", "assets/bases");
    economy::register_exchange_screen();
    // Outfitting (np-9cu.3): hull + equipment prices, and the Ship Dealer +
    // Equipment screen bodies registered via the same np-9cu.4 hook seam.
    outfitting::load("assets/data/ship_prices.json",
                     "assets/data/equipment_prices.json");
    outfitting::register_screens();
    // Mission computer (np-zte.1): generated cargo-delivery + bounty jobs.
    // No data file to load (missions are generated from the commodity catalog
    // + galaxy graph); just register the screen body via the np-9cu.4 seam.
    // The per-base board is (re)generated on each dock, below.
    missions::register_screen();
    // Faction comm chatter table (np-ma2.1) — flavour lines surfaced on
    // the HUD when a kill moves reputation. Missing file is non-fatal.
    comm::load("assets/data/comm_lines.json");

    // Fresh-start player state. --system override flows through so the
    // recorded location matches the world we actually loaded. A --load
    // replaces this wholesale just below.
    g.player = player::new_game(g.system_name);

    // Save-load boot (np-ymp.1): if a slot was requested, deserialize it
    // over the fresh new_game() state. On failure (missing/corrupt/version)
    // load() leaves g.player untouched and we silently keep the new game —
    // never crash on a bad save. The speed-caps block below reads g.player
    // AFTER this, so a loaded ship/engine tier seeds the right caps.
    if (g.load_slot >= 0) {
        bool ok = false;
        if (g.load_slot == savegame::k_autosave_slot) {
            // --continue: load the most recent timestamped save by path.
            const auto saves = savegame::list_saves();
            if (!saves.empty()) ok = savegame::load(g.player, saves.front().path);
        } else {
            ok = savegame::load(g.player, g.load_slot);   // explicit slot
        }
        if (ok) {
            // If an explicit --system overrode the world we loaded, keep the
            // player's recorded location consistent with that world (the
            // saved current_system would otherwise disagree with reality).
            if (g.system_explicit) g.player.current_system = g.system_name;
            g.apply_health_pending = g.player.hp_valid;   // restore hull damage
            std::printf("[save] resumed — %lld cr, ship '%s', system '%s', base '%s'\n",
                        (long long)g.player.credits,
                        g.player.ship_class_name.c_str(),
                        g.player.current_system.c_str(),
                        g.player.last_docked_base.c_str());
        } else {
            std::fprintf(stderr, "[save] could not load slot %d — starting a new game\n",
                         g.load_slot);
        }
    }

    // --ship CLI override (dev): swap the player into an arbitrary hull AND
    // refit it to that class's stock guns, so the override flies a complete
    // ship rather than new_game's single laser. Applied to g.player HERE
    // (before the atlas resolve + ship spawn below read ship_class_name) so
    // the hull, the 3rd-person atlas, and the fitted guns all agree. No
    // override -> keep new_game's canonical Tarsus + single laser.
    if (!g_player_ship_override.empty()) {
        if (const ShipClass* k = ship_class::find(g_player_ship_override)) {
            g.player.ship_class_name = g_player_ship_override;
            g.player.gun_mounts.clear();
            for (const GunMount& m : k->default_guns)
                g.player.gun_mounts.push_back(gun::to_name(m.type));
            std::printf("[player] --ship override: flying '%s' with %zu stock guns\n",
                        g_player_ship_override.c_str(), g.player.gun_mounts.size());
        } else {
            std::fprintf(stderr, "[player] --ship '%s' not found, keeping '%s'\n",
                         g_player_ship_override.c_str(), g.player.ship_class_name.c_str());
        }
    }

    // Seed the camera's flight speed caps from the starting hull + engine
    // (np-9cu.3). Stock Tarsus @ engine L0 reproduces the 300/600 defaults.
    {
        const outfitting::SpeedCaps caps = outfitting::effective_speed_caps(g.player);
        g.camera.max_speed_cruise0 = caps.cruise0;
        g.camera.max_speed_cruise1 = caps.cruise1;
    }
    // --skip-title (dev): drop straight into flight, bypassing the menu.
    if (g.skip_title_at_boot) {
        g.show_title = false;
        std::printf("[dev] --skip-title: starting in flight\n");
    }
    }  // end if (first_time) — one-time tables + player state

    // Procedural skybox (B1): no saved PNGs — the cubemap is rendered on the
    // fly from the seed on the first frame (generate(), called in frame_cb
    // before the scene pass). init() only sets up draw-side resources.
    if (first_time && !g.sun.init())  { std::fprintf(stderr, "[main] sun init failed\n");  std::exit(1); }
    // Sky family is picked from a hash of the skybox seed (np-3dp). The
    // family drives BOTH the sun preset (so a purple skybox gets a
    // purple/blue/yellow sun, not a red one) AND the skybox tint targets
    // (so the same family also leans purple instead of orange).
    const uint64_t seed_hash = sky_family_hash(g.system.skybox_seed);
    const SkyFamily family    = sky_family_for_hash(seed_hash);
    const SkyFamilyConfig& cfg = k_sky_families[(int)family];
    // The JSON's preset is overridden by the family pick so the pairing
    // is canonical. The warn/fallback path still runs so unknown names
    // surface in stderr (logs stay useful for debugging).
    const std::string sun_name = sky_family_pick_sun(family, seed_hash);
    if (const StarPreset* sp = find_star_preset(sun_name)) {
        apply_star_preset(g.sun, *sp);
        g.system.star_preset = sun_name;
        std::printf("[stars] family=%d sun='%s' warmth=%.2f seed='%s'\n",
                    (int)family, sun_name.c_str(), cfg.warmth,
                    g.system.skybox_seed.c_str());
    } else {
        std::fprintf(stderr, "[main] sky family picked unknown preset '%s'\n",
                     sun_name.c_str());
    }
    if (!g.skybox.init(g.system.skybox_seed, /*face_res=*/4096, cfg.warmth,
                       cfg.target_a, cfg.target_b)) {
        std::fprintf(stderr, "[main] skybox init failed for seed '%s'\n",
                     g.system.skybox_seed.c_str());
        std::exit(1);
    }

    // Park the sun at the *centroid* of all nav points. This makes the
    // star sit in the middle of its system geographically — jump points
    // and stations end up arrayed around it like a real planetary disc,
    // and the lens flare / lighting cues all radiate outward from the
    // hub the player is meant to think of as 'the centre of Troy'
    // rather than the arbitrary world origin (0,0,0).
    //
    // Skipped when nav_points is empty (e.g. Crimson Veil, Hadrian's
    // Gate at time of writing) — those systems were authored against
    // the sun-at-origin convention and their hand-tuned spawn framings
    // would break if we silently moved the star.
    // A tiny dim sun parked far off-axis so meshes get readable directional
    // light without the bloom/lens-flare whitewashing the scene. Two things
    // opt in: --capture-clean (screenshot/atlas mode) and the per-system
    // "studio_lighting": true flag (debug/inspection scenes that still want
    // a live HUD). Both call this same helper so the lighting setup never
    // drifts between the two paths.
    auto apply_studio_sun = [](Sun& sun) {
        sun.position     = HMM_V3(-200000.0f, 150000.0f, 200000.0f);
        sun.radius       = 100.0f;
        sun.core_color   = HMM_V3(0.70f, 0.70f, 0.70f);
        sun.glow_color   = HMM_V3(0.06f, 0.06f, 0.06f);
        sun.corona_alpha = 0.0f;
        sun.gas_strength = 0.0f;
        sun.ray_strength = 0.0f;
    };

    if (g.capture_clean) {
        apply_studio_sun(g.sun);
        g.post.bloom_strength = 0.0f;
        g.post.flare_strength = 0.0f;
        std::printf("[main] capture-clean studio sun enabled; bloom/flare disabled\n");
    } else if (g.system.studio_lighting) {
        apply_studio_sun(g.sun);
        std::printf("[main] system studio_lighting=true: dim sun enabled\n");
    } else if (!g.system.nav_points.empty()) {
        HMM_Vec3 sum{0.0f, 0.0f, 0.0f};
        for (const auto& nav : g.system.nav_points) {
            sum = HMM_AddV3(sum, nav.position);
        }
        const float n = (float)g.system.nav_points.size();
        const HMM_Vec3 centroid = HMM_V3(sum.X / n, sum.Y / n, sum.Z / n);

        // Don't let the star swallow a nav point. If the centroid lands
        // within the sun's visible extent (core sphere + gas shell) of any
        // nav, shove it ~30 km in a stable pseudo-random direction and
        // re-check (a few tries). Seeded off the system name so a given
        // system always jitters the same way (no per-run flicker).
        const float clear_r = g.sun.radius * g.sun.gas_radius_mult + 8000.0f;
        auto overlaps_nav = [&](HMM_Vec3 p) {
            for (const auto& nav : g.system.nav_points)
                if (HMM_LenV3(HMM_SubV3(p, nav.position)) < clear_r) return true;
            return false;
        };
        HMM_Vec3 pos = centroid;
        if (overlaps_nav(pos)) {
            uint32_t st = 2166136261u;
            for (char c : g.system.name) st = (st ^ (uint8_t)c) * 16777619u;
            auto rnd01 = [&]() {
                st = st * 1664525u + 1013904223u;
                return (float)((st >> 8) & 0xFFFF) / 65535.0f;
            };
            for (int attempt = 0; attempt < 12 && overlaps_nav(pos); ++attempt) {
                const float az = rnd01() * 6.2831853f;
                const float el = (rnd01() - 0.5f) * 3.1415926f;
                const HMM_Vec3 dir = HMM_V3(std::cos(el) * std::cos(az),
                                            std::sin(el),
                                            std::cos(el) * std::sin(az));
                pos = HMM_AddV3(centroid, HMM_MulV3F(dir, 30000.0f));
            }
        }
        g.sun.position = pos;
        std::printf("[main] sun parked at nav-centroid (%.0f, %.0f, %.0f)%s from %d nav points\n",
                    g.sun.position.X, g.sun.position.Y, g.sun.position.Z,
                    (pos.X == centroid.X && pos.Y == centroid.Y && pos.Z == centroid.Z)
                        ? "" : " [jittered clear of a nav]",
                    (int)g.system.nav_points.size());
    }

    // Spin up one AsteroidField per entry in the system JSON. Each uses its
    // own seed so placement/sizes are deterministic per-sector.
    g.asteroid_fields.reserve(g.system.asteroid_fields.size());
    for (const auto& def : g.system.asteroid_fields) {
        AsteroidField f;
        f.center      = def.center;
        f.half_extent = def.half_extent;
        f.total_count = def.count;
        f.base_radius = def.base_radius;
        f.size_min    = def.size_min;
        f.size_max    = def.size_max;
        f.seed        = def.seed;
        if (!f.init()) {
            std::fprintf(stderr, "[main] asteroid field init failed\n");
            std::exit(1);
        }
        g.asteroid_fields.push_back(std::move(f));
    }

    if (first_time && !g.dust.init()) { std::fprintf(stderr, "[main] dust init failed\n"); std::exit(1); }
    if (first_time && !g.warp_streaks.init()) {
        std::fprintf(stderr, "[main] warp_streaks init failed\n"); std::exit(1);
    }
    if (first_time && !g.jump_gate.init()) {
        std::fprintf(stderr, "[main] jump_gate init failed\n"); std::exit(1);
    }

    // Mesh renderer + placed mesh instances. Load OBJs from disk now; any
    // file that fails to parse is skipped with a warning so one bad entry
    // doesn't take the whole system down.
    if (first_time && !g.mesh_render.init()) {
        std::fprintf(stderr, "[main] mesh renderer init failed\n");
        std::exit(1);
    }

    // Dear ImGui debug overlay. Must come after sg_setup() so the sokol
    // backend has a valid device/context to build its pipeline against.
    if (first_time) {
        debug_panel::init();
        sprite_light_editor::init();
        atlas_grid_viewer::init();
        sound_labeler::init();
        music_labeler::init();
        speech_labeler::init();
        sprite_generation_tool::init();
        mesh_orient_editor::init();
        navmap_auditor::init();
    }

    // Dev remote: HTTP control channel on 127.0.0.1. Lets external
    // tools (code puppy, curl, shell scripts) teleport the camera,
    // grab screenshots, and read state. Non-fatal if it can't bind.
    // Port 47001 picked to avoid collisions with common local dev
    // servers (3000, 5000, 8080, 8765, …).
    if (first_time) {
        dev_remote::start(47001);
    }
    dev_remote::publish_system_name(g.system.name.c_str());

    // Audio device + test samples. Failure is non-fatal by design —
    // audio::init logs and every later call no-ops, the game runs
    // silent (same resilience philosophy as dev_remote above).
    if (first_time) {
        audio::init();
        g.sfx_blip  = audio::load("assets/sfx/blip.wav");
        g.sfx_burst = audio::load("assets/sfx/burst.wav");
        g.sfx_hum   = audio::load("assets/sfx/hum.wav");
        // Gameplay SFX table + the (silent until throttled) engine-hum loop.
        sfx::load_all();
        // Dynamic music layer (np-m96): loads the rendered AdLib tracks
        // (gitignored, clean clones run silent). update() below drives the
        // state->track selection every frame.
        music::load_all();
    }
    for (const auto& pm_def : g.system.placed_meshes) {
        PlacedMesh pm;
        pm.name        = pm_def.obj_path;
        pm.position    = pm_def.position;
        pm.euler_deg   = pm_def.euler_deg;
        pm.body_tint     = pm_def.tint;
        pm.spec_amount   = pm_def.spec;
        pm.double_sided  = pm_def.double_sided;
        pm.clay_mode     = pm_def.clay_mode;
        pm.ambient_floor = pm_def.ambient_floor;
        pm.rim_strength  = pm_def.rim_strength;
        pm.atm_thickness = pm_def.atm_thickness;
        pm.atm_color     = pm_def.atm_color;
        pm.atm_strength  = pm_def.atm_strength;
        const std::string full = "assets/" + pm_def.obj_path;
        const uint64_t     t0 = stm_now();
        if (!load_obj_file(full, pm.mesh) || !pm.mesh.upload()) {
            std::fprintf(stderr, "[main] skipping placed mesh '%s'\n", full.c_str());
            continue;
        }
        const double load_ms = stm_ms(stm_since(t0));

        // Resolve scale. `length_meters` is the preferred path: it uses
        // the mesh's measured bounding box so that 1 scene unit = 1 metre
        // regardless of whatever units the source tool exported. Raw
        // `scale` is still accepted for legacy / non-ship placements.
        const float ext = pm.mesh.longest_extent();
        if (pm_def.length_meters > 0.0f && ext > 1e-6f) {
            pm.scale = pm_def.length_meters / ext;
        } else if (pm_def.scale > 0.0f) {
            pm.scale = pm_def.scale;
        } else {
            pm.scale = 1.0f;
        }

        // Textures now live on `pm.mesh.materials`, populated by the OBJ
        // loader from the `<stem>.materials.json` sidecar. The only
        // exception is procedurally-generated planet textures, which are
        // injected into the mesh's first (usually only) material's
        // diffuse slot so the rest of the pipeline treats it like any
        // other material.
        if (!pm_def.texture_preset.empty() && !pm.mesh.materials.empty()) {
            PlanetTexture pt = make_planet_texture(pm_def.texture_preset);
            Material& m0 = pm.mesh.materials[0];
            if (m0.diffuse.valid) {   // replace any prior-loaded slot
                sg_destroy_view(m0.diffuse.view);
                sg_destroy_image(m0.diffuse.image);
            }
            m0.diffuse.image = pt.image;
            m0.diffuse.view  = pt.view;
            m0.diffuse.valid = true;
        }

        // Count material slots actually populated.
        int md = 0, ms = 0, mg = 0, mn = 0;
        for (const auto& mat : pm.mesh.materials) {
            md += mat.diffuse.valid ? 1 : 0;
            ms += mat.spec.valid    ? 1 : 0;
            mg += mat.glow.valid    ? 1 : 0;
            mn += mat.normal.valid  ? 1 : 0;
        }
        std::printf("[main]   mesh '%s' loaded in %.1f ms: %d tris  "
                    "submeshes=%zu  mats=%zu (D%d/S%d/G%d/N%d)\n",
                    full.c_str(), load_ms, pm.mesh.index_count / 3,
                    pm.mesh.submeshes.size(),
                    pm.mesh.materials.size(),
                    md, ms, mg, mn);
        g.placed_meshes.push_back(std::move(pm));
    }

    // --- sprite renderer (np-0kv) ----------------------------------------
    // One-time init, then iterate the system JSON's `placed_sprites`
    // array. Each entry resolves to an `assets/<stem>.png` hull file plus
    // optional `<stem>_lights.png` static overlay and `<stem>.lights.json`
    // animated-light sidecar (authored by the light editor, np-0kv.5).
    // SpriteArt is cached by stem so repeated references share one GPU
    // texture upload.
    if (first_time && !g.sprite_render.init()) {
        std::fprintf(stderr, "[main] sprite renderer init failed\n");
        std::exit(1);
    }
    // Load bolt sprite art (per-GunType animated frames from assets/bolts/).
    // Flattened into bolt_textures with per-type offsets for the renderer.
    if (first_time) {
        g.bolt_art.load("assets/bolts");
        g.bolt_art.flatten(g.bolt_textures, g.bolt_tex_offsets);
    }
    for (const auto& sd : g.system.placed_sprites) {
        const std::string stem_full = "assets/" + sd.sprite;  // e.g. assets/sprites/mining_base

        // Cache lookup — load once per unique stem even if referenced
        // multiple times. emplace returns {iter, inserted}.
        auto [it, inserted] = g.sprite_art.try_emplace(sd.sprite, SpriteArt{});
        if (inserted) {
            if (!load_sprite_art(stem_full, it->second)) {
                std::fprintf(stderr, "[main] skipping placed sprite '%s'\n",
                             sd.sprite.c_str());
                g.sprite_art.erase(it);
                continue;
            }
        }

        SpriteObject s{};
        s.art        = &it->second;
        s.position   = sd.position;
        s.world_size = sd.length_meters;
        s.tint       = HMM_V4(1.0f, 1.0f, 1.0f, 1.0f);
        // Placed environmental sprites (bases / stations / planets) are
        // big and you fly AROUND them, so they billboard toward the
        // camera POSITION, not the view plane (np-3dp.23) — they turn to
        // face you as you pass instead of staying a flat front-facing
        // card.
        s.face_camera_position = true;
        // Lights are loaded once into SpriteArt by load_sprite_art(); copy
        // them onto the instance so the F2 editor can mutate per-instance
        // lists without touching the shared art (and so future per-instance
        // overrides — e.g. a damaged ship missing a nav light — drop in
        // without restructuring storage).
        s.lights = it->second.light_spots;
        g.placed_sprites.push_back(s);
    }

    for (const auto& sd : g.system.placed_ship_sprites) {
        // np-wdk: route every system-JSON ship atlas stem through the central
        // resolver so it loads the sprites_3d/ variant. The cache key is the
        // RESOLVED stem so later finds (encounter spawn, talon debug) agree.
        const std::string atlas_stem = resolve_ship_atlas_stem(sd.atlas);
        auto [it, inserted] = g.ship_sprite_atlases.try_emplace(atlas_stem, ShipSpriteAtlas{});
        if (inserted && !load_ship_sprite_atlas(atlas_stem, it->second, g.sprite_art)) {
            std::fprintf(stderr, "[main] skipping ship sprite atlas '%s'\n", atlas_stem.c_str());
            g.ship_sprite_atlases.erase(it);
            continue;
        }

        ShipSpriteObject s{};
        s.atlas          = &it->second;
        s.position       = sd.position;
        // Ships scaled by k_ship_size_scale (world_scale.h). Both the
        // rendered billboard AND the hit-radius derived from world_size
        // (see ship::hit_radius_m) grow together, so visual size and
        // collision stay locked in sync — the player can target what
        // they see and trust the bullet will register.
        s.world_size     = sd.length_meters * world_scale::k_ship_size_scale;
        s.lights_enabled = sd.lights_enabled;
        s.tint           = HMM_V4(1.0f, 1.0f, 1.0f, 1.0f);

        // Motion: convert deg/s to rad/s once at scene-load time so the
        // hot path doesn't redo the multiplication every frame. orientation
        // defaults to identity (nose along world +Z) — adding an authored
        // initial yaw is a one-quaternion-multiply away if a future scene
        // needs it.
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        s.angular_velocity = HMM_MulV3F(sd.angular_velocity_deg, kDegToRad);
        s.forward_speed    = sd.forward_speed;

        g.placed_ship_sprites.push_back(s);
    }

    // Resolve (loading if necessary) the player's own hull atlas so the
    // 3rd-person autopilot camera has a sprite to render. Keyed the same way
    // as NPC atlases (resolve_ship_atlas_stem -> sprites_3d variant). Done
    // every system build because g.ship_sprite_atlases is per-system; the
    // player persists but its atlas pointer must be re-resolved each time.
    {
        // Hull atlas follows the player's actual class (np-3dp.25) — stock
        // Tarsus by default, --ship override or a ship-swap save otherwise.
        const std::string pc = g.player.ship_class_name.empty()
                             ? std::string("tarsus") : g.player.ship_class_name;
        const std::string pstem = resolve_ship_atlas_stem("ships/" + pc + "/atlas_manifest");
        auto [it, inserted] = g.ship_sprite_atlases.try_emplace(pstem, ShipSpriteAtlas{});
        if (inserted && !load_ship_sprite_atlas(pstem, it->second, g.sprite_art)) {
            std::fprintf(stderr, "[orbit] player atlas '%s' failed to load - 3rd-person hull hidden\n",
                         pstem.c_str());
            g.ship_sprite_atlases.erase(it);
            g.player_atlas = nullptr;
        } else {
            g.player_atlas = &it->second;
            std::printf("[orbit] player hull atlas ready: %s\n", pstem.c_str());
        }
    }

    // ---- build the Ship array (one per placed sprite) -------------------
    // Each ShipSpriteObject above is paired with a Ship that owns class /
    // faction / health / behaviour. Class lookup: explicit `ship_class`
    // field on the JSON entry first, otherwise derive from the atlas path
    // by extracting the second slash-segment ("ships/talon/atlas_manifest"
    // -> "talon") — every existing scene happens to follow that
    // convention, so we get class binding for free.
    // Player ship FIRST so it lands in registry slot 0 — the well-known
    // ShipRegistry::player_handle() convention. No class, no sprite —
    // pose is filled in each frame from g.camera before perception runs.
    // Adding the player to g.ships unifies the perception + AI inner
    // loops on a single "all ships" iteration; without this every later
    // layer would need a special case for "target the player".
    // np-6al.1: the player Ship is spawned ONCE (slot 0) and PERSISTS across
    // a system switch — only its pose is re-seeded to the new spawn (below).
    if (first_time) {
    g.ships.spawn(ship::spawn_player());

    // Fit the player's persistent loadout onto the freshly-spawned slot-0
    // Ship (np-3dp.25). Class + guns come straight from g.player
    // (new_game's stock Tarsus + single laser, a loaded save's hull, or a
    // --ship override applied above), so the live ship can never drift from
    // the player state / equipment shop. apply_player_loadout heals to full
    // and mounts exactly the guns named in p.gun_mounts.
    {
        Ship& player = *g.ships.player();
        apply_player_loadout(player, g.player);
        // A loaded save may start docked/landed. Apply its HP snapshot
        // immediately after the boot loadout heals the live ship, or the
        // base repair screen will quote a magically-full hull until the
        // player launches and the Flight-frame pending apply runs. UI
        // should not require a ceremonial space lap. Ridiculous.
        apply_pending_player_health_snapshot(player);
        std::printf("[player] equipped: klass=%s mounts=%zu shield F/A/P/St=%.0f/%.0f/%.0f/%.0f "
                    "armor F/A/P/St=%.0f/%.0f/%.0f/%.0f energy=%.0f\n",
                    player.klass ? player.klass->name.c_str() : "<null>",
                    player.mounts.size(),
                    player.shield_fore_cm, player.shield_aft_cm,
                    player.shield_port_cm, player.shield_starboard_cm,
                    player.armor_fore_cm, player.armor_aft_cm,
                    player.armor_port_cm, player.armor_starboard_cm,
                    player.energy_gj);
    }
    }  // end if (first_time) — one-time player Ship spawn + loadout

    // Re-seed the player pose to the new system's spawn every build. The
    // player persists across a jump; only their position/orientation move.
    if (Ship* p = g.ships.player()) {
        p->position    = g.camera.position;
        p->orientation = g.camera.orientation;
    }

    int n_with_behavior = 0;
    int n_inert         = 0;
    for (size_t i = 0; i < g.system.placed_ship_sprites.size(); ++i) {
        const auto& sd = g.system.placed_ship_sprites[i];
        if (i >= g.placed_ship_sprites.size()) break;   // atlas-load failure earlier

        // Inert mode — visual-only mannequin for atlas/capture scenes.
        // alive=false makes perception/AI/firing/projectile-collision all
        // skip this Ship, so the sprite renders but the simulation
        // pretends it isn't there. The Ship::sprite pointer is the only
        // sprite linkage (deque storage keeps it stable); registry slot
        // order no longer needs to mirror sprite order.
        if (sd.inert) {
            Ship mannequin{};
            mannequin.sprite = &g.placed_ship_sprites[i];
            mannequin.alive  = false;
            g.ships.spawn(std::move(mannequin));
            ++n_inert;
            continue;
        }

        std::string class_name = sd.ship_class;
        if (class_name.empty()) {
            // sd.atlas looks like "ships/<class>/atlas_manifest" (or with
            // a .json suffix); pull out the <class> segment.
            const std::string& a = sd.atlas;
            auto slash1 = a.find('/');
            if (slash1 != std::string::npos) {
                auto slash2 = a.find('/', slash1 + 1);
                if (slash2 != std::string::npos) {
                    class_name = a.substr(slash1 + 1, slash2 - slash1 - 1);
                }
            }
        }
        const ShipClass* klass = ship_class::find(class_name);
        if (!klass) {
            std::fprintf(stderr, "[main] no ship_class for atlas '%s' (derived '%s'); "
                                 "sprite will run on legacy motion only\n",
                         sd.atlas.c_str(), class_name.c_str());
            // Spawn a placeholder Ship anyway so the sprite still has an
            // owner. Behavior is None, so it does nothing — the existing
            // motion path drives the sprite as today.
            Ship placeholder{};
            placeholder.sprite = &g.placed_ship_sprites[i];
            g.ships.spawn(std::move(placeholder));
            continue;
        }

        Ship inst = ship::spawn(*klass);
        inst.sprite = &g.placed_ship_sprites[i];

        // Faction override — same Talon hull can spawn as Pirate (the
        // class default), Militia, or Retro. Empty string keeps the
        // class default.
        if (!sd.faction_override.empty()) {
            const Faction f = faction::from_name(sd.faction_override);
            if (f != Faction::Count) {
                inst.faction = f;
            } else {
                std::fprintf(stderr, "[main] unknown faction '%s' on '%s' — using class default\n",
                             sd.faction_override.c_str(), class_name.c_str());
            }
        }

        // Translate the JSON behaviour string into the Ship enum.
        if (sd.behavior_kind == "pursue_target") {
            inst.behavior.kind       = ShipBehavior::PursueTarget;
            inst.behavior.target_pos = sd.behavior_target_pos;
            ++n_with_behavior;
        } else if (sd.behavior_kind.empty() || sd.behavior_kind == "none") {
            inst.behavior.kind = ShipBehavior::None;
        } else {
            std::fprintf(stderr, "[main] unknown ship behavior '%s' on '%s' "
                                 "— treating as none\n",
                         sd.behavior_kind.c_str(), class_name.c_str());
        }

        // Translate the optional `ai` JSON block into the AI state.
        // ai_enabled wins over behavior — ship_ai::tick will overwrite
        // it every frame.
        if (sd.ai_enabled) {
            inst.ai.enabled = true;
            const AIState seeded = sd.ai_initial_state.empty()
                ? AIState::Idle
                : ship_ai::from_name(sd.ai_initial_state);
            inst.ai.state = (seeded == AIState::Count) ? AIState::Idle : seeded;
            inst.ai.has_patrol_anchor = sd.ai_has_patrol_anchor;
            inst.ai.patrol_anchor     = sd.ai_patrol_anchor;

            // Cowards (merchants etc.) auto-get their spawn position as
            // a patrol_anchor when one isn't explicitly set in JSON.
            // Used by the Flee state's home-tether: the ship still flees
            // from threats but is pulled back toward this point as it
            // gets further from home, so merchants don't fly off to
            // infinity. Standard-personality ships (pirates, militia)
            // don't get the auto-anchor — they're free hunters.
            if (klass->personality == AIPersonality::Coward
                && !inst.ai.has_patrol_anchor) {
                inst.ai.patrol_anchor     = sd.position;
                inst.ai.has_patrol_anchor = true;
            }
        }

        g.ships.spawn(std::move(inst));
    }
    std::printf("[ship] %zu instances spawned (%d with active behavior, %d inert)\n",
                g.ships.size(), n_with_behavior, n_inert);

    // ---- encounter director (np-ma2.3) ---------------------------------
    // Pre-load the sprite atlas for every ship class an encounter rule can
    // spawn. The placed-fleet loop above only loaded atlases for ships it
    // actually placed; a rule may draw a class that isn't in the static
    // scene (a roaming Orion in an otherwise Talon-only system). Without
    // this, encounter_spawn would hit "atlas not loaded" and skip. Same
    // try_emplace + load pattern as the placed-sprite loop.
    // Collect every ship class referenced by any nav's wcnews encounter
    // table (the canonical per-nav spawn model) plus any legacy rule mixes,
    // and preload its atlas so populate_on_entry's spawns resolve.
    auto preload_class = [&](const std::string& cls) {
        if (cls.empty()) return;
        const std::string atlas_stem =
            resolve_ship_atlas_stem("ships/" + cls + "/atlas_manifest");
        auto [it, inserted] = g.ship_sprite_atlases.try_emplace(atlas_stem, ShipSpriteAtlas{});
        if (inserted && !load_ship_sprite_atlas(atlas_stem, it->second, g.sprite_art)) {
            std::fprintf(stderr, "[encounter] could not preload atlas '%s' "
                         "(class '%s' will spawn nothing)\n", atlas_stem.c_str(), cls.c_str());
            g.ship_sprite_atlases.erase(it);
        }
    };
    for (const NavPointDef& nav : g.system.nav_points)
        for (const EncounterGroupDef& grp : nav.encounters)
            for (const EncounterMemberDef& m : grp.members) preload_class(m.ship_class);
    for (const EncounterRuleDef& rule : g.system.encounters)
        for (const EncounterWeight& cw : rule.classes) preload_class(cw.name);

    // #14: a mission-driven forced wing can field a faction with NO native
    // presence in this system (a Pirate bounty issued in Confed space), whose
    // fallback fighter won't appear in the nav/rule tables above. Preload the
    // typical fighter classes for every active mission's target_faction —
    // Scout/Patrol random spawns now use target_faction too — so the spawn
    // always resolves to an atlas.
    for (const ActiveMission& am : g.player.missions) {
        const missions::MissionType mt = (missions::MissionType)am.type;
        if (mt != missions::MissionType::Attack &&
            mt != missions::MissionType::DefendBase &&
            mt != missions::MissionType::Bounty &&
            mt != missions::MissionType::Scout &&
            mt != missions::MissionType::Patrol) continue;
        const Faction fac = faction::from_name(am.target_faction);
        if (fac == Faction::Count) continue;
        for (const std::string& cls : encounters::faction_fighter_classes(g.system, fac))
            preload_class(cls);
    }

    // Hand the live ship registry + player reputation to the threat oracle so
    // threat::hostiles_near() (autopilot gate) queries the real world.
    encounters::init(g.system);   // legacy rule director (inert for nav-table systems)
    threat::set_world(&g.ships, &g.player.rep);

    // Hand the civilian AI the nav-point lattice it travels between (lane
    // traffic) and flees toward (gates/bases). Bases + jump points are
    // marked dock_or_gate so a fleeing/departing ship can actually exit.
    {
        std::vector<ship_ai::NavWaypoint> wps;
        wps.reserve(g.system.nav_points.size());
        for (const NavPointDef& n : g.system.nav_points) {
            const bool exit = (n.kind == "jump" || n.kind == "station" ||
                               n.kind == "planet" || n.dockable);
            wps.push_back({ n.position, exit });
        }
        ship_ai::set_nav_waypoints(wps);
    }

    // wcnews encounter model: roll each nav's table ONCE and spawn its wave
    // now (system entry). No continuous refill — see encounters.h. The
    // player camera is already at player_start by this point.
    encounters::populate_on_entry(g.system, g.camera.position, g.sun.position,
                                  encounter_spawn);

    // Fly-by-wire defaults OFF. Player toggles it with SPACE. This is much
    // friendlier for tools/capture scripts and prevents the camera from
    // drifting because the OS cursor happened to be off-centre. Amazing how
    // not fighting the tooling makes the tooling less cursed.
    sapp_show_mouse(true);
    g.fly_by_wire = false;

    g.last_frame_ticks = stm_now();
    g.last_fps_ticks   = g.last_frame_ticks;
    std::printf("[new_privateer] backend=%d, '%s' loaded\n",
                (int)sg_query_backend(), g.system.name.c_str());

    // --dev-land <base>: skip the flight + autodock and boot straight into
    // the base concourse (np-9cu.4 dev affordance). The transition is
    // applied at the top of the first frame_cb, which calls
    // base_screens::enter(last_docked_base) for us.
    if (first_time && !g.dev_land_base.empty()) {
        g.player.last_docked_base = g.dev_land_base;
        g.player.docked           = true;
        game_state::request_mode(g.game, GameMode::Landed);
        std::printf("[new_privateer] --dev-land '%s' — booting into Landed\n",
                    g.dev_land_base.c_str());
    }
    // Resume a docked save AT its base (np-3dp.21): if --load/--continue
    // restored a save that was docked, boot straight into that base's
    // concourse rather than adrift in space. Mirrors --dev-land. Skipped
    // when --dev-land already chose a destination.
    else if (first_time && g.load_slot >= 0 &&
             g.player.docked && !g.player.last_docked_base.empty()) {
        game_state::request_mode(g.game, GameMode::Landed);
        std::printf("[save] resumed docked save -> booting into Landed at %s\n",
                    g.player.last_docked_base.c_str());
    }
}

// ---- system teardown (np-6al.1) ---------------------------------------------
//
// The teardown half of a runtime jump. Frees EVERY GPU/scene resource the
// scene-build path allocated, in dependency order (holders before the things
// they point into), so a switch never leaks a sokol image/view/buffer and the
// player can hop systems indefinitely. What survives: the player (slot-0 Ship
// + PlayerState), and the long-lived renderers (sun geometry, dust, mesh +
// sprite renderers) which are system-independent and re-used by the next
// build. Audited resource list (each paired with its sg_destroy):
//   * Skybox          — cubemap image/view + sampler + vbuf/ibuf + shader/pipeline
//   * AsteroidField[] — per-variant instance buffers + shader/pipeline
//   * PlacedMesh[]    — vbuf/ibuf + every Material's diffuse/spec/glow/normal
//                       image+view (Mesh::destroy walks the material table)
//   * SpriteArt cache — hull/lights image+view+sampler; this also owns the GPU
//                       textures behind every ShipSpriteAtlas (the atlases just
//                       hold SpriteArt* into this cache), so clearing it frees
//                       both placed-sprite and ship-sprite imagery in one pass
//   * ShipSpriteAtlas map + placed_ship_sprites + placed_sprites — CPU-side
//                       holders, cleared after the GPU textures they reference
// Director bookkeeping + transient FX vectors are pure CPU and just cleared.
void unload_current_system() {
    const size_t n_fields  = g.asteroid_fields.size();
    const size_t n_sprites = g.placed_sprites.size();
    const size_t n_atlases = g.ship_sprite_atlases.size();
    const size_t n_meshes  = g.placed_meshes.size();

    // 1. Director state first (ids only — the registry owns the ships), then
    //    drop every NPC / encounter ship, keeping the player in slot 0.
    encounters::shutdown();
    const size_t n_ships = g.ships.clear_except_player();

    // 2. Transient FX + per-frame scratch — pure CPU vectors.
    g.projectiles.clear();
    g.missiles.clear();
    g.explosions.clear();
    g.shield_flashes.clear();
    g.armor_flashes.clear();
    g.frame_sprites.clear();

    // 3. CPU-side sprite holders BEFORE the GPU textures they point into.
    //    (ShipSpriteAtlas frames hold SpriteArt* into g.sprite_art; the
    //    ShipSpriteObjects hold ShipSpriteAtlas*; clear inner-most first.)
    g.placed_ship_sprites.clear();
    g.free_sprite_slots.clear();
    g.ship_sprite_atlases.clear();
    g.placed_sprites.clear();

    // 4. GPU resources — every sg_make_* in the scene path gets its paired
    //    sg_destroy here. sokol treats destroy-on-invalid as a no-op, so a
    //    first-time/empty container is harmless.
    for (auto& [_, art] : g.sprite_art) art.destroy();   // sprite + ship-atlas imagery
    g.sprite_art.clear();
    for (auto& pm : g.placed_meshes) pm.mesh.destroy();   // vbuf/ibuf + material textures
    g.placed_meshes.clear();
    for (auto& f : g.asteroid_fields) f.destroy();        // instance buffers + pipeline
    g.asteroid_fields.clear();
    g.skybox.destroy();                                   // cubemap/view/sampler/buffers/pipeline

    // 5. Targeting / traversal state that referenced the old system.
    g.selected_nav     = -1;
    g.player_target_id = 0;
    g.show_navmap      = false;
    g.autopilot = Autopilot{};
    g.docking   = Docking{};

    std::printf("[system] unloading %s: freed %zu asteroid fields, %zu sprites, "
                "%zu ship atlases, %zu meshes, %zu NPC ships\n",
                g.system.name.c_str(), n_fields, n_sprites, n_atlases,
                n_meshes, n_ships);
}

// ---- load + (re)build a system (np-6al.1) -----------------------------------
//
// The single entry point both startup and the runtime jump go through. On a
// switch (first_time=false): validate the new system loads BEFORE tearing down
// the old one (so a typo'd id leaves the current world intact), unload, then
// build. The player's PlayerState is untouched the whole time — only
// current_system is updated to the new world.
bool load_and_build_system(const std::string& id, bool first_time) {
    if (!first_time) {
        std::printf("[system] === switching '%s' -> '%s' ===\n",
                    g.player.current_system.c_str(), id.c_str());
    }

    // Resolve the per-system JSON path through the galaxy catalog when the id
    // is a known galaxy id; otherwise fall back to treating `id` as a
    // system name/path directly (so --system <file> + legacy flows still work).
    std::string load_arg = id;
    if (const galaxy::SystemEntry* e = g.galaxy.find(id); e && !e->json_path.empty()) {
        load_arg = e->json_path;
    }

    auto loaded = load_system(load_arg);
    if (!loaded) {
        std::fprintf(stderr, "[system] load of '%s' failed\n", id.c_str());
        return false;   // nothing torn down yet — current world still intact
    }

    if (!first_time) unload_current_system();
    g.system = std::move(*loaded);
    build_system_scene(first_time);

    g.player.current_system = id;
    g.system_loaded = true;
    g.system_name   = id;
    dev_remote::publish_system_name(g.system.name.c_str());
    sapp_set_window_title(("new_privateer — " + g.system.name).c_str());
    std::printf("[system] built %s: %zu asteroid fields, %zu sprites, "
                "%zu ship atlases, %zu nav points, registry=%zu ships "
                "(player persists: %lld cr, ship '%s')\n",
                g.system.name.c_str(), g.asteroid_fields.size(),
                g.placed_sprites.size(), g.ship_sprite_atlases.size(),
                g.system.nav_points.size(), g.ships.size(),
                (long long)g.player.credits, g.player.ship_class_name.c_str());
    return true;
}

// ---- jump execution (np-6al.3) ----------------------------------------------
//
// How long the hyperspace flash holds before the world swaps. Short enough
// to feel snappy, long enough that the white flash reads as a transition
// rather than a single-frame glitch.
static constexpr float k_jump_loading_s = 0.9f;

// Perform the queued jump. Called from frame_cb once the Loading beat has
// elapsed. Reuses load_and_build_system (np-6al.1) so the player's ship /
// cargo / credits / reputation persist exactly as the dev --goto path proves;
// then drops the player at the reciprocal gate (galaxy::jump_target gave us
// the arrival nav), nudged a hair toward system center and facing INTO the
// system (away from the gate) with a modest inward drift. Any failure still
// returns to Flight so the player is never stranded on the Loading screen.
void execute_jump() {
    const std::string dest = g.pending_jump_system;
    const std::string nav  = g.pending_jump_nav;
    g.pending_jump_system.clear();
    g.pending_jump_nav.clear();

    if (!load_and_build_system(dest, /*first_time=*/false)) {
        std::fprintf(stderr, "[jump] build of '%s' failed — aborting jump\n",
                     dest.c_str());
        game_state::request_mode(g.game, GameMode::Flight);
        return;
    }

    // Locate the arrival gate by name in the freshly-built destination.
    const NavPointDef* gate = nullptr;
    for (const auto& n : g.system.nav_points) {
        if (n.name == nav) { gate = &n; break; }
    }
    if (gate) {
        // "Into the system" = toward the origin/sun. A gate sitting exactly
        // at the origin (none today) degenerates to the camera's default
        // forward (-Z) rather than a NaN direction.
        HMM_Vec3    into = HMM_MulV3F(gate->position, -1.0f);
        const float len  = HMM_LenV3(into);
        into = (len > 1e-3f) ? HMM_DivV3F(into, len) : HMM_V3(0.0f, 0.0f, -1.0f);

        constexpr float k_arrival_offset = 1500.0f;  // clear of the gate hull
        constexpr float k_arrival_speed  = 60.0f;     // gentle inward coast
        g.camera.position = HMM_AddV3(gate->position,
                                      HMM_MulV3F(into, k_arrival_offset));
        g.camera.velocity = HMM_MulV3F(into, k_arrival_speed);

        // Orient toward `into` (shortest arc from default forward -Z), same
        // construction build_system_scene uses for player_look_at.
        const HMM_Vec3 def_fwd = HMM_V3(0.0f, 0.0f, -1.0f);
        const HMM_Vec3 axis    = HMM_Cross(def_fwd, into);
        const float    sin2    = HMM_DotV3(axis, axis);
        if (sin2 > 1e-10f) {
            const float sin_a = std::sqrt(sin2);
            const float cos_a = std::clamp(HMM_DotV3(def_fwd, into), -1.0f, 1.0f);
            const float angle = std::atan2(sin_a, cos_a);
            g.camera.orientation = HMM_QFromAxisAngle_RH(HMM_DivV3F(axis, sin_a), angle);
        } else {
            g.camera.orientation = HMM_Q(0.0f, 0.0f, 0.0f, 1.0f);
        }
        // Arrive coasting, not cruising — no leftover spool from the origin.
        g.camera.cruise_target = 0.0f;
        g.camera.cruise_level  = 0.0f;

        std::printf("[jump] arrived in %s at gate '%s' — pos %.0f,%.0f,%.0f, "
                    "facing into-system\n",
                    g.system.name.c_str(), nav.c_str(),
                    g.camera.position.X, g.camera.position.Y, g.camera.position.Z);
    } else {
        std::fprintf(stderr, "[jump] arrival nav '%s' not found in %s — "
                     "spawning at default player_start\n",
                     nav.c_str(), g.system.name.c_str());
    }

    // Sync the slot-0 player Ship pose to the new camera immediately so the
    // first Flight frame's perception/AI don't see a one-frame teleport
    // ghost (the registry was just rebuilt around the persisted player).
    if (Ship* p = g.ships.player()) {
        p->position = g.camera.position;
    }

    game_state::request_mode(g.game, GameMode::Flight);
}

// ---- debug ship spawn/despawn (registry smoke test, np-eag.1) ---------------
//
// Consumes the deferred requests set by debug_panel's buttons. Runs at
// the top of frame_cb, BEFORE any system iterates the registry, so a
// spawn/despawn never mutates storage mid-frame. This is the first
// runtime exercise of the slot-map machinery — real encounter
// generation (np-ma2.3) will follow the same recipe:
//
//   1. claim a sprite slot (reuse a freed one, else append — deque
//      growth keeps every existing Ship::sprite pointer valid),
//   2. ship::spawn(klass) for class-derived health/mounts/AI,
//   3. registry.spawn(std::move(ship)) -> handle.
//
// Despawn is the reverse: park the sprite slot (atlas=nullptr makes
// renderer + integrator skip it) and registry.despawn(handle), which
// bumps the slot generation so any stale handle resolves to nullptr.
void apply_ship_debug_requests() {
    // --dev-kill-at <secs>: fire the death test once, in Flight, after the
    // clock crosses the threshold. One-shot (we disarm by clearing the
    // threshold) so respawn doesn't immediately re-die.
    if (g.dev_kill_at_s >= 0.0f && g.game.mode == GameMode::Flight) {
        g.dev_run_clock_s += (float)sapp_frame_duration();
        if (g.dev_run_clock_s >= g.dev_kill_at_s) {
            g.ship_debug.kill_player = true;
            g.dev_kill_at_s = -1.0f;   // disarm
            std::printf("[dev] --dev-kill-at fired at %.1fs\n", g.dev_run_clock_s);
        }
    }

    // np-ma2.1 reputation test: run the player-kill rep logic against the
    // chosen victim faction directly. Deterministic — surfaces the same
    // rep deltas / stance flips / taunts as a real projectile kill would.
    if (g.ship_debug.sim_kill_faction >= 0) {
        const Faction vf = (Faction)g.ship_debug.sim_kill_faction;
        g.ship_debug.sim_kill_faction = -1;
        std::printf("[debug] simulate player kill of %s\n", faction::to_name(vf));
        comm::report_player_kill(g.player, vf);
        // advance matching bounties, gated to the active system (np-zte.1/#15)
        missions::on_target_destroyed(g.player, vf, g.player.current_system);
    }

    // --dev-missions: one-shot — accept a handful of generated jobs so the
    // navmap MISSION STATUS panel has real cards to inspect. Dev aid only;
    // fires once we're actually in Flight (galaxy + system are ready).
    if (g.dev_seed_missions && g.game.mode == GameMode::Flight) {
        g.dev_seed_missions = false;
        g.player.has_jump_drive = true;   // so cross-system jobs are acceptable
        std::string base = g.player.last_docked_base;
        if (base.empty())
            for (const NavPointDef& n : g.system.nav_points)
                if (!n.base_id.empty()) { base = n.base_id; break; }
        const std::vector<missions::Mission> offers = missions::generate(
            base, g.player.current_system, g.galaxy, 12345u,
            missions::MissionSource::Computer);
        int taken = 0;
        for (const missions::Mission& m : offers) {
            if (taken >= 6) break;
            if (missions::accept(g.player, m, /*capacity=*/1000)) ++taken;
        }
        std::printf("[dev] --dev-missions: accepted %d of %d generated jobs\n",
                    taken, (int)offers.size());
        g.show_navmap = true;             // pop the map so the cards are visible
    }

    if (g.ship_debug.spawn_talon) {
        g.ship_debug.spawn_talon = false;

        const ShipClass* klass = ship_class::find("talon");
        // np-wdk: resolve to match the key the preload loop cached under.
        auto atlas_it = g.ship_sprite_atlases.find(
            resolve_ship_atlas_stem("ships/talon/atlas_manifest"));
        if (!klass || atlas_it == g.ship_sprite_atlases.end()) {
            std::fprintf(stderr, "[debug_spawn] talon class or atlas not loaded "
                                 "in this system; spawn ignored\n");
        } else {
            // Claim a sprite slot (reuse parked, else append — see helper).
            const size_t slot = claim_sprite_slot();
            ShipSpriteObject& spr = g.placed_ship_sprites[slot];
            spr.atlas      = &atlas_it->second;
            // 500 m ahead of the camera — close enough to see immediately,
            // far enough to not clip through the cockpit.
            spr.position   = HMM_AddV3(g.camera.position,
                                       HMM_MulV3F(g.camera.forward(), 500.0f));
            spr.world_size = 80.0f * world_scale::k_ship_size_scale;  // matches troy.json talons

            Ship inst   = ship::spawn(*klass);
            inst.sprite = &spr;
            inst.ai.enabled = true;             // joins the brawl like its JSON kin
            const ShipHandle h = g.ships.spawn(std::move(inst));
            std::printf("[debug_spawn] talon spawned: handle {%u, %u}, sprite slot %zu\n",
                        h.index, h.generation, slot);
        }
    }

    if (g.ship_debug.despawn_target) {
        g.ship_debug.despawn_target = false;

        const ShipHandle h = g.ships.find_handle_by_id(g.player_target_id);
        const Ship* t = g.ships.get(h);
        if (!t) {
            std::printf("[debug_despawn] no valid target locked (T-cycle first)\n");
        } else if (t->is_player) {
            std::printf("[debug_despawn] refusing to despawn the player, nice try\n");
        } else {
            // Park the sprite slot for reuse (atlas=nullptr is the
            // "unoccupied" marker the renderer + integrator skip).
            free_sprite_slot(t->sprite);
            g.ships.despawn(h);
            g.player_target_id = 0;
            // Prove the staleness contract: the handle we just retired
            // must now resolve to nullptr. Cheap, loud if ever broken.
            if (g.ships.get(h) != nullptr) {
                std::fprintf(stderr, "[debug_despawn] BUG: stale handle still resolves!\n");
            } else {
                std::printf("[debug_despawn] despawned; stale handle {%u, %u} "
                            "now resolves to nullptr (good)\n", h.index, h.generation);
            }
        }
    }
}

// ---- system-switch dev timers (np-6al.1) ------------------------------------
//
// Advances the --goto / --goto-soak dev clocks and QUEUES frame-boundary
// switches — it only ever sets g.pending_goto; the actual teardown+build runs
// at the top of frame_cb. --goto fires one switch; --goto-soak cycles the
// galaxy's systems N times then quits cleanly (so cleanup_cb runs and the
// shutdown path is part of the leak audit).
void update_system_switch_timers(float dt) {
    g.switch_clock_s += dt;

    // --goto <id>: a single deferred switch, goto_at_s after Flight begins.
    if (!g.goto_system.empty() && g.switch_clock_s >= g.goto_at_s) {
        g.pending_goto = g.goto_system;
        g.goto_system.clear();
        return;
    }

    // --goto-soak <n>: cycle through the galaxy's systems on an interval.
    if (g.soak_remaining > 0 && g.switch_clock_s >= g.soak_interval) {
        if (!g.galaxy.systems.empty()) {
            const int n = (int)g.galaxy.systems.size();
            for (int step = 0; step < n; ++step) {
                g.soak_index = (g.soak_index + 1) % n;
                const std::string& cand = g.galaxy.systems[g.soak_index].id;
                if (cand != g.player.current_system) {
                    g.pending_goto = cand;
                    break;
                }
            }
        }
        --g.soak_remaining;
        if (g.soak_remaining <= 0) {
            g.soak_quit = true;   // last switch queued; quit once it lands
            std::printf("[system] --goto-soak: final switch queued\n");
        }
        return;
    }

    // Soak finished: let the final system render a beat, then quit cleanly.
    if (g.soak_quit && g.soak_remaining <= 0 && g.pending_goto.empty()
        && g.switch_clock_s >= g.soak_interval) {
        std::printf("[system] --goto-soak complete; quitting for clean shutdown\n");
        sapp_request_quit();
    }
}

// ---- jump soak dev driver (np-6al.3) ----------------------------------------
//
// The headless stand-in for a human pressing J. On the dev_jump_interval,
// while in Flight with no jump already in flight: select the first surveyed
// jump gate in the current system, teleport just inside its trigger range,
// and fire the jump through the EXACT same code path the J keypress uses
// (jump::evaluate -> pending_jump + Loading). Because Troy's first gate leads
// to Pyrenees and Pyrenees' only gate leads back to Troy, this ping-pongs the
// round-trip indefinitely — a leak/stability soak over repeated teardown+build.
void update_dev_jump_soak(float dt) {
    if (g.dev_jump_remaining <= 0 && !g.dev_jump_quit) return;
    if (!g.pending_jump_system.empty()) return;   // a jump is mid-flight

    g.dev_jump_clock_s += dt;
    if (g.dev_jump_clock_s < g.dev_jump_interval) return;
    g.dev_jump_clock_s = 0.0f;

    // Final-jump linger: let the last arrival render a beat, then quit clean.
    if (g.dev_jump_quit) {
        std::printf("[dev] --dev-jump-soak complete; quitting for clean shutdown\n");
        sapp_request_quit();
        return;
    }

    // Try each surveyed gate: teleport just clear of it (toward system
    // center, inside trigger range) and take the FIRST that's Ready. Trying
    // them all (not just the first surveyed gate) means a gate with encounter
    // hostiles camped on it doesn't stall the soak.
    int idx = -1;
    jump::Eligibility e;
    for (int i = 0; i < (int)g.system.nav_points.size(); ++i) {
        const NavPointDef& n = g.system.nav_points[i];
        if (n.kind != "jump") continue;
        if (!g.galaxy.jump_target(g.player.current_system, n.name).ok) continue;
        HMM_Vec3    into = HMM_MulV3F(n.position, -1.0f);
        const float len  = HMM_LenV3(into);
        into = (len > 1e-3f) ? HMM_DivV3F(into, len) : HMM_V3(0.0f, 0.0f, -1.0f);
        g.camera.position = HMM_AddV3(n.position, HMM_MulV3F(into, 1000.0f));
        g.camera.velocity = HMM_V3(0.0f, 0.0f, 0.0f);
        e = jump::evaluate(g.camera, g.system, g.galaxy, g.player.current_system, i,
                            g.player.has_jump_drive);
        if (e.status == jump::Status::Ready) { idx = i; break; }
        std::printf("[dev] --dev-jump-soak: gate '%s' not ready (%s); trying next\n",
                    n.name.c_str(), jump::status_str(e.status));
    }
    if (idx < 0) {
        std::printf("[dev] --dev-jump-soak: no READY gate in %s this tick; waiting\n",
                    g.system.name.c_str());
        return;   // don't burn a jump credit; retry next interval
    }

    g.selected_nav = idx;
    const NavPointDef& gate = g.system.nav_points[idx];
    std::printf("[dev] --dev-jump-soak: auto-J %s -> %s via %s (%d remaining)\n",
                g.player.current_system.c_str(), e.dest_id.c_str(),
                gate.name.c_str(), g.dev_jump_remaining - 1);
    g.pending_jump_system = e.dest_id;
    g.pending_jump_nav    = e.arrival_nav;
    sfx::jump();
    game_state::request_mode(g.game, GameMode::Loading);

    --g.dev_jump_remaining;
    if (g.dev_jump_remaining <= 0) g.dev_jump_quit = true;
}

// ---- player death & respawn (np-ma2.2) --------------------------------------
//
// How long the death cinematic holds before we respawn. The explosion FX
// lifetime is ~1.2s; we linger a beat past that so the fireball fully
// blooms-and-fades over the cockpit before the screen swaps to the base.
static constexpr float k_death_cinematic_s = 6.0f;   // ~3s to watch the
                                                       // fireball fully bloom
                                                       // and another beat to
                                                       // register "I'm dead".

// Drop the player back into the world after a death. POLICY (Privateer-
// authentic): reload the autosave (slot 0, written on every dock by
// docking::tick) and land the player at their last docked base. The
// consequence — by design — is that you lose any progress made since
// that last landing (credits earned, cargo bought, reputation shifts):
// the classic "save-at-base" tension that pairs with np-ymp.1's
// autosave-on-dock. If no autosave exists yet (died before ever docking)
// we fall back to the new_game baseline in the current system and respawn
// in free Flight where the wreck was, since there's no base to land at.
// to_title (np-3dp.18): instead of dropping the player back into the world
// at their last base, bounce all the way out to the TITLE screen on death.
// The heal/clear/atlas-rebind cleanup is identical either way; only the
// restore SOURCE and the final destination differ.
static void respawn_player(bool to_title = false) {
    // 1) Restore the persistent half of the player. Returning to the title
    //    starts from a fresh new_game baseline (so the menu's NEW is
    //    clean); a normal respawn reloads the autosave (last-docked base).
    if (to_title) {
        g.player = player::new_game(g.player.current_system);
        std::printf("[respawn] death -> title: new_game baseline (%lld cr)\n",
                    (long long)g.player.credits);
    } else {
        PlayerState restored;
        const bool from_save = savegame::load(restored, savegame::k_autosave_slot);
        if (from_save) {
            g.player = restored;
            std::printf("[respawn] restored autosave: base=%s, system=%s, %lld cr\n",
                        g.player.last_docked_base.c_str(),
                        g.player.current_system.c_str(),
                        (long long)g.player.credits);
        } else {
            g.player = player::new_game(g.player.current_system);
            std::printf("[respawn] no autosave found — new_game baseline (%lld cr)\n",
                        (long long)g.player.credits);
        }
    }

    // 2) Refit the player ship from the (possibly restored) player state:
    //    re-bind the hull class, heal to full, and re-mount the saved guns
    //    (np-3dp.25) so a ship-swap / re-armed save respawns correctly,
    //    not just with the boot loadout.
    if (Ship* pl = g.ships.player()) {
        apply_player_loadout(*pl, g.player);
    }

    // 3) Clear anything that could act on / kill the fresh player: in-flight
    //    projectiles, the death FX, and any autopilot/docking ownership.
    g.projectiles.clear();
    g.missiles.clear();
    g.explosions.clear();
    if (autopilot::engaged(g.autopilot)) {
        autopilot::disengage(g.autopilot, g.camera, "");
    }
    g.docking = Docking{};
    g.player_target_id = 0;
    g.camera.velocity     = HMM_V3(0.0f, 0.0f, 0.0f);
    g.camera.cruise_level = 0.0f;
    g.camera.cruise_target = 0.0f;

    // 4) Re-resolve the player hull atlas (np-ma2.2). The death trigger
    //    nulled it so the 3rd-person orbit cam wouldn't render the wreck
    //    for the rest of the cinematic. Resolve against the (possibly
    //    restored) ship class name so a future ship-swap save still
    //    shows the right hull. Same logic as the system-load block, so
    //    we re-share the helper at the top of the file by inlining the
    //    resolve here (one-shot, cheap).
    {
        const std::string pc = g.player.ship_class_name.empty()
                             ? std::string("tarsus") : g.player.ship_class_name;
        const std::string pstem = resolve_ship_atlas_stem("ships/" + pc + "/atlas_manifest");
        auto [it, inserted] = g.ship_sprite_atlases.try_emplace(pstem, ShipSpriteAtlas{});
        if (inserted && !load_ship_sprite_atlas(pstem, it->second, g.sprite_art)) {
            std::fprintf(stderr, "[orbit] player atlas '%s' failed to load - 3rd-person hull hidden\n",
                         pstem.c_str());
            g.ship_sprite_atlases.erase(it);
            g.player_atlas = nullptr;
        } else {
            g.player_atlas = &it->second;
            std::printf("[orbit] player hull atlas ready: %s\n", pstem.c_str());
        }
    }

    // 5) Destination. Death -> TITLE (np-3dp.18): re-show the menu, frozen
    //    in Flight render mode, and re-arm the title scene so it re-rolls
    //    a fresh variant/ship set. The wreck isn't drawn under the title,
    //    and NEW/LOAD take over from the chrome.
    if (to_title) {
        g.player.docked = false;
        game_state::request_mode(g.game, GameMode::Flight);
        g.show_title         = true;
        g.title_scene_inited = false;
        std::printf("[respawn] player destroyed -> returning to TITLE screen\n");
        return;
    }

    // Otherwise: with a last docked base, drop the player Landed there
    // (apply_pending's transition handler runs base_screens::enter for
    // us). Otherwise free Flight at the current spot.
    if (!g.player.last_docked_base.empty()) {
        g.player.docked = true;
        game_state::request_mode(g.game, GameMode::Landed);
        std::printf("[respawn] RESPAWNING AT %s — Landed\n",
                    g.player.last_docked_base.c_str());
    } else {
        g.player.docked = false;
        game_state::request_mode(g.game, GameMode::Flight);
        std::printf("[respawn] RESPAWNING in free flight (no base on record)\n");
    }
}

// ---- encounter director host hooks (np-ma2.3) -------------------------------
//
// The director (src/encounters.cpp) decides WHEN/WHERE/WHO; these two
// functions are the WHAT — the exact np-eag.1 spawn/despawn recipe the
// debug button above runs, lifted into reusable hooks the director calls
// through encounters::SpawnFn / DespawnFn. They live here (not in
// encounters.cpp) because the sprite-slot pool + atlas map + registry all
// live in AppState; the director stays AppState-free by going through
// these.

// Per-class display length (world metres, pre-scale) for director spawns,
// mirroring the sizes the troy.json fleet uses so director ships read at
// the same scale as their hand-placed kin. Unknown classes fall back to a
// fighter-ish 80 m.
static float encounter_class_length(const std::string& cls) {
    if (cls == "drayman")  return 525.0f;  // big heavy hauler (a true freighter hull)
    if (cls == "kamekh")   return 400.0f;  // Kilrathi capital
    if (cls == "paradigm") return 450.0f;  // Confed capital
    if (cls == "galaxy")   return 120.0f;
    if (cls == "broadsword") return 110.0f;
    if (cls == "tarsus")   return 100.0f;
    if (cls == "orion")    return 60.0f;
    return 80.0f;   // talon + the light fighters + anything unrecognised
}

// Spawn recipe: claim a sprite slot (reuse freed, else append — deque
// growth keeps every Ship::sprite pointer valid), attach the class atlas,
// ship::spawn() for class-derived stats, assign faction + enable AI, and
// register in the slot-map. Returns the new ship's monotonic id, or 0 if
// the class/atlas isn't loaded in this system (director skips a 0).
//
// On-demand atlas load: if the director preload loop missed this class
// (a new mission target_faction not in any nav/rule table of the current
// system), we try `load_ship_sprite_atlas` here so encounter_spawn never
// silently 0's because the preload omitted it. A per-process "tried+failed"
// set prevents spam on a hard asset miss.
static bool ensure_ship_atlas_for_class(const std::string& cls) {
    if (cls.empty()) return false;
    const std::string atlas_stem =
        resolve_ship_atlas_stem("ships/" + cls + "/atlas_manifest");
    if (g.ship_sprite_atlases.find(atlas_stem) != g.ship_sprite_atlases.end())
        return true;   // already loaded (preload or earlier on-demand hit)
    static std::unordered_map<std::string, bool> failed;
    if (failed.count(cls)) return false;   // we already proved it doesn't load
    auto [it, inserted] = g.ship_sprite_atlases.try_emplace(atlas_stem, ShipSpriteAtlas{});
    if (!load_ship_sprite_atlas(atlas_stem, it->second, g.sprite_art)) {
        std::fprintf(stderr,
            "[encounter] could not on-demand load atlas '%s' for class '%s'\n",
            atlas_stem.c_str(), cls.c_str());
        g.ship_sprite_atlases.erase(it);
        failed[cls] = true;     // permanent miss — don't retry every frame
        return false;
    }
    return true;
}
static uint32_t encounter_spawn(const encounters::SpawnRequest& req) {
    // np-wdk: resolve to the _3d cache key so this find() matches the slot
    // the encounter-director preload loop populated.
    const std::string atlas_key =
        resolve_ship_atlas_stem("ships/" + req.class_name + "/atlas_manifest");
    const ShipClass*  klass     = ship_class::find(req.class_name);
    auto atlas_it = g.ship_sprite_atlases.find(atlas_key);
    if (!klass || atlas_it == g.ship_sprite_atlases.end()) {
        // On-demand load fallback: maybe a new mission target_faction slipped
        // past the preload loop (this is the case for dralthi/kamekh etc when
        // the current system has no native kilrathi in its nav tables). Try
        // once now and retry the lookup; bail cleanly if the asset is missing.
        if (klass && ensure_ship_atlas_for_class(req.class_name))
            atlas_it = g.ship_sprite_atlases.find(atlas_key);
        else
            atlas_it = g.ship_sprite_atlases.end();
    }
    if (!klass || atlas_it == g.ship_sprite_atlases.end()) {
        std::fprintf(stderr, "[encounter] spawn failed: class/atlas '%s' not loaded\n",
                     req.class_name.c_str());
        return 0;
    }

    const size_t slot = claim_sprite_slot();
    ShipSpriteObject& spr = g.placed_ship_sprites[slot];
    spr.atlas      = &atlas_it->second;
    spr.position   = req.position;
    spr.world_size = encounter_class_length(req.class_name) * world_scale::k_ship_size_scale;

    Ship inst       = ship::spawn(*klass);
    inst.sprite     = &spr;
    inst.faction    = req.faction;
    inst.ai.enabled = true;
    inst.ai.state   = req.initial_ai_state;
    inst.ai.patrol_anchor     = req.patrol_anchor;
    inst.ai.has_patrol_anchor = true;   // loiter / flee-home tether at spawn
    // Non-combat civilian behaviour (lane traffic / loiter / convoy escort).
    inst.ai.civ_role          = req.civ_role;
    inst.ai.formation_lead_id = req.formation_lead_id;
    inst.ai.formation_offset  = req.formation_offset;

    const uint32_t id = inst.id;        // ship::spawn already minted it
    g.ships.spawn(std::move(inst));
    return id;
}

// Despawn recipe (the reverse): park the sprite slot (atlas=nullptr makes
// renderer + integrator skip it) and despawn the registry slot (bumps the
// generation so any stale handle resolves to nullptr). No-op on an
// already-gone id.
static void encounter_despawn(uint32_t id) {
    const ShipHandle h = g.ships.find_handle_by_id(id);
    Ship* s = g.ships.get(h);
    if (!s) return;
    free_sprite_slot(s->sprite);
    g.ships.despawn(h);
}

// Despawn EVERY non-player NPC (np-3dp.20). Used on base launch to clear
// the old wave so a fresh encounter rolls — otherwise the ships you left
// behind (including any you provoked into hostility) persist across the
// land/launch and an accidental shot reads 'red' forever. Player ship is
// left untouched. Collect ids first, then despawn, so we don't mutate the
// registry mid-iteration.
static void despawn_all_npcs() {
    std::vector<uint32_t> ids;
    for (const Ship& s : g.ships)
        if (!s.is_player && s.id != 0) ids.push_back(s.id);
    for (uint32_t id : ids) encounter_despawn(id);
}

// ---- mission-driven forced spawns: host wiring (#14) ------------------------
//
// UNIFIED approach-trigger model: EVERY mission type triggers a ONE-TIME
// spawn of enemies when the player approaches a nav point or base that meets
// the mission's criteria. The difference is whether the spawn is RANDOM or
// GIVEN:
//
//   * Scout / Patrol  -> RANDOM: a chance roll on first approach. If it hits,
//     spawn a small group; if it misses, mark that objective "rolled" and
//     never spawn there (true "enemies may or may not show up"). Mission
//     still completes on reach regardless.
//   * Attack / DefendBase / Bounty -> GIVEN: the specific target_faction,
//     guaranteed to the required count, one-time, no top-up.
//
// Each objective keys on a unique string (nav name / base_id / "__bounty__")
// so a Patrol spawns independently at each nav — one-time each. The roll
// happens at most once per objective: mission_objective_triggered() gates it.
// sync_active_missions() first drops bookkeeping for any mission we no longer
// hold; prune_mission_tracks() drops stale ids so the triggered-set reflects
// the LIVE wing. No type tops up — one-time spawns only.
static void update_mission_forces() {
    using MT = missions::MissionType;

    // Reconcile tracked forces with the live mission set (drop abandoned /
    // completed / jumped-away ids) BEFORE arming new ones.
    std::vector<std::string> active_ids;
    active_ids.reserve(g.player.missions.size());
    for (const ActiveMission& am : g.player.missions) active_ids.push_back(am.id);
    encounters::sync_active_missions(active_ids);
    encounters::prune_mission_tracks(g.ships);   // drop stale ids before arming

    // Nav/base position resolvers against the LIVE system — same rules the #13
    // tracker uses, so the forced wing lands exactly where the objective clears.
    auto nav_pos_by_name = [&](const std::string& name, HMM_Vec3& out) -> bool {
        for (const NavPointDef& n : g.system.nav_points)
            if (n.name == name) { out = n.position; return true; }
        return false;
    };
    auto base_pos_by_id = [&](const std::string& base_id, HMM_Vec3& out) -> bool {
        if (base_id.empty()) return false;
        for (const NavPointDef& n : g.system.nav_points)
            if (n.base_id == base_id) { out = n.position; return true; }
        return nav_pos_by_name(base_id, out);
    };

    // Uniform random int in [lo, hi] (inclusive) for scout/patrol spawn counts.
    auto uri = [](int lo, int hi) -> int {
        if (hi <= lo) return lo;
        return lo + (std::rand() % (hi - lo + 1));
    };
    // One chance roll: true with probability `p` (0..1).
    auto chance_roll = [](float p) -> bool {
        return (float)(std::rand() % 10000) < p * 10000.0f;
    };

    for (const ActiveMission& am : g.player.missions) {
        const MT type = (MT)am.type;
        if (type == MT::CargoDelivery) continue;   // no forced wing

        // Scout / Patrol use the mission's target_faction for their random
        // spawns too (it's set by the generator and matches the briefing's
        // $EN). A random-outlaw variant is a one-line tweak here if an
        // "undetermined type" is wanted later.
        const Faction fac = faction::from_name(am.target_faction);
        if (fac == Faction::Count) continue;

        // ---- RANDOM types: Scout / Patrol ----
        // Each nav rolls independently on first approach; a hit spawns a small
        // group, a miss marks the objective one-time (never re-rolls).
        if (type == MT::Scout || type == MT::Patrol) {
            if (am.target_system != g.player.current_system) continue;
            for (const std::string& nav_name : am.nav_targets) {
                HMM_Vec3 nav_pos{ 0, 0, 0 };
                if (!nav_pos_by_name(nav_name, nav_pos)) continue;
                if (HMM_LenV3(HMM_SubV3(g.camera.position, nav_pos))
                    > encounters::k_mission_arm_m)
                    continue;   // not yet approaching this nav
                if (encounters::mission_objective_triggered(am.id, nav_name))
                    continue;   // already rolled (hit or miss) — one-time
                // CAP: once k_patrol_max_enemy_navs navs on this route have
                // actually yielded enemies, every remaining nav auto-misses
                // (marked one-time) so a long patrol never becomes a gauntlet.
                if (encounters::mission_yielded_count(am.id)
                    >= encounters::k_patrol_max_enemy_navs) {
                    encounters::mark_mission_objective_triggered(am.id, nav_name);
                    continue;
                }
                if (chance_roll(encounters::k_scout_encounter_chance)) {
                    encounters::MissionForce mf;
                    mf.mission_id   = am.id;
                    mf.objective_key = nav_name;
                    mf.anchor        = nav_pos;
                    mf.faction       = fac;
                    mf.count         = uri(encounters::k_scout_spawn_min,
                                           encounters::k_scout_spawn_max);
                    encounters::ensure_mission_force(g.system, mf,
                                                     g.camera.position, encounter_spawn);
                } else {
                    // Miss: mark one-time so this nav never re-rolls.
                    encounters::mark_mission_objective_triggered(am.id, nav_name);
                }
            }
            continue;   // Scout/Patrol handled — no GIVEN fallthrough
        }

        // ---- GIVEN types: Attack / DefendBase / Bounty ----
        HMM_Vec3 anchor{ 0, 0, 0 };
        int      count   = 0;
        bool     armed   = false;
        std::string obj_key;

        switch (type) {
        case MT::Attack:
            // In the target system AND within arm range of the nav objective
            // (don't pre-spawn a furball half a system away).
            if (am.target_system != g.player.current_system) break;
            if (!am.nav_targets.empty() && nav_pos_by_name(am.nav_targets.front(), anchor)) {
                obj_key = am.nav_targets.front();
                count   = am.hostiles_required > 0 ? am.hostiles_required : am.count_required;
                armed   = HMM_LenV3(HMM_SubV3(g.camera.position, anchor))
                          <= encounters::k_mission_arm_m;
            }
            break;
        case MT::DefendBase:
            // In the target system AND within arm range of the defend-target
            // base: spawning on system entry (from a different base in the
            // same system) left the attackers free to drift >40km and get
            // despawned before the player arrived. Arm only when the player
            // is actually approaching the base, same gate Attack uses.
            if (am.target_system != g.player.current_system) break;
            if (base_pos_by_id(am.target_base, anchor)) {
                obj_key = am.target_base;
                count   = am.hostiles_required > 0 ? am.hostiles_required : am.count_required;
                armed   = HMM_LenV3(HMM_SubV3(g.camera.position, anchor))
                          <= encounters::k_mission_arm_m;
            }
            break;
        case MT::Bounty: {
            // Roaming hunt across a region: arm whenever the player is in a
            // system the bounty covers, anchored on the player so the quarry
            // turns up wherever they search.
            const std::string& sys = g.player.current_system;
            bool in_region = (am.target_system == sys) ||
                             (am.last_seen_system == sys) ||
                             (am.last_seen_alt_system == sys);
            for (const std::string& r : am.bounty_region)
                if (r == sys) { in_region = true; break; }
            if (!in_region) break;

            // Don't ambush the player on the launch pad. The quarry anchors
            // on the player (spawns ~8-15km out), so arming the instant the
            // player is in-region makes the bounty pop in the moment they
            // undock from a base that happens to sit in the hunt region.
            // Gate on standoff from the NEAREST base: only arm once the
            // player has left a base's vicinity and is actually out hunting.
            // (Jump-ins land far from bases, so this fires shortly after
            // arrival there; deep-space systems with no base arm at once.)
            float nearest_base_m = 1e30f;
            for (const NavPointDef& n : g.system.nav_points) {
                if (n.base_id.empty()) continue;
                nearest_base_m = std::min(nearest_base_m,
                    HMM_LenV3(HMM_SubV3(g.camera.position, n.position)));
            }
            if (nearest_base_m < encounters::k_bounty_base_standoff_m) break;

            obj_key = "__bounty__";
            anchor  = g.camera.position;   // hunt finds them near the player
            count   = am.count_required;
            armed   = true;
            break;
        }
        default: break;
        }

        if (!armed || count <= 0) continue;

        encounters::MissionForce mf;
        mf.mission_id    = am.id;
        mf.objective_key = obj_key;
        mf.anchor        = anchor;
        mf.faction       = fac;
        mf.count         = count;
        mf.guarantee_full = true;   // Attack/DefendBase/Bounty: arm to `count`, top up partials
        encounters::ensure_mission_force(g.system, mf, g.camera.position, encounter_spawn);
    }
}

// ---- non-Flight stub screens ------------------------------------------------
//
// Landed / Dying / Loading don't have real screens yet (np-eag.2 only adds
// the state machine). Each renders a dark clear + a one-line debugtext
// label so it's unmistakable which mode you're in, plus the debug panel
// (so the Game Mode combo can drive you back out) and the dev_remote
// hooks (so /screenshot keeps working for validation). Escape returns to
// Flight — wired in event_cb. ASCII-only labels because the sokol
// debugtext fonts have no glyphs past the 8-bit range.
void frame_stub() {
    // Keep the dev channel responsive: queued commands (screenshot,
    // camera pokes) still drain even though the sim is paused.
    dev_remote::drain_commands(g.camera);

    // Fade the engine hum out — landed/dying/loading ships don't thrum.
    // Uses the real frame dt is unavailable here (stub skips the
    // timestep block), so approximate with the display refresh; the
    // lerp only needs "roughly seconds" to fade smoothly.
    sfx::update_engine_hum(0.0f, 0.0f, /*flight_mode=*/false,
                           (float)sapp_frame_duration());

    // Dynamic music director (np-ida) keeps running across modes: Landed
    // plays the per-base tune (picked by g.player.last_docked_base's
    // archetype), Dying fires the death sting + fades out, Loading fires the
    // jump sting + holds the current bed across the jump beat. Uses the
    // display refresh as dt (the stub skips the sim timestep) — the gain lerp
    // only needs "roughly seconds".
    music::update(g.game.mode, g.camera.position,
                  g.player.last_docked_base.c_str(),
                  (float)sapp_frame_duration());

    // Landed mode draws the data-driven base screens (np-9cu.4) over the
    // whole framebuffer; Dying/Loading still show a one-line debugtext
    // label. ASCII-only labels (the debugtext fonts have no glyphs past
    // the 8-bit range).
    // Jump hyperspace flash (np-6al.3): when a jump is queued, the Loading
    // screen blows out white at entry and fades to the void over the jump
    // beat — a cheap, satisfying "snap into hyperspace" cue (no bespoke FX
    // pass; just the clear colour animated against time_in_mode_s). `flash`
    // is 1 at entry, 0 by the time execute_jump swaps the world.
    const bool  jumping = (g.game.mode == GameMode::Loading &&
                           !g.pending_jump_system.empty());
    float       flash   = 0.0f;
    if (jumping) {
        const float t = g.game.time_in_mode_s / k_jump_loading_s;  // 0..1
        flash = 1.0f - std::min(1.0f, std::max(0.0f, t));
        flash = flash * flash;   // ease-out so the white lingers then snaps off
    }

    const bool landed = (g.game.mode == GameMode::Landed);
    if (!landed) {
        const float fb_w = (float)sapp_width();
        const float fb_h = (float)sapp_height();
        sdtx_canvas(fb_w * 0.5f, fb_h * 0.5f);
        sdtx_font(0);
        sdtx_color3f(0.7f, 1.0f, 0.9f);
        sdtx_pos(2.0f, 2.0f);
        switch (g.game.mode) {
            case GameMode::Dying:   sdtx_puts("DYING - death sequence TBD\n");  break;
            case GameMode::Loading:
                if (jumping) sdtx_printf("JUMPING TO %s...\n",
                                         g.pending_jump_system.c_str());
                else         sdtx_puts("LOADING - load screens TBD\n");
                break;
            default:                sdtx_puts("? - unknown mode\n");            break;
        }
        sdtx_font(1);
        sdtx_color3f(0.5f, 0.6f, 0.7f);
        sdtx_printf("\nmode '%s' for %.1fs   ESC returns to flight\n",
                    game_state::to_name(g.game.mode), g.game.time_in_mode_s);
    }

    // ImGui frame so the Ctrl+M debug panel (Game Mode combo) stays
    // usable while parked on a stub screen. debug_panel::build issues
    // simgui_new_frame(), so base_screens::build (which records ImGui draw
    // commands) must follow it and precede debug_panel::render().
    debug_panel::build(g.placed_meshes, g.placed_ship_sprites, g.game,
                       g.ship_debug, g.audio_debug, g.player);
    if (landed) {
        // A save loaded straight into a base (autosave-on-dock, or LOAD from
        // the menu) never passes through the Flight update where the loaded
        // loadout + damage snapshot are normally stamped onto the live hull
        // (np-3dp.19). Do it here, BEFORE the base screens read the ship, or
        // the repair desk quotes a stale/full hull until you launch + land
        // again. Gated on the one-shot apply_health_pending flag so it runs
        // once per load, not every landed frame.
        if (Ship* pl = g.ships.player()) {
            if (g.apply_health_pending) {
                // Re-fit the live hull to the loaded player state (the menu
                // load rebuilds the system with first_time=false, which skips
                // the spawn-time loadout, so a ship-swap/re-armed save would
                // otherwise show the previously-spawned hull). heal=false so
                // the damage snapshot below is what defines current health.
                apply_player_loadout(*pl, g.player, /*heal=*/false);
            }
            apply_pending_player_health_snapshot(*pl);
        }
        base_screens::build(g.player, g.ships.player(), g.docking, g.camera, g.game,
                            g.sun.position);
    }

    sg_pass p{};
    p.swapchain = sglue_swapchain();
    p.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    // Lerp the void toward white by the jump flash (0 = normal dark clear).
    p.action.colors[0].clear_value = {
        0.02f + (1.0f - 0.02f) * flash,
        0.02f + (1.0f - 0.02f) * flash,
        0.05f + (1.0f - 0.05f) * flash,
        1.0f };
    sg_begin_pass(&p);
    if (!landed) sdtx_draw();
    debug_panel::render();
    sg_end_pass();
    sg_commit();

    dev_remote::maybe_capture_screenshot();
}

// Debug fast-forward multiplier for the sim dt. ']' cycles 1x -> 2x -> 4x ->
// 8x -> 1x. Scales EVERYTHING (AI, physics, projectiles, camera turn rate),
// which is the point -- watch the AI brawl unfold in 1/8th the wall time.
static float g_time_scale = 1.0f;

// Update the 3rd-person orbit/freelook camera. Active only while the nav
// autopilot is engaged (the ship flies itself). The mouse pans the camera
// (offset-from-centre = yaw/pitch rate, virtual-joystick style, matching
// fly-by-wire); scroll changes distance. The ship continues to fly via
// g.camera — orbit_cam is a separate RENDER camera only.
void update_orbit_camera(float dt) {
    // Orbit camera is forced ON while the death cinematic plays so the
    // player can watch their ship bloom into a fireball (np-ma2.2).
    // Otherwise follow autopilot engagement.
    g.orbit_active = autopilot::engaged(g.autopilot) ||
                     g.game.mode == GameMode::Dying;
    if (!g.orbit_active) { g.orbit_was_active = false; return; }

    // Engage edge: start LOCKED directly behind the ship (engines in view,
    // roll matched). Drop fly-by-wire so the camera is fixed and the cursor
    // is free — same semantics as manual flight. Press SPACE to toggle
    // fly-by-wire ON and freelook-orbit around the ship.
    if (!g.orbit_was_active) {
        g.orbit_yaw   = 0.0f;
        g.orbit_pitch = 0.0f;
        g.orbit_dist  = 600.0f;
        g.orbit_was_active = true;
        g.fly_by_wire = false;
        sapp_show_mouse(true);
    }

    // Freelook only while fly-by-wire is ON (SPACE toggles it, exactly as in
    // manual flight). Otherwise the camera is hard-locked behind the ship.
    // Signs are negated to cancel the 180-degree view roll below, so the
    // on-screen orbit direction stays intuitive (mouse right -> pan right).
    if (g.fly_by_wire) {
        const float dpi = sapp_dpi_scale();
        const float vw = (float)sapp_width()  / dpi;
        const float vh = (float)sapp_height() / dpi;
        const float off_x = std::clamp((g.mouse_x - vw * 0.5f) / (vw * 0.5f), -1.0f, 1.0f);
        const float off_y = std::clamp((g.mouse_y - vh * 0.5f) / (vh * 0.5f), -1.0f, 1.0f);
        auto dz = [](float x) { return (std::fabs(x) < 0.08f) ? 0.0f : x; };
        constexpr float k_orbit_rate = 2.2f;   // rad/s at full deflection
        g.orbit_yaw   += -dz(off_x) * k_orbit_rate * dt;
        g.orbit_pitch += -dz(off_y) * k_orbit_rate * dt;
        g.orbit_pitch  = std::clamp(g.orbit_pitch, -1.45f, 1.45f);   // ~ +-83 deg
    }
    // When fly-by-wire is OFF the camera HOLDS its current pan (locked
    // wherever you left it) rather than snapping back to the rear view —
    // the mouse simply stops affecting it.
    g.orbit_dist = std::clamp(g.orbit_dist, 80.0f, 1500.0f);

    // cam orientation = ship * yaw(body+Y) * pitch(body+X) * roll(180 about
    // forward). The roll flips the view upright (the ship was rendering
    // upside-down without it). Roll is a fixed constant -> never changes
    // with mouse, so the camera stays roll-locked to the ship. Position
    // keeps the ship centred: cam = ship - forward*dist.
    const HMM_Quat cam_o = HMM_NormQ(HMM_MulQ(HMM_MulQ(HMM_MulQ(
        g.camera.orientation,
        HMM_QFromAxisAngle_RH(HMM_V3(0.0f, 1.0f, 0.0f), g.orbit_yaw)),
        HMM_QFromAxisAngle_RH(HMM_V3(1.0f, 0.0f, 0.0f), g.orbit_pitch)),
        HMM_QFromAxisAngle_RH(HMM_V3(0.0f, 0.0f, 1.0f), 3.14159265358979f)));

    Camera& oc = g.orbit_cam;
    oc = g.camera;   // inherit fov / near / far / cruise fov etc.
    oc.orientation = cam_o;
    const HMM_Vec3 cam_fwd = oc.forward();
    oc.position = HMM_SubV3(g.camera.position, HMM_MulV3F(cam_fwd, g.orbit_dist));
}

// Alpha-build welcome / briefing overlay. Drawn during the HUD pass (ImGui
// frame already open) while g.show_welcome is true; the sim is frozen
// (dt=0) behind it. Dismissed with SPACE/ENTER (see event_cb).
void draw_welcome_overlay() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 center(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                        vp->WorkPos.y + vp->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar;

    const ImU32 amber = IM_COL32(255, 200, 60, 255);
    const ImU32 cyan  = IM_COL32(120, 220, 255, 255);

    if (ImGui::Begin("##welcome", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, amber);
        ImGui::TextUnformatted("WELCOME TO THE ALPHA BUILD OF PRIVATEER: NEXT GEN");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushTextWrapPos(0.0f);

        ImGui::TextUnformatted(
            "You are flying a Tarsus - the space equivalent of a piece-of-shit "
            "Chevy pickup from the 80s. It has two laser cannon: the worst guns "
            "in the entire game, but in the hands of a skilled pilot, nothing to "
            "scoff at. You also carry 4 dumbfire missiles. Use them wisely.");
        ImGui::Spacing();

        ImGui::TextUnformatted(
            "In the distance there is a massive battle between Pirates (Talons) "
            "and Confederation forces (Centurions and Orions). Go kill some Talons.");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, cyan);
        ImGui::TextUnformatted("FLIGHT");
        ImGui::PopStyleColor();
        ImGui::TextUnformatted(
            "Use +/- to control your speed. Your piece-of-shit Tarsus tops out at "
            "300 kps. Afterburners can push you to 600 kps but rapidly drain your "
            "energy. Energy returns gradually over time - and it's also what fires "
            "your laser cannon.");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, cyan);
        ImGui::TextUnformatted("COMBAT");
        ImGui::PopStyleColor();
        ImGui::TextUnformatted(
            "Press T to cycle through nearby targets - find a Talon and blast him "
            "(left-click / Ctrl to fire lasers, Enter to fire missiles). Your "
            "shields are about as strong as office printer paper and your armor is "
            "basically bamboo: if a Talon gets you in a 1-on-1 joust, you will not "
            "survive it. But a skilled pilot can easily defeat a pirate Talon.");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, amber);
        ImGui::TextUnformatted(
            "PRO TIP: the Space bar toggles fly-by-wire on and off.");
        ImGui::PopStyleColor();
        ImGui::TextUnformatted(
            "You will likely get killed - but this is the alpha, so you'll just "
            "respawn and carry on.");
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, cyan);
        ImGui::TextUnformatted("AFTER THE FIGHT");
        ImGui::PopStyleColor();
        ImGui::TextUnformatted(
            "Once all the Talons are dead, explore the Troy system. Press N to "
            "select a jump point (e.g. the jump to Regallis), press A to autopilot "
            "cruise there, approach the jump gate and press J to jump. Other "
            "systems await. This is a very vanilla build - not much is complete yet.");

        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, amber);
        ImGui::TextUnformatted("            >>>  Press  [ SPACE ]  or  [ ENTER ]  to begin  <<<");
        ImGui::PopStyleColor();
    }
    ImGui::End();
}



void frame_cb() {
    // Issue #26 scoped timer for the whole frame, plus a free log line on
    // every event we know is part of combat input. Together these let a
    // postmortem correlate "key down at T+0" with "engine responded at T+23ms"
    // (ScopEDTimer at the function reading the key) with "frame finished at
    // T+16ms" (this top-level TRACELOG_SCOPED_FUNC). They're cheap and
    // compile out cleanly when TRACELOG_ENABLE is 0.
    TRACELOG_SCOPED_FUNC();

    // --- timestep -----------------------------------------------------------
    const uint64_t now    = stm_now();
    const float    raw_dt = (float)stm_sec(stm_diff(now, g.last_frame_ticks));
    float          dt     = raw_dt * g_time_scale;
    g.last_frame_ticks    = now;

    // Welcome overlay freezes the whole sim (player, AI, projectiles) so the
    // briefing reads against a still frame. Everything still RENDERS — only
    // the integration step is zeroed. Dismissed in event_cb (SPACE/ENTER).
    // Title screen freezes the sim so the chrome plate doesn't move
    // behind the menu (and so LOAD can re-target the world safely).
    // (g.show_welcome was the prior welcome-overlay freeze — REMOVED in
    // np-3dp.4 since the title screen owns the chrome now.)
    if (g.show_title)   dt = 0.0f;
    // Flight pause (np-pau.28): P toggles g.paused; while up we freeze the
    // sim the same way the title screen does so the world reads still.
    if (g.paused)      dt = 0.0f;

    // Title scene (np-3dp): advance the patrol ships even while the sim
    // is frozen, otherwise the title would render static ships and the
    // 'fly by' feel wouldn't read. Lazy-init on the first frame the
    // title is up so we always have a fresh category + atlas load.
    // Title intro fly-in timer (np-3dp). Accumulates real seconds since
    // the title appeared; the render path eases the camera in over the
    // first few seconds. File-static so the render section (later in this
    // same frame_cb) can read it.
    static float s_title_elapsed = 0.0f;
    if (g.show_title) {
        if (!g.title_scene_inited) {
            s_title_elapsed = 0.0f;   // restart the fly-in on a fresh title
            // Pick a category deterministically per process. Mixes
            // wall-clock nanoseconds + a monotonic show-count so two
            // back-to-back title visits don't repeat.
            const auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            static int s_show_n = 0;
            ++s_show_n;
            const uint64_t h = (uint64_t)now_ns ^ (uint64_t)(s_show_n * 2654435761u);
            constexpr int k_n = 5;   // Category count
            const int cat = (int)(h % (uint64_t)k_n);
            title_scene::Category chosen = (title_scene::Category)cat;
            title_scene::init(chosen, g.sprite_art);
            // Apply the category's star preset so the sun reads as canonical
            // (e.g. red sun for Kilrathi, yellow for the rest). NPC + the
            // system skybox are untouched; the player sees whatever's loaded.
            if (const StarPreset* sp = find_star_preset(title_scene::star_preset(chosen))) {
                apply_star_preset(g.sun, *sp);
            }
            // ChaseCam first traversal (np-3dp.16): park the sun at a
            // FIXED world point clearly off to the LEFT or RIGHT of the
            // ship's projected path (the +Z line into the hole), not
            // ahead/behind it. Lateral X dominates; Z sits somewhere
            // WITHIN the 180k traversal so the star is beside the path
            // and sweeps front->back with parallax as the ship cruises.
            if (title_scene::variant() == title_scene::Variant::ChaseCam) {
                const float side = ((std::rand() & 1) ? 1.0f : -1.0f)
                                 * (70000.0f + (float)(std::rand() % 45000));
                const float high = -(float)(std::rand() % 10001);   // -10000..0
                const float along = -(50000.0f + (float)(std::rand() % 100000));
                g.sun.position = HMM_AddV3(title_scene::jump_hole_pos(),
                                           HMM_V3(side, high, along));
            }
            g.title_scene_inited = true;
            std::printf("[title_scene] init cat=%d preset='%s' category='%s'\n",
                        cat, title_scene::star_preset(chosen),
                        title_scene::category_label(chosen));
        }
        // Re-anchor the patrol so the ships stay in view regardless of
        // the player's position in the system. We orbit around a point
        // that's a few hundred meters in front of the camera; the
        // forward direction is the camera's +Z so the ships face the
        // player correctly.
        {
            const HMM_Vec3 fwd   = g.camera.forward();
            const HMM_Vec3 right = g.camera.right();
            const HMM_Vec3 up    = g.camera.up();
            const HMM_Vec3 anchor_pos = HMM_AddV3(g.camera.position,
                                                   HMM_MulV3F(fwd, 600.0f));
            title_scene::set_anchor(anchor_pos, fwd, right, up);

            // Sun handling differs per variant:
            //   * ChaseCam: the sun is a FIXED world point, set once per
            //     traversal at the jump event (np-3dp.13) — NOT followed
            //     per-frame, so it drifts with natural parallax as the
            //     ship cruises (the old per-frame follow felt forced).
            //     So we leave g.sun.position alone here.
            //   * Patrol: park a static up-left sun as a distant star.
            const title_scene::ChaseConfig cc =
                title_scene::chase_config(g.camera.position, fwd, right, up);
            if (!cc.cam_override) {
                g.sun.position =
                    HMM_AddV3(g.camera.position,
                              HMM_AddV3(HMM_MulV3F(fwd,   90000.0f),
                                        HMM_AddV3(HMM_MulV3F(right, -22000.0f),
                                                  HMM_MulV3F(up,     12000.0f))));
            }
            // Warp streaks (the autopilot cruise trails). The chase cam
            // turns them on; patrol leaves them off (warp_on=false).
            if (cc.warp_on) {
                g.warp_streaks.intensity    = cc.warp_intensity;
                g.warp_streaks.streak_len_m = cc.warp_len;
                g.warp_streaks.vel_dir      = HMM_NormV3(cc.warp_dir);
            } else {
                g.warp_streaks.intensity = 0.0f;
            }
        }
        // NOTE: the 180-deg view roll for the title is applied to a COPY
        // of the camera at render time (see scene_cam construction below),
        // NOT here. Mutating g.camera.orientation each frame accumulated
        // the roll (dt=0 never resets it) and flipped the view every
        // frame -> bad flicker. (np-3dp)
        title_scene::tick(raw_dt);   // animate with the real dt so ships drift
        // Clamp the per-frame slice so a giant first-frame dt (skybox
        // cubemap generation can stall a frame for 1-3s) doesn't skip
        // the whole 4s fly-in. 50ms cap = ~20fps floor; normal 60fps
        // frames (~16ms) pass through untouched.
        s_title_elapsed += std::min(raw_dt, 0.05f);

        // --- Galaxy tour (np-3dp.13): jump through a hole each 90s pass ---
        // The hero ship arrives at a jump hole exactly on the hull swap
        // (title_scene owns the timing + flash). On the jump event we
        // reskin the backdrop to a random system's sky + reposition the
        // sun. A full system reload here crashes mid-title (Metal encoder
        // fault), and the chase cam only ever shows skybox + sun + ships
        // anyway, so we swap JUST the skybox cubemap + sun preset. We're
        // in the sim section, BEFORE any render pass opens, so the
        // skybox destroy()+init() is encoder-safe; the cube regenerates
        // at the usual generate() next frame, hidden behind the flash.
        if (title_scene::variant() == title_scene::Variant::ChaseCam &&
            title_scene::consume_jump_event() && !g.galaxy.systems.empty()) {
            const int n = (int)g.galaxy.systems.size();
            const galaxy::SystemEntry& sys = g.galaxy.systems[std::rand() % n];
            const std::string seed = sys.id;   // per-system sky
            const uint64_t   h   = sky_family_hash(seed);
            const SkyFamily  fam = sky_family_for_hash(h);
            const SkyFamilyConfig& cfg = k_sky_families[(int)fam];
            const std::string sun_name = sky_family_pick_sun(fam, h);
            if (const StarPreset* sp = find_star_preset(sun_name))
                apply_star_preset(g.sun, *sp);
            g.skybox.destroy();
            g.skybox.init(seed, /*face_res=*/4096, cfg.warmth,
                          cfg.target_a, cfg.target_b);
            // Park the new system's sun at a FIXED world point clearly off
            // to the LEFT or RIGHT of the ship's projected path (np-3dp.16):
            // lateral X dominates; Z sits WITHIN the 180k traversal so the
            // star is beside the path, not ahead/behind it, and sweeps
            // front->back with natural parallax as the ship cruises (no
            // per-frame follow). Randomised side/height/depth per system.
            const float side = ((std::rand() & 1) ? 1.0f : -1.0f)
                             * (70000.0f + (float)(std::rand() % 45000));
            const float high = -(float)(std::rand() % 10001);   // -10000..0
            const float along = -(50000.0f + (float)(std::rand() % 100000));
            g.sun.position = HMM_AddV3(
                title_scene::jump_hole_pos(),
                HMM_V3(side, high, along));
            sapp_set_window_title(("new_privateer — " + sys.display_name).c_str());
            std::printf("[title] jump -> '%s' (sky reskin + sun repos)\n",
                        sys.display_name.c_str());
        }
    } else if (title_scene::inited()) {
        // Title dismissed — drop our cached state so the next title visit
        // gets a fresh category + atlas load, and re-arm the lazy-init
        // latch so re-entering the title (e.g. on death) re-rolls.
        title_scene::shutdown();
        g.title_scene_inited = false;
    }

    // --- deferred system switch (np-6al.1, frame boundary only) -------------
    // Dev timers (--goto / --goto-soak) and the debug dropdown queue a switch
    // in g.pending_goto; we apply it HERE, at a clean frame boundary, never
    // mid-frame (the switch tears down + rebuilds the entire scene). Gated to
    // Flight so we don't yank the world out from under a base screen. After a
    // switch we present nothing else this frame and let the next frame run
    // the freshly-built world.
    if (g.game.mode == GameMode::Flight) {
        update_system_switch_timers(dt);
        update_dev_jump_soak(dt);
    }
    if (!g.pending_goto.empty()) {
        const std::string target = g.pending_goto;
        g.pending_goto.clear();
        g.switch_clock_s = 0.0f;
        load_and_build_system(target, /*first_time=*/false);
        g.keys_down.fill(false);   // no key ghosts across the switch
        return;
    }

    // --- game-mode transition (frame start, never mid-frame) ----------------
    // Any request_mode() calls from last frame land here. On a transition,
    // wipe held-key state so a key held across the flip doesn't ghost-
    // thrust (or ghost-fire) when we re-enter Flight later.
    if (game_state::apply_pending(g.game, dt)) {
        g.keys_down.fill(false);
        // Base-screen lifecycle (np-9cu.4): enter loads the docked base's
        // concourse art + hotspot JSON when we land; exit frees the GPU
        // texture when we leave Landed (launch, or any other exit).
        if (g.game.mode == GameMode::Landed) {
            base_screens::enter(g.player.last_docked_base);
            // Base screens always need the cursor (np-3dp.29). If the player
            // pressed SPACE during flight to drop fly-by-wire, the OS cursor
            // would stay hidden through the base menu otherwise. Force the
            // cursor on and clear fly_by_wire so the cockpit HUD stops
            // drawing the aim cursor too — leaving fly_by_wire set on a
            // docked ship means re-launching would inherit a stale aim pose.
            sapp_show_mouse(true);
            g.fly_by_wire = false;
            // Anchor the player to the base's nav point (np-3dp.21). When
            // you LAND normally docking already set pad_pos + parked the
            // camera on the pad; but when you LOAD a docked save (or
            // --dev-land) you never flew there, so pad_pos/camera are
            // stale (origin) and launch would fling you to a random spot.
            // Look up the base's nav position and pin both pad_pos + the
            // camera to it, so the subsequent launch puts you right beside
            // the base.
            for (const NavPointDef& n : g.system.nav_points) {
                if (n.dockable && n.base_id == g.player.last_docked_base) {
                    g.docking.pad_pos   = n.position;
                    g.docking.base_id   = n.base_id;
                    g.docking.base_name = n.name;
                    g.camera.position   = n.position;
                    break;
                }
            }
            // Mission board (np-zte.1): regenerate this base's offers on each
            // dock from a base-id + slow-clock seed, so the board feels alive
            // between visits but is stable within a sitting. Reads the galaxy
            // graph for reachable delivery destinations.
            missions::generate_board(g.player.last_docked_base,
                                     g.player.current_system, g.galaxy);
        } else if (g.game.prev_mode == GameMode::Landed) {
            base_screens::exit();
            // Re-fit the live ship from the player state on launch (np-3dp.26):
            // the equipment dealer only mutates PlayerState (gun_mounts,
            // ship_class_name), so without this a gun you fitted or a hull
            // you bought wouldn't take effect until you died. Heal ONLY when
            // the hull class actually changed (a brand-new ship comes full);
            // a normal land/launch preserves unrepaired battle damage so the
            // repair service still matters.
            if (Ship* pl = g.ships.player()) {
                const bool hull_changed =
                    !pl->klass || pl->klass->name != g.player.ship_class_name;
                // Armor is a per-instance purchase (no hull default); compare
                // the player's bought package against what's currently fitted
                // so installing/removing armor re-heals to the new max.
                const ArmorType* desired_armor = g.player.armor_name.empty()
                    ? nullptr : armor::find(g.player.armor_name);
                const bool armor_changed = desired_armor != pl->fitted_armor;
                apply_player_loadout(*pl, g.player, /*heal=*/hull_changed || armor_changed);
                std::printf("[outfit] launch re-fit: klass=%s mounts=%zu%s%s\n",
                            pl->klass ? pl->klass->name.c_str() : "<null>",
                            pl->mounts.size(),
                            hull_changed ? " (new hull -> healed)" : "",
                            (!hull_changed && armor_changed) ? " (new armor -> healed)" : "");
            }
            // Outfitting (np-9cu.3): fold the player's hull + engine_level into
            // the camera's flight speed caps as we launch back into Flight.
            // engine_level 0 on the stock Tarsus reproduces the old 300/600.
            const outfitting::SpeedCaps caps = outfitting::effective_speed_caps(g.player);
            g.camera.max_speed_cruise0 = caps.cruise0;
            g.camera.max_speed_cruise1 = caps.cruise1;
            std::printf("[outfit] launch speed caps -> %.0f / %.0f (engine L%d)\n",
                        caps.cruise0, caps.cruise1, g.player.engine_level);

            // Fresh sky on launch (np-3dp.20): clear the old NPC wave +
            // any in-flight ordnance/target, then re-roll the per-nav
            // encounter tables. This is the 'base-launch clear old wave'
            // path the wcnews model always intended (encounters.h) —
            // without it the ships you left behind, including ones you
            // PROVOKED, persist across the land/launch so an accidental
            // shot stays hostile forever. Canonical Privateer gives you a
            // clean encounter every time you undock.
            despawn_all_npcs();
            g.projectiles.clear();
            g.missiles.clear();
            g.player_target_id = 0;
            g.missile_lock = AppState::MissileLock{};
            encounters::init(g.system);
            encounters::populate_on_entry(g.system, g.camera.position,
                                          g.sun.position, encounter_spawn);
            std::printf("[encounter] base launch -> cleared old wave + re-rolled\n");
        }
    }

    // Debug spawn/despawn requests — applied here, before any system
    // iterates the registry or holds Ship&s for the frame.
    apply_ship_debug_requests();

    // Jump transition (np-6al.3): while parked in the Loading cinematic with
    // a queued jump, hold for the hyperspace flash (k_jump_loading_s) then
    // warp — rebuild the destination system + drop the player at the
    // reciprocal gate, and request a return to Flight. Same deferred
    // discipline as the system switch above: the heavy teardown/build runs
    // at a clean frame boundary, never mid-frame.
    if (g.game.mode == GameMode::Loading && !g.pending_jump_system.empty()
        && g.game.time_in_mode_s >= k_jump_loading_s) {
        execute_jump();
    }

    // Dying cinematic (np-ma2.2): hold for k_death_cinematic_s while the
    // explosion plays out, then bounce all the way back to the TITLE
    // screen (np-3dp.18 — death returns you to the menu). We DON'T short-
    // circuit to frame_stub here — Dying falls through to the full Flight
    // sim+render below (gated to freeze player control) so the fireball is
    // actually visible blooming over the dead cockpit. respawn_player
    // (to_title) re-shows the title + re-arms the scene; the flip lands at
    // the top of the following frame.
    if (g.game.mode == GameMode::Dying &&
        g.game.time_in_mode_s >= k_death_cinematic_s) {
        respawn_player(/*to_title=*/true);
    }

    // Landed / Loading render a stub screen and skip the entire sim +
    // render path below. Flight (and the Dying cinematic) fall through.
    if (g.game.mode != GameMode::Flight && g.game.mode != GameMode::Dying) {
        frame_stub();
        return;
    }

    // During the Dying cinematic the sim keeps running (NPCs, projectiles,
    // explosions) but the player is a dead wreck — freeze pilot input so
    // the camera holds on the blast instead of drifting under stale keys.
    const bool dying = (g.game.mode == GameMode::Dying);

    // --- fly-by-wire aim ----------------------------------------------------
    // Drive yaw/pitch from the absolute mouse position. We skip when the
    // player has popped into free-cursor mode (SPACE) and ALSO when
    // ImGui wants the mouse (e.g. they're hovering the debug panel) —
    // otherwise the ship would lurch every time the user reaches for a
    // slider in CTRL+M.
    // Autodock (np-9cu.1) and the nav autopilot (np-opa.3) each own the
    // ship while engaged — mute the pilot's aim so they can't fight the
    // autopilot. nav_autopilot may be cancelled by manual input just
    // below, so it's recomputed after that.
    const float dpi = sapp_dpi_scale();
    const float view_w = (float)sapp_width()  / dpi;
    const float view_h = (float)sapp_height() / dpi;
    const float off_x = (g.mouse_x - view_w * 0.5f) / (view_w * 0.5f);
    const float off_y = (g.mouse_y - view_h * 0.5f) / (view_h * 0.5f);

    const bool dock_autopilot = docking::controls_locked(g.docking);

    // Manual-override cancel (bead np-opa.3): once engaged, any deliberate
    // flight input hands the stick back — the afterburner key, or a hard
    // mouse-steer past the dead-zone (a resting cursor doesn't count, or
    // the ship would never autopilot at all).
    if (autopilot::engaged(g.autopilot)) {
        const bool key_input = g.keys_down[SAPP_KEYCODE_TAB];
        const bool mouse_steer =
            g.fly_by_wire && !ImGui::GetIO().WantCaptureMouse &&
            !autopilot::engaged(g.autopilot) &&   // during autopilot the mouse orbits the camera, never cancels
            (std::fabs(off_x) > 0.35f || std::fabs(off_y) > 0.35f);
        if (key_input || mouse_steer) {
            autopilot::disengage(g.autopilot, g.camera,
                                 "AUTOPILOT DISENGAGED - MANUAL OVERRIDE");
            std::printf("[autopilot] manual input — control returned\n");
        }
    }
    const bool nav_autopilot = autopilot::controls_locked(g.autopilot);
    const bool autopilot_lock = dock_autopilot || nav_autopilot;

    if (g.fly_by_wire && !ImGui::GetIO().WantCaptureMouse && !autopilot_lock
        && !dying) {
        g.camera.apply_mouse_aim(off_x, off_y, dt);
    }

    // Drain any queued dev_remote commands into game state BEFORE
    // physics / rendering — a /camera/set that arrived this frame
    // should be visible in the frame we're about to produce.
    dev_remote::drain_commands(g.camera);

    // --- physics ------------------------------------------------------------
    // Hold-Tab cruise: drive target to 1 while held, 0 otherwise. Camera
    // smooths the lerp so it feels like winding up and winding down.
    // Skipped entirely under dock autopilot — docking::tick moves the
    // camera itself, and we don't want manual thrust/integrate doubling
    // up on its position update.
    if (!autopilot_lock && !dying) {
        // Afterburner energy gate (np-zte.2, merged pool). The cruise system
        // IS the afterburner here (there's no second engine — see camera.h),
        // and it now drains from the player Ship's energy_gj — the SAME pool
        // the guns spend from. Holding TAB with energy on hand drains it and
        // drives cruise_target to 1; at empty the engine cuts out (target
        // forced to 0) and the player must release to let firing.cpp's
        // energy_recharge refill the bank. One pool, one bar, one tactical
        // call: burst speed vs. burst fire.
        const bool want_ab   = g.keys_down[SAPP_KEYCODE_TAB];
        Ship* pl_for_ab = g.ships.player();
        const float ab_energy = pl_for_ab ? pl_for_ab->energy_gj : 0.0f;
        const bool ab_active = want_ab && ab_energy > 0.0f && pl_for_ab != nullptr;
        if (ab_active) {
            g.camera.cruise_target = 1.0f;
            const float drain = player::k_afterburner_drain_per_s * dt;
            const float taken = std::min(drain, pl_for_ab->energy_gj);
            pl_for_ab->energy_gj -= taken;
            if (pl_for_ab->energy_gj < 0.0f) pl_for_ab->energy_gj = 0.0f;
            // Log the cutout edge once, when the bank just hit empty.
            if (pl_for_ab->energy_gj <= 0.0f && taken > 0.0f)
                std::printf("[afterburner] energy exhausted \u2014 cutout\n");
        } else {
            g.camera.cruise_target = 0.0f;
            // No explicit regen here — firing.cpp's tick rebuilds energy_gj
            // every frame at klass->energy_recharge. Releasing TAB just
            // stops the drain; the bank refills on its own.
        }
        // Throttle: HOLD + / - to ramp the cruising speed up/down at
        // k_throttle_rate m/s per second (smooth, no tapping). Capped at
        // normal cruise on the high end; MINUS clamps at zero so the ship
        // decelerates to a stop rather than overshooting into negative
        // throttle and flying backwards (np-spd.27).
        constexpr float k_throttle_rate = 150.0f;   // m/s per second held
        const float thr_step = k_throttle_rate * dt;
        if (g.keys_down[SAPP_KEYCODE_EQUAL])
            g_speed_input_ref = std::min(g_speed_input_ref + thr_step,
                                         g.camera.max_speed_cruise0);
        if (g.keys_down[SAPP_KEYCODE_MINUS])
            g_speed_input_ref = std::max(g_speed_input_ref - thr_step,
                                        0.0f);

        // Roll: HOLD , / . to roll around the camera view axis. Held
        // action — release lets the camera's angular damping (in
        // Camera::integrate) level it back out. Scoped to Flight mode
        // to match the throttle block above. Sign: , rolls left (-1),
        // . rolls right (+1). apply_roll composes locally on the
        // orientation quaternion, so it coexists with mouse-aim
        // pitch+yaw without cancelling either.
        if (g.keys_down[SAPP_KEYCODE_COMMA])
            g.camera.apply_roll(-1.0f, dt);
        if (g.keys_down[SAPP_KEYCODE_PERIOD])
            g.camera.apply_roll(+1.0f, dt);

        // Forward speed is the ONLY manual flight input now (the ship aims
        // by mouse). + / - set the cruising speed in g_speed_input_ref;
        // holding Tab overrides to full afterburn speed and snaps back to
        // the setting on release. integrate() lerps at the ship's accel
        // rate, so afterburn reads as a hard kick forward.
        const float desired = ab_active ? g.camera.max_speed_cruise1
                                        : g_speed_input_ref;
        g.camera.set_forward_input(desired);
        g.camera.integrate(dt);
    }

    // Automatic landing zone (np-3dp.22): the player no longer requests a
    // dock by hand near a base — fly close enough and it just happens.
    // Inside k_zone_announce_m of the nearest dockable base we play the
    // 'Now entering an automatic landing zone' comms + sting (once per
    // approach); inside k_auto_land_m we land INSTANTLY (land_now commits
    // straight to Landed — no fly-to-pad approach, no cinematic beat).
    // Only while free-flying (not mid-dock, not in the post-launch
    // cooldown).
    if (g.game.mode == GameMode::Flight &&
        g.docking.state == DockingState::None && g.docking.cooldown_s <= 0.0f) {
        int   near_nav = -1;
        float near_d   = 1e30f;
        for (int i = 0; i < (int)g.system.nav_points.size(); ++i) {
            const NavPointDef& n = g.system.nav_points[i];
            if (!n.dockable || n.base_id.empty()) continue;
            const float d = HMM_LenV3(HMM_SubV3(n.position, g.camera.position));
            if (d < near_d) { near_d = d; near_nav = i; }
        }
        if (near_nav >= 0 && near_d < docking::k_zone_announce_m) {
            if (!g.landing_zone_announced) {
                g.landing_zone_announced = true;
                comm::push("Now entering an automatic landing zone.", false);
                music::landing_approach();
            }
            if (near_d < docking::k_auto_land_m) {
                docking::land_now(g.docking, g.game, g.player,
                                  g.system.nav_points[near_nav]);
            }
        } else {
            g.landing_zone_announced = false;   // re-arm on leaving the zone
        }
    }

    // Autodock step (np-9cu.1). Drives the camera during the approach,
    // holds the docking beat, then requests GameMode::Landed; also bleeds
    // the post-launch cooldown. No-op in free flight. Runs after manual
    // physics so its pose is the one the audio listener + render see.
    docking::tick(g.docking, g.camera, g.game, g.player, dt);

    // Issue #24 fail-on-land: any cargo consignment whose dest_base isn't
    // the base we just docked at gets failed (cargo jettisoned, no reward).
    // Idempotent — the moment we commit to the pad is the only time the
    // "off-target" check kicks in; the function also no-ops on missions
    // whose target is this dock (so Deliver still works) and on every
    // non-cargo mission.
    if (g.player.docked && !g.player.last_docked_base.empty()) {
        static std::string s_last_docked;
        if (s_last_docked != g.player.last_docked_base) {
            missions::fail_cargo_on_dock(g.player, g.player.last_docked_base);
            s_last_docked = g.player.last_docked_base;
        }
    }

    // Nav autopilot step (np-opa.3). While engaged it owns the camera:
    // orients toward the selected nav, winds up the cruise engine, and
    // eases to a stop on arrival (or drops out if threat:: trips). No-op
    // in free flight. Runs after manual physics for the same reason.
    autopilot::tick(g.autopilot, g.camera, g.system, dt, g.sun.position);

    // ---- sun damage (np-3dp) ----------------------------------------------
    // Inside the 15k bubble the player takes 5cm damage per second. We
    // accumulate dt and apply once a full second has rolled over, so the
    // damage reads as a discrete tick every second rather than a per-frame
    // trickle. NPC ships would also benefit here but for v1 we keep it
    // player-only so AI-vs-sun interactions stay out of the AI's way.
    static float s_sun_damage_accum = 0.0f;
    if (Ship* pl = g.ships.player(); pl && pl->alive) {
        if (hazards::inside_sun_damage(g.sun.position, g.camera.position)) {
            s_sun_damage_accum += dt;
            while (s_sun_damage_accum >= hazards::k_sun_tick_s) {
                s_sun_damage_accum -= hazards::k_sun_tick_s;
                // Pick a facing at random so a sun-dweller doesn't have
                // all damage channel to one shield facet. Any one will
                // do for the player right now (just barely past a Heavy
                // Shields block, say) — same code path the NPCs use.
                static const HitFacing k_faces[] = {
                    HitFacing::Fore, HitFacing::Aft,
                    HitFacing::Port, HitFacing::Starboard};
                const HitFacing face = k_faces[((int)(stm_sec(stm_now()) * 1000.0)) & 3];
                ship::take_damage(*pl, hazards::k_sun_damage_per_s_cm, face);
            }
        } else {
            // Drain the accumulator back to zero when outside so a brief
            // edge graze doesn't queue up a flurry of hits afterwards.
            s_sun_damage_accum = 0.0f;
        }
    } else {
        s_sun_damage_accum = 0.0f;
    }

    // 3rd-person orbit/freelook camera, driven off autopilot state. Runs
    // after autopilot::tick so engaged-state + ship pose are current; the
    // built orbit_cam is consumed by the scene render pass below.
    update_orbit_camera(dt);

    // ---- warp streaks driver (np-streaks) -------------------------------
    // Spool the cruise streaks in/out based on autopilot state + speed.
    // The streaks ramp in only while autopilot is engaged AND the player
    // is actually moving fast enough for the elongation to read (>800 m/s,
    // i.e. well past normal manual cruise). Streak length is proportional
    // to speed and capped so the wrap-cube doesn't get filled edge-to-edge.
    {
        const HMM_Vec3 vel  = g.camera.velocity;
        const float    spd  = HMM_LenV3(vel);
        const bool     ap   = autopilot::engaged(g.autopilot);
        constexpr float k_speed_floor = 800.0f;     // below this, no streaks
        constexpr float k_speed_full  = 2500.0f;    // at-and-above: full effect
        constexpr float k_len_per_mps = 0.10f;      // streak_len_m per m/s
        constexpr float k_len_max     = 350.0f;     // cap (cube is 600 half-extent)
        constexpr float k_fade_rate   = 4.0f;       // 1/s ease in & out (~0.25s)

        float target_i = 0.0f;
        float target_L = 0.0f;
        HMM_Vec3 target_dir = g.warp_streaks.vel_dir;   // hold last when off
        if (ap && spd > k_speed_floor) {
            target_i = std::clamp((spd - k_speed_floor)
                                  / (k_speed_full - k_speed_floor), 0.0f, 1.0f);
            target_L = std::min(spd * k_len_per_mps, k_len_max);
            target_dir = HMM_DivV3F(vel, spd);          // unit velocity
        }

        // Exponential ease toward the targets so engage/disengage spools
        // smoothly instead of popping. Direction snaps when on (since the
        // streak orientation depends on it), but the master intensity fade
        // hides that on the engage edge.
        const float k = 1.0f - std::exp(-k_fade_rate * dt);
        g.warp_streaks.intensity    += (target_i - g.warp_streaks.intensity)    * k;
        g.warp_streaks.streak_len_m += (target_L - g.warp_streaks.streak_len_m) * k;
        if (target_i > 0.0f) g.warp_streaks.vel_dir = target_dir;
    }

    // Audio listener follows the camera. Before any play_world calls
    // this frame so new voices spatialize against the fresh pose;
    // existing world voices re-pan/attenuate here too.
    audio::set_listener(g.camera.position, g.camera.right());

    // Engine hum rides the throttle: speed fraction vs the cruise-1 max
    // plus the cruise spool level. We're in the Flight path here, so
    // flight_mode is unconditionally true; the non-Flight stub calls
    // this with false to fade the bed out (see frame_stub).
    {
        // While the title is up there are no engine sounds (np-3dp.10 —
        // Mike: removed the menu jet/hum/afterburner, revisit later).
        // Drive the hum like the non-Flight stub does: flight_mode=false
        // fades the hum bed to 0 and releases the afterburner loop. raw_dt
        // (not the frozen dt=0) so that fade actually runs.
        if (g.show_title) {
            sfx::update_engine_hum(0.0f, 0.0f, /*flight_mode=*/false, raw_dt);
        } else {
            const float speed_frac = HMM_LenV3(g.camera.velocity)
                                   / std::fmax(g.camera.max_speed_cruise1, 1.0f);
            sfx::update_engine_hum(speed_frac, g.camera.cruise_level,
                                   /*flight_mode=*/true, dt);
        }
    }

    // Dynamic music director (np-ida): Flight runs the in-flight combat-tier
    // state machine (FlightMain vs Combat Far/Near by nearest-hostile
    // distance, with hysteresis + a resolve outro) and lerps the crossfade.
    // Same threat oracle the autopilot/jump gates use, so the music switches
    // exactly when the danger does. In flight there's no docked base, so the
    // base_id is empty; dt drives the gain lerps + combat hysteresis.
    //
    // While the title is up we feed the director a Menu mode WITHOUT
    // touching the real g.game.mode (np-3dp.6) — the render gate above
    // routes any non-Flight mode to the stub screen + early return, which
    // would black out the title scene. So the mode stays Flight for
    // rendering; only the music sees Menu and plays the OPENING bed.
    const GameMode music_mode = g.show_title ? GameMode::Menu : g.game.mode;
    // dt is 0 while the title freezes the sim, but the music crossfade
    // needs real seconds to lerp the OPENING bed in — feed it raw_dt then.
    const float music_dt = g.show_title ? raw_dt : dt;
    music::update(music_mode, g.camera.position,
                  g.player.last_docked_base.c_str(), music_dt);

    // Audio smoke-test buttons (debug panel). 2D = centered blip; 3D =
    // blip at the selected nav point (or 2km ahead when none selected,
    // so the button always makes a sound you can hunt by ear). Logs the
    // computed L/R gains + distance — the audible check and the math
    // check in one line.
    if (g.audio_debug.play_blip_2d) {
        g.audio_debug.play_blip_2d = false;
        const VoiceId v = audio::play(g.sfx_blip, 1.0f);
        float gl = 0, gr = 0; audio::voice_gains(v, &gl, &gr);
        std::printf("[audio] voice %u playing blip 2D  gain L/R %.2f/%.2f\n",
                    v, gl, gr);
    }
    if (g.audio_debug.play_blip_nav) {
        g.audio_debug.play_blip_nav = false;
        HMM_Vec3 pos;
        const char* where;
        if (g.selected_nav >= 0 && g.selected_nav < (int)g.system.nav_points.size()) {
            pos   = g.system.nav_points[g.selected_nav].position;
            where = g.system.nav_points[g.selected_nav].name.c_str();
        } else {
            pos   = HMM_AddV3(g.camera.position,
                              HMM_MulV3F(g.camera.forward(), 2000.0f));
            where = "<2km ahead — no nav selected>";
        }
        // ref 500m / max 50km: a nav-point beacon should be loud up
        // close and audible across most of the play bubble.
        const VoiceId v = audio::play_world(g.sfx_blip, pos, 500.0f, 50000.0f);
        const float dist = HMM_LenV3(HMM_SubV3(pos, g.camera.position));
        float gl = 0, gr = 0; audio::voice_gains(v, &gl, &gr);
        std::printf("[audio] voice %u playing blip 3D at %s  "
                    "gain L/R %.2f/%.2f  dist %.0fm\n",
                    v, where, gl, gr, dist);
    }

    // Pose sync: every ship's canonical position/orientation field
    // (used by perception + AI) gets refreshed from its source of
    // truth. Player (registry slot 0) gets the camera; NPCs get their
    // sprite (the integrator's owner). After this, downstream code
    // reads `s.position` uniformly with no special cases.
    if (Ship* player = g.ships.player(); player) {
        player->position       = g.camera.position;
        player->orientation    = g.camera.orientation;
        // Camera carries the player's full 3D world velocity (forward
        // thrust + lateral strafes + persistent coasting under zero
        // damping). Projectiles inherit this in firing.cpp so tracers
        // move with the player's reference frame instead of drifting.
        player->world_velocity = g.camera.velocity;
        // On the frame after a save load, stamp the loaded damage snapshot
        // onto the live hull (np-3dp.19), BEFORE the mirror below, so a
        // reloaded damaged save stays damaged. One-shot.
        apply_pending_player_health_snapshot(*player);
        // Mirror the hull's CURRENT condition into PlayerState every
        // frame (np-3dp.19) so any save (autosave-on-land, manual)
        // captures live damage without needing the Ship at the save
        // site. Applied back to the spawned ship on load/respawn.
        if (player->alive && g.game.mode == GameMode::Flight) {
            g.player.hp_valid          = true;
            g.player.hp_armor_fore     = player->armor_fore_cm;
            g.player.hp_armor_aft      = player->armor_aft_cm;
            g.player.hp_armor_port     = player->armor_port_cm;
            g.player.hp_armor_starboard = player->armor_starboard_cm;
            g.player.hp_shield_fore    = player->shield_fore_cm;
            g.player.hp_shield_aft     = player->shield_aft_cm;
            g.player.hp_shield_port    = player->shield_port_cm;
            g.player.hp_shield_starboard = player->shield_starboard_cm;
            g.player.hp_energy         = player->energy_gj;
        }
    }
    for (Ship& s : g.ships) {
        if (!s.is_player) ship::sync_from_sprite(s);
    }

    // Perception: every ship learns who's in radar range this tick and
    // how the faction matrix (or player reputation, when the player is
    // involved) classifies them. Runs BEFORE behaviour so any AI that
    // wants to read perception ("chase nearest hostile") sees fresh
    // data. O(N²) over alive ships; cheap at the demo's scale, will
    // need spatial bucketing past ~100 ships.
    perception::tick(g.ships, g.player.rep);

    // AI state machine: between perception (input) and ship::tick
    // (output). For each ai-enabled ship it transitions the state and
    // writes a fresh behaviour for the controller to consume this same
    // tick. ai-disabled ships fall through unchanged — their behaviour
    // came from JSON or stays at None for legacy motion.
    {
        const float t_now = (float)stm_sec(stm_now());   // process uptime
        for (Ship& s : g.ships) ship_ai::tick(s, g.ships, t_now, g.system, g.sun.position);
    }

    // Encounters are no longer maintained continuously: the wcnews model
    // rolls each nav's table ONCE on system entry (populate_on_entry in
    // build_system_scene) and never refills until the next entry / base
    // launch. So there is no per-frame director tick — dead NPCs are reaped
    // by the death pass and stay gone. encounter_despawn is retained for the
    // future base-launch "clear old wave" path; reference it so it doesn't
    // warn as unused.
    (void)&encounter_despawn;

    // Player firing input. Hold LEFT_CTRL to fire — clear, modifier-style
    // key that doesn't conflict with the existing W/A/S/D thrust + Q/E
    // roll + SPACE / TAB / X / N controls. The flag drains every frame
    // it's held; firing::tick consumes it and respects per-mount
    // cooldowns + energy budget.
    //
    // Aim direction defaults to camera-forward ("shoot where I'm
    // looking"). When the player has a locked target (T-cycle), an
    // ITTS aim-gimbal nudges the fire direction toward the lead point
    // within a small cone — same lead math as the on-screen ITTS
    // reticle, so the player sees "green crosshair → tracers go
    // there". Outside the cone, falls back to camera-forward and the
    // player has to rotate the ship to engage.
    if (Ship* player_p = g.ships.player(); player_p) {
        Ship& player = *player_p;

        // Drop the player's ship-target lock if the contact has wandered
        // past the 15 km radar/lock ceiling — mirrors the HUD-targeting
        // rule that nothing beyond 15 km is targetable in the first place.
        // Also clears a target that's gone dead (sprite reaped) so the
        // firing/missile paths below don't aim at a corpse.
        if (g.player_target_id != 0) {
            const Ship* t = g.ships.find_by_id(g.player_target_id);
            if (!t || !t->alive) {
                g.player_target_id = 0;
            } else {
                const float d = HMM_LenV3(HMM_SubV3(t->position, player.position));
                if (d > 15000.0f) {
                    std::printf("[target] dropped: out of range (%.0f m > 15000)\n", d);
                    g.player_target_id = 0;
                }
            }
        }

        player.controller.fire_guns =
            g.keys_down[SAPP_KEYCODE_LEFT_CONTROL] ||
            g.keys_down[SAPP_KEYCODE_RIGHT_CONTROL] ||
            g.mouse_left_held;
        if (g.show_title)    player.controller.fire_guns = false;   // frozen on briefing
        // Navmap overlay owns the click — left-mouse would otherwise fire guns
        // every time the player aimed for a navmap button or scrolled the map.
        if (g.show_navmap)   player.controller.fire_guns = false;

        HMM_Vec3 aim = g.camera.forward();
        if (g.player_target_id != 0) {
            const Ship* target = g.ships.find_by_id(g.player_target_id);
            if (target && !target->alive) target = nullptr;
            if (target) {
                // Average projectile speed across player mounts.
                float proj_speed = 1100.0f;
                {
                    int n = 0; float sum = 0.0f;
                    for (const auto& m : player.mounts) {
                        if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
                        const GunStats& gs = g_gun_stats[(int)m.type];
                        if (!gs.complete) continue;
                        sum += gs.speed_mps; ++n;
                    }
                    if (n > 0) proj_speed = sum / n;
                }
                // Lead position: target_pos + target_vel * (dist / proj_speed).
                // world_velocity is already populated for both NPCs (via
                // sync_from_sprite) and player (camera.velocity), so this
                // works whether the target is moving on rails or being
                // controlled. Using sprite->position when available so
                // we lead where the visual ship is, not the frame-stale
                // Ship::position snapshot.
                const HMM_Vec3 t_pos = target->sprite ? target->sprite->position
                                                       : target->position;
                const HMM_Vec3 to_t  = HMM_SubV3(t_pos, player.position);
                const float    dist  = std::sqrt(HMM_DotV3(to_t, to_t));
                const float    t_int = (proj_speed > 1.0f) ? dist / proj_speed : 0.0f;
                const HMM_Vec3 lead  = HMM_AddV3(t_pos,
                    HMM_MulV3F(target->world_velocity, t_int));
                const HMM_Vec3 to_lead = HMM_SubV3(lead, player.position);
                const float    ll2     = HMM_DotV3(to_lead, to_lead);
                if (ll2 > 1e-6f) {
                    const HMM_Vec3 lead_dir = HMM_DivV3F(to_lead, std::sqrt(ll2));
                    // ±12° gimbal cone around camera-forward. Wider
                    // than the original 5° because long-range targets
                    // with lateral motion produce big lead offsets —
                    // a fighter at 5 km moving 240 m/s sideways leads
                    // ~270 m which can be 10°+ off the target's
                    // current position, outside a tight cone, so
                    // aim-assist never engages where it'd help most.
                    // Wider cone keeps the assist usable at long range
                    // without becoming "auto-aim everywhere" (40°+
                    // would feel like the gun has a mind of its own).
                    constexpr float gimbal_cos = 0.9781f;   // cos(12°)
                    if (HMM_DotV3(g.camera.forward(), lead_dir) >= gimbal_cos) {
                        aim = lead_dir;
                    }
                }
            }
        }
        player.controller.desired_forward = aim;
    }

    // ---- missile target-lock state machine (np-zte.2) ------------------
    // Runs every Flight frame. The selected missile type decides whether a
    // lock is needed (HS/IR) and whether it must BUILD UP (IR ~1.5s). We
    // reuse the T-cycle target (g.player_target_id) as the lock subject:
    //   * no lock-type selected, or no/dead target -> reset to "none".
    //   * HS (instant): locks the moment a target is held.
    //   * IR (build-up): accrues progress_s while the target's held; locks
    //     at k_ir_lock_time. The seeking tone beeps on a cadence until then.
    // Lock-acquired plays once on the seeking->locked rising edge.
    {
        constexpr float k_ir_lock_time   = 1.5f;   // IR build-up window (s)
        constexpr float k_seek_beep_period = 0.45f; // seeking beep cadence (s)
        const MissileStats& sel = g_missile_stats[g.selected_missile];
        AppState::MissileLock& lk = g.missile_lock;

        const Ship* tgt = (g.player_target_id != 0)
                        ? g.ships.find_by_id(g.player_target_id) : nullptr;
        const bool have_target = tgt && tgt->alive;

        if (!sel.needs_lock || !have_target) {
            // Dumbfire selected, or nothing to lock onto: drop any lock.
            lk = AppState::MissileLock{};
        } else {
            // Target changed since last frame -> restart the acquire.
            if (lk.locked_id != g.player_target_id) {
                lk = AppState::MissileLock{};
                lk.locked_id = g.player_target_id;
            }
            const bool was_locked = lk.locked;
            if (!sel.lock_buildup) {
                lk.locked = true;                 // HS: instant heat lock
            } else {
                lk.progress_s += dt;              // IR: hold to build up
                if (lk.progress_s >= k_ir_lock_time) lk.locked = true;
            }
            if (lk.locked && !was_locked) {
                sfx::lock_acquired();             // rising-edge confirmation
            } else if (!lk.locked) {
                // Still seeking — beep on a cadence.
                lk.seek_beep_s -= dt;
                if (lk.seek_beep_s <= 0.0f) {
                    sfx::lock_seeking();
                    lk.seek_beep_s = k_seek_beep_period;
                }
            }
        }

        // ---- ECM break check (np-3dp.27) --------------------------------
        // The player's fitted ECM has a per-second chance to drop any held
        // missile lock. Canonical Privateer rates: L1 25%, L2 50%, L3 75%
        // per second. Roll ONCE per second so the chance is independent of
        // dt (not "every frame at 25%" which is way too strong).
        if (g.player.ecm_level > 0 && lk.locked) {
            constexpr float k_ecm_check_period = 1.0f;
            static float  rate_dt = 0.0f;
            rate_dt += dt;
            if (rate_dt >= k_ecm_check_period) {
                rate_dt = 0.0f;
                const int ecm_pct = (int)g.player.ecm_level * 25;   // 25/50/75
                if ((rand() % 100) < ecm_pct) {
                    lk.locked = false;
                    lk.progress_s = 0.0f;     // force IR to rebuild
                    std::printf("[ecm] break (%d%% roll): missile lock dropped\n",
                                ecm_pct);
                }
            }
        }
    }

    // ---- missile fire (np-zte.2) ---------------------------------------
    // Consume the edge-triggered request raised by the ENTER key. Spawns
    // from the player's muzzle along camera-forward, inheriting ship
    // velocity (same as guns). Refuses (dry click) on an empty rack or a
    // lock-type fired without a completed lock. Finite ammo: success
    // decrements g.player.missiles[type].
    if (g.missile_fire_request) {
        g.missile_fire_request = false;
        if (Ship* pl = g.ships.player(); pl && !dying) {
            const int           ti  = g.selected_missile;
            const MissileStats& sel = g_missile_stats[ti];
            const bool has_ammo = player::missile_count(g.player, ti) > 0;
            const bool lock_ok  = !sel.needs_lock || g.missile_lock.locked;
            if (!has_ammo) {
                sfx::out_of_ammo();
                std::printf("[missile] FIRE refused: %s rack empty\n", sel.short_name);
            } else if (!lock_ok) {
                sfx::out_of_ammo();
                std::printf("[missile] FIRE refused: %s needs a lock (none)\n", sel.short_name);
            } else {
                player::consume_missile(g.player, ti);
                const uint32_t target_id = sel.needs_lock ? g.missile_lock.locked_id : 0;
                const HMM_Vec3 muzzle = HMM_AddV3(pl->position,
                                          HMM_MulV3F(g.camera.forward(), 30.0f));
                Missile m = missile::spawn((MissileType)ti, muzzle,
                                           g.camera.forward(), pl->world_velocity,
                                           pl->id, target_id);
                g.missiles.push_back(m);
                sfx::missile_fired();
                std::printf("[missile] FIRE %s -> target %u | remaining %d | "
                            "pos %.0f,%.0f,%.0f dmg %.0f\n",
                            sel.short_name, target_id,
                            player::missile_count(g.player, ti),
                            muzzle.X, muzzle.Y, muzzle.Z, m.damage_cm);
            }
        }
    }

    // Firing -> spawn projectiles. Runs after AI/player set fire_guns,
    // before projectile motion so a freshly-spawned projectile gets a
    // first-frame integration step (otherwise it'd appear stuck at the
    // muzzle for one frame).
    firing::tick(g.ships, g.projectiles, dt);
    projectile::tick(g.projectiles, dt);
    // Guided missiles (np-zte.2): steer + advance BEFORE the snapshot/damage
    // pass below, exactly like projectiles, so their detonations are caught
    // by the same kill-detection + explosion FX that gunfire uses.
    missile::tick(g.missiles, g.ships, dt);

    // Snapshot alive flags BEFORE damage so we can detect kills this
    // frame and spawn explosions at the right positions. Cheap (one
    // bool per ship); a static thread-local buffer reuses storage so
    // we don't allocate every frame in the steady state. Indexed by
    // registry SLOT (free slots snapshot harmlessly as dead) — slot
    // indices are stable across the damage pass below because nothing
    // despawns mid-frame; the buffers are re-snapshotted every frame
    // so spawn/despawn between frames can't alias either.
    static thread_local std::vector<bool> was_alive;
    was_alive.resize(g.ships.slot_count());
    for (size_t i = 0; i < g.ships.slot_count(); ++i) {
        const Ship* s = g.ships.ship_at(i);
        was_alive[i] = s && s->alive;
    }

    // np-ma2.2 death test: the debug panel's "kill player" button routes
    // through the REAL damage pipeline so the death pass below catches the
    // alive->dead flip exactly as it would for a pirate's killing blow.
    // Applied here (after was_alive is snapshotted, so the transition is
    // visible) rather than in apply_ship_debug_requests (which runs before
    // the snapshot and would make the kill invisible to detection).
    if (g.ship_debug.kill_player) {
        g.ship_debug.kill_player = false;
        if (Ship* pl = g.ships.player(); pl && pl->alive) {
            ship::take_damage(*pl, 1.0e9f, HitFacing::Fore);
            std::printf("[debug] kill player — applied lethal damage\n");
        }
    }

    // Same idea for shield + armor values — record per-facing cm so we
    // can spawn flashes whenever any facing decreases. 8 floats per ship
    // total; ~400 bytes per frame at the demo's scale.
    struct HpSnap { float sh_fore, sh_aft, sh_port, sh_stbd,
                        ar_fore, ar_aft, ar_port, ar_stbd; };
    static thread_local std::vector<HpSnap> hp_prev;
    hp_prev.resize(g.ships.slot_count());
    for (size_t i = 0; i < g.ships.slot_count(); ++i) {
        const Ship* s = g.ships.ship_at(i);
        if (!s) { hp_prev[i] = {}; continue; }
        hp_prev[i] = { s->shield_fore_cm,
                       s->shield_aft_cm,
                       s->shield_port_cm,
                       s->shield_starboard_cm,
                       s->armor_fore_cm,
                       s->armor_aft_cm,
                       s->armor_port_cm,
                       s->armor_starboard_cm };
    }

    // Collision + damage. Runs AFTER projectile::tick — by this point
    // each alive projectile sits at its post-integration position, and
    // collide_and_damage reconstructs the previous position via velocity
    // for the swept-segment test against ship hit spheres. A successful
    // hit subtracts shield-then-armor from the right facing, sets
    // alive=false on a kill, and marks the projectile dead so it stops
    // rendering. Shield regen ticks afterwards on the same frame so
    // recently-hit facings honour their pause-after-hit timer before
    // refilling.
    projectile::collide_and_damage(g.projectiles, g.ships, dt);
    // Missile detonation + damage through the SAME ship::take_damage path
    // (no forked damage logic) — proximity to the locked target or a direct
    // swept hit. Runs alongside the gun collision so a missile kill flows
    // through the identical death-detection below.
    missile::collide_and_damage(g.missiles, g.ships, dt);
    for (Ship& s : g.ships) ship::regen_shields(s, dt);

    // Death detection. For NPCs we trigger on ANY occupied dead ship
    // (not just the alive:true->false edge), because some damage sources
    // run AFTER this loop in frame order — notably ship-ship collision /
    // ramming damage further below. An edge-only check missed those
    // (their alive flips false after the snapshot for the NEXT frame too),
    // leaving un-reaped "ghost" hulls whose animated lights kept drawing.
    // Since a dead NPC is reaped (despawned) the moment it's seen here, the
    // !alive test still fires exactly once per ship; a ram-kill is just
    // caught one frame later. The PLAYER is never reaped, so it keeps the
    // was_alive edge to enter the Dying cinematic exactly once.
    for (size_t i = 0; i < g.ships.slot_count() && i < was_alive.size(); ++i) {
        const Ship* s = g.ships.ship_at(i);
        const bool died = s && !s->alive &&
                          (s->is_player ? was_alive[i] : true);
        if (died) {
            const HMM_Vec3 pos = s->sprite
                ? s->sprite->position
                : s->position;
            // Death rumble. "Big" = cargo-class hulls (>= 100 cargo
            // units: Tarsus, Galaxy — fat freighters make fat booms);
            // fighters and class-less placeholders get the small one.
            sfx::ship_exploded(pos, s->klass && s->klass->cargo_units >= 100);

            // Reputation + mission fallout (np-ma2.1). Two different
            // attribution rules, intentionally split:
            //
            //   * Reputation + the lifetime-kill scoreboard are PLAYER-ONLY
            //     — only the shot the player fired changes how factions feel
            //     about them. killed_by_id is stamped by
            //     projectile::collide_and_damage at the lethal hit (0 = a
            //     non-projectile cause).
            //
            //   * Mission KILL PROGRESS (bounty / attack / defend) counts the
            //     target's death REGARDLESS of who landed it. Mission forces
            //     aren't replaced after combat losses, so if a third party
            //     (ally patrol, faction infighting, a collision) destroys
            //     your bounty target, crediting only player kills would
            //     soft-lock the contract. This pass only runs for ships in
            //     the player's current system, and on_target_destroyed
            //     re-gates by mission region, so it can't credit kills the
            //     player isn't around to witness.
            if (!s->is_player) {
                const Ship* pl = g.ships.player();
                const bool by_player =
                    pl && s->killed_by_id != 0 && s->killed_by_id == pl->id;
                if (by_player) {
                    comm::report_player_kill(g.player, s->faction);
                    // Career scoreboard (np-3dp.19): tally the kill by the
                    // victim's faction so the save records lifetime kills.
                    if ((int)s->faction >= 0 && (int)s->faction < kFactionCount)
                        g.player.faction_kills[(int)s->faction]++;
                }
                // Mission progress: any killer counts (see above).
                missions::on_target_destroyed(g.player, s->faction,
                                              g.player.current_system);
            }
            // Capture the camera basis at the moment of death so the
            // disc shockwave stays where it was if the camera rotates
            // afterwards. Player-facing disc at t=0 reads as a clean
            // expanding ring; later frames the ring may be at a slight
            // angle if the camera moved, which adds parallax.
            explosion::spawn(g.explosions, pos,
                             g.camera.right(), g.camera.up());

            // Player death (np-ma2.2): kick off the Dying cinematic. The
            // mode flip is deferred (lands next frame), so the rest of
            // THIS frame still renders the explosion in the Flight path.
            // Guard on Flight so multiple lethal hits the same frame can't
            // double-request (and so a stray death-flag while already
            // Dying/Landed is ignored). Hand back anything that owns the
            // ship and freeze the camera so the wreck holds in view.
            if (s->is_player && g.game.mode == GameMode::Flight) {
                if (autopilot::engaged(g.autopilot)) {
                    autopilot::disengage(g.autopilot, g.camera, "");
                }
                g.docking = Docking{};
                g.camera.velocity      = HMM_V3(0.0f, 0.0f, 0.0f);
                g.camera.cruise_target = 0.0f;
                // Detach the player hull atlas so the 3rd-person orbit
                // camera doesn't keep drawing the wreck for the rest of
                // the cinematic (np-ma2.2). The render path gates on
                // (orbit_active && g.player_atlas); nulling it here stops
                // the sprite entirely. The atlas is re-resolved in
                // respawn_player() once the cinematic completes.
                g.player_atlas = nullptr;
                g.player_ship_sprite.atlas = nullptr;
                game_state::request_mode(g.game, GameMode::Dying);
                std::printf("[death] player ship destroyed — entering Dying\n");
            }

            // Corpse reap (np-zte.1): a dead NON-player ship has had its
            // explosion/sfx/kill-attribution emitted above — now free its
            // resources so registry slots + the sprite pool don't leak and
            // the encounter director's hard-cap gate (ships.size() >= cap)
            // can keep spawning past 64 cumulative kills. v1 reaps
            // IMMEDIATELY (no linger timer): the explosion FX lives in
            // g.explosions independent of the ship, so the boom outlives
            // the corpse. The player is NEVER reaped here — the
            // death->respawn path (Dying mode, above) owns that hull.
            // The encounter director's prune just erase()s the now-stale
            // managed id next tick (find_by_id -> nullptr); it never
            // double-despawns, so no stale-handle use.
            if (!s->is_player) {
                free_sprite_slot(s->sprite);
                g.ships.despawn(g.ships.find_handle_by_id(s->id));
            }
        }
    }
    explosion::tick(g.explosions, dt);
    comm::tick(dt);   // age the reputation/taunt HUD feed (np-ma2.1)

    // In-flight mission progress (#13): after perception/threat + the
    // kill-attribution hooks above, flip nav-reach / clear state on active
    // missions in THIS system and let the model settle any payouts. Flight
    // only — landed/dying frames don't survey nav points. Player position is
    // the camera (same point the autopilot/nav markers measure from).
    if (g.game.mode == GameMode::Flight) {
        // #14: guarantee the forced opposition for active combat missions is
        // in the world FIRST, so the tracker below can detect (and clear) it.
        update_mission_forces();
        mission_tracker::tick(g.player, g.player.current_system,
                              g.system.nav_points, g.camera.position);
    }

    // Shield + armor impact detection. Walk every ship and check if any
    // facing dropped this frame; spawn the appropriate flash. Skip dead
    // ships — the explosion FX already covers their final visual. The
    // PLAYER ship is special-cased: its bubble would render at the
    // camera position and just fill the view, so we only flag the
    // screen-edge vignette for the player rather than spawning a
    // bubble. NPC armor / shield bubbles render as world-space glows.
    for (size_t i = 0; i < g.ships.slot_count() && i < hp_prev.size(); ++i) {
        const Ship* sp = g.ships.ship_at(i);
        if (!sp) continue;
        const Ship& s = *sp;
        if (!s.alive) continue;
        const HpSnap& prev = hp_prev[i];
        const bool sh_hit = (prev.sh_fore > s.shield_fore_cm)
                         || (prev.sh_aft  > s.shield_aft_cm)
                         || (prev.sh_port > s.shield_port_cm)
                         || (prev.sh_stbd > s.shield_starboard_cm);
        const bool ar_hit = (prev.ar_fore > s.armor_fore_cm)
                         || (prev.ar_aft  > s.armor_aft_cm)
                         || (prev.ar_port > s.armor_port_cm)
                         || (prev.ar_stbd > s.armor_starboard_cm);
        if (!(sh_hit || ar_hit)) continue;

        if (s.is_player) {
            // Player: screen-edge vignette instead of a bubble. Impact
            // sound still plays — 2D-ish by virtue of being at the
            // listener (full ref-dist gain, centered pan).
            g.player_hit_intensity = 1.0f;
            sfx::impact(s.position, /*shield=*/sh_hit, /*victim_is_player=*/true);
            continue;
        }

        const HMM_Vec3 pos = s.sprite ? s.sprite->position : s.position;
        // Impact thunk. Shield-vs-armor distinction mirrors the flash
        // logic above: sh_hit gets the soft absorbed thunk; pure armor
        // damage (shields already down) gets the harsh crack. When both
        // dropped in one frame the shield sound wins — the shield ate
        // first, physically. Rate-limited globally in sfx.cpp.
        sfx::impact(pos, /*shield=*/sh_hit, /*victim_is_player=*/false);
        const float r = ship::hit_radius_m(s);

        if (sh_hit) {
            AppState::ShieldFlash f;
            f.position   = pos;
            f.radius     = r * 1.8f;
            f.age_s      = 0.0f;
            f.lifetime_s = 0.25f;
            g.shield_flashes.push_back(f);
        }
        if (ar_hit) {
            // Armor flash sits ON the hull (1.0x radius, not the
            // wider shield bubble), shorter lifetime, sparkier — these
            // are the "shields down, you're hitting metal now" cue.
            AppState::ArmorFlash f;
            f.position   = pos;
            f.radius     = r * 1.0f;
            f.age_s      = 0.0f;
            f.lifetime_s = 0.18f;
            g.armor_flashes.push_back(f);
        }
    }

    // Flash tick + prune. Inline (small structs, single use site each).
    for (auto& f : g.shield_flashes) {
        f.age_s += dt;
        if (f.age_s >= f.lifetime_s) f.lifetime_s = -1.0f;
    }
    g.shield_flashes.erase(
        std::remove_if(g.shield_flashes.begin(), g.shield_flashes.end(),
                       [](const AppState::ShieldFlash& f){ return f.lifetime_s < 0.0f; }),
        g.shield_flashes.end());
    for (auto& f : g.armor_flashes) {
        f.age_s += dt;
        if (f.age_s >= f.lifetime_s) f.lifetime_s = -1.0f;
    }
    g.armor_flashes.erase(
        std::remove_if(g.armor_flashes.begin(), g.armor_flashes.end(),
                       [](const AppState::ArmorFlash& f){ return f.lifetime_s < 0.0f; }),
        g.armor_flashes.end());

    // Player hit-indicator decay. ~0.5s fade-out (e^(-2*dt) per frame)
    // so the vignette pulses cleanly — bright on impact, gone in half
    // a second. Sustained fire keeps refreshing it via the spawn path
    // above, so it reads as steady-red "you're being hit".
    if (g.player_hit_intensity > 0.0f) {
        g.player_hit_intensity *= std::exp(-2.0f * dt);
        if (g.player_hit_intensity < 0.001f) g.player_hit_intensity = 0.0f;
    }

    // One-shot perception summary on first tick — confirms the wiring
    // without spamming stdout. Static gate flips after the first call.
    {
        static bool s_first_perception_dump = true;
        if (s_first_perception_dump) {
            s_first_perception_dump = false;
            for (const Ship& s : g.ships) {
                const char* name = s.klass     ? s.klass->name.c_str()
                                  : s.is_player ? "player"
                                  :               "?";
                std::printf("[perception] %-8s sees: %d hostile, %d allied, %d neutral",
                            name,
                            s.perception.n_hostile,
                            s.perception.n_allied,
                            s.perception.n_neutral);
                if (s.perception.nearest_hostile_id) {
                    std::printf("  (nearest hostile id=%u @ %.0fm)",
                                s.perception.nearest_hostile_id,
                                s.perception.nearest_hostile_dist);
                }
                std::printf("\n");
            }
        }
    }

    // Ship behaviour + flight controller. Runs BEFORE the integrator so
    // any ship with an active behaviour writes fresh angular_velocity /
    // forward_speed onto its sprite this frame. Ships with behavior=None
    // skip the controller entirely and the integrator below sees the
    // legacy JSON-set motion unchanged — the existing demo flies on this
    // back-compat path.
    for (Ship& s : g.ships) ship::tick(s, dt);

    // NPC ship motion. Free-strafe lives on the camera (player only); ships
    // get aircraft-style integration — orientation rotates by body-frame
    // angular velocity, position advances along body +Z at forward_speed.
    // No-op for the static-ship case so this is safe to call unconditionally.
    update_ship_sprite_motion(g.placed_ship_sprites, dt);

    // ---- ship-vs-ship collisions -----------------------------------
    // O(N²) sphere-sphere check. When two ships overlap (distance <
    // r1 + r2), apply an elastic-ish bounce by adding impulse to each
    // ship's collision_velocity field, separate them by the overlap
    // amount so they don't keep colliding next frame, and apply damage
    // proportional to closing speed (more KE on impact = bigger
    // crunch). Player special-cased because the camera owns player
    // velocity, not the sprite — bounce hits g.camera.velocity
    // directly instead of the (nonexistent) player sprite.
    {
        // Damage per impact: linear in closing speed plus a base. Was
        // 50/0.5/250 then 10/0.1/50; crushed another 10x -> 1/0.01/5 so
        // a glancing nudge is ~0cm and even a 400 kps ram is only ~5cm.
        // Collisions are now visual/control events (the bounce + tumble
        // below), not damage events. Pairs with the spawn separation
        // guard so we very rarely reach a real crunch in the first place.
        constexpr float k_base_dmg          = 1.0f;
        constexpr float k_dmg_per_mps       = 0.01f;
        constexpr float k_dmg_max           = 5.0f;
        constexpr float k_elasticity        = 0.85f;  // 0=plastic, 1=fully elastic (np-3dp: snappier bounce)
        constexpr float k_bounce_boost      = 1.5f;   // extra impulse so a fast ram really separates ships
        constexpr float k_player_hit_radius = 30.0f * 1.4f;  // matches ship::hit_radius_m

        // Slot-indexed double loop (j starts at i+1 so each pair tests
        // once). ship_at returns nullptr for free slots — skipped the
        // same way dead ships are.
        for (size_t i = 0; i < g.ships.slot_count(); ++i) {
            Ship* a_p = g.ships.ship_at(i);
            if (!a_p) continue;
            Ship& a = *a_p;
            if (!a.alive) continue;
            const HMM_Vec3 a_pos = a.sprite ? a.sprite->position : a.position;
            const float a_r = ship::hit_radius_m(a);
            if (a_r <= 0.0f) continue;
            const HMM_Vec3 a_vel = a.is_player ? g.camera.velocity : a.world_velocity;

            for (size_t j = i + 1; j < g.ships.slot_count(); ++j) {
                Ship* b_p = g.ships.ship_at(j);
                if (!b_p) continue;
                Ship& b = *b_p;
                if (!b.alive) continue;
                const HMM_Vec3 b_pos = b.sprite ? b.sprite->position : b.position;
                const float b_r = ship::hit_radius_m(b);
                if (b_r <= 0.0f) continue;

                const HMM_Vec3 d = HMM_SubV3(b_pos, a_pos);
                const float d2 = HMM_DotV3(d, d);
                const float r_sum = a_r + b_r;
                if (d2 >= r_sum * r_sum) continue;

                // Normal: from a -> b. Degenerate-overlap fallback to
                // an arbitrary axis so we still separate ships that
                // happen to spawn at the same point.
                float dist = std::sqrt(std::max(d2, 1e-6f));
                const HMM_Vec3 n = (dist > 1e-3f)
                    ? HMM_DivV3F(d, dist) : HMM_V3(1.0f, 0.0f, 0.0f);

                // Closing speed along the normal. Positive = ships are
                // moving toward each other; negative = already
                // separating (skip impulse but still separate by
                // overlap so they don't stay stuck together).
                const HMM_Vec3 v_rel = HMM_SubV3(b.is_player ? g.camera.velocity
                                                              : b.world_velocity,
                                                  a_vel);
                const float v_rel_n = HMM_DotV3(v_rel, n);

                // Seeded once per pair for all the random-direction logic
                // (kick + tumble below). Both blocks want the same seed so
                // they stay consistent per-frame for a given contact.
                const float t_seed = (float)stm_sec(stm_now());

                // Position separation — split overlap evenly. Player
                // moves the camera; NPCs nudge their sprite position.
                const float overlap = r_sum - dist;
                const HMM_Vec3 push = HMM_MulV3F(n, overlap * 0.5f);
                if (a.is_player) {
                    g.camera.position = HMM_SubV3(g.camera.position, push);
                } else if (a.sprite) {
                    a.sprite->position = HMM_SubV3(a.sprite->position, push);
                }
                if (b.is_player) {
                    g.camera.position = HMM_AddV3(g.camera.position, push);
                } else if (b.sprite) {
                    b.sprite->position = HMM_AddV3(b.sprite->position, push);
                }

                // Apply impulse only if approaching. For equal masses,
                // the impulse magnitude per ship is
                // (1+e) * v_rel_n / 2; opposite signs so a gets pushed
                // back along -n, b along +n.
                // Size scaling (np-3dp): each ship scales its own bounce by
                // the OTHER ship's size ratio (other_r / player_hit_r). A Talon
                // hitting another Talon gets the baseline; ramming a Drayman
                // (or Kamekh/Paradigm) launches the player ~5x further. NPC-NPC
                // pairs scale symmetrically, so the larger ship barely budges.
                const float size_ratio_a = b_r / k_player_hit_radius;
                const float size_ratio_b = a_r / k_player_hit_radius;
                constexpr float k_min_size_ratio = 1.0f;   // baseline
                constexpr float k_max_size_ratio = 5.0f;   // cap (capitals)
                const float scale_a = std::clamp(size_ratio_a, k_min_size_ratio, k_max_size_ratio);
                const float scale_b = std::clamp(size_ratio_b, k_min_size_ratio, k_max_size_ratio);
                if (v_rel_n > 0.0f) {
                    const float impulse_mag = (1.0f + k_elasticity) * v_rel_n * 0.5f * k_bounce_boost;
                    const HMM_Vec3 impulse_a = HMM_MulV3F(n, -(impulse_mag * scale_a));
                    const HMM_Vec3 impulse_b = HMM_MulV3F(n,  (impulse_mag * scale_b));
                    if (a.is_player) {
                        g.camera.velocity = HMM_AddV3(g.camera.velocity, impulse_a);
                    } else if (a.sprite) {
                        a.sprite->collision_velocity =
                            HMM_AddV3(a.sprite->collision_velocity, impulse_a);
                    }
                    if (b.is_player) {
                        g.camera.velocity = HMM_AddV3(g.camera.velocity, impulse_b);
                    } else if (b.sprite) {
                        b.sprite->collision_velocity =
                            HMM_AddV3(b.sprite->collision_velocity, impulse_b);
                    }

                    // Random-direction "kick" (np-3dp): a capital-ram launch
                    // adds an extra impulse along a partly-random direction
                    // -- 50% along "away", 30% random sideways, 20% upward
                    // -- so the player tumbles off in a believable arc
                    // instead of being shoved straight back along the
                    // contact normal. Strength scales with the OTHER ship's
                    // size ratio so Talon-vs-Talon is unchanged but ramming
                    // a Drayman/Kamekh/Paradigm flings the player across
                    // the sector. Decay is automatic: NPC kick rides on
                    // collision_velocity (0.5/s half-life ~1.4s); player
                    // kick rides on camera.velocity which the flight model
                    // damps naturally. Tuning is a quick knob.
                    constexpr float k_kick_base_mps   = 35.0f;  // base kick for closing ~50 m/s; scaled by |v_rel_n|
                    constexpr float k_kick_cap_boost  = 2.5f;   // ramp vs size_ratio past the Talon baseline
                    constexpr float k_kick_min_boost  = 0.15f;  // floor so even Talon-vs-Talon gets a small kick
                    const float kick_a_str = k_kick_base_mps *
                        std::max(k_kick_min_boost, (scale_a - 1.0f) * k_kick_cap_boost + k_kick_min_boost)
                        * (1.0f + std::fabs(v_rel_n) * 0.05f);
                    const float kick_b_str = k_kick_base_mps *
                        std::max(k_kick_min_boost, (scale_b - 1.0f) * k_kick_cap_boost + k_kick_min_boost)
                        * (1.0f + std::fabs(v_rel_n) * 0.05f);
                    auto build_kick_dir = [&](uint32_t seed_salt) {
                        // Direction: 50% along -n ("away"), 30% random
                        // sideways (in the plane perpendicular to n), and
                        // 20% world-up (also projected perpendicular to n).
                        // Inline random unit vector via three phase-shifted
                        // sines so we don't depend on rand_unit (which is
                        // defined later in this block).
                        const float sx = std::sin((float)seed_salt * 0.013f);
                        const float sy = std::sin((float)seed_salt * 0.027f + 1.7f);
                        const float sz = std::sin((float)seed_salt * 0.041f + 3.14f);
                        HMM_Vec3 rnd = HMM_V3(sx, sy, sz);
                        const float rl = HMM_LenV3(rnd);
                        if (rl > 1e-3f) rnd = HMM_DivV3F(rnd, rl);
                        else rnd = HMM_V3(0, 1, 0);
                        // Project random onto plane perpendicular to n
                        const float proj_r = HMM_DotV3(rnd, n);
                        HMM_Vec3 side = HMM_SubV3(rnd, HMM_MulV3F(n, proj_r));
                        const float sl = HMM_LenV3(side);
                        if (sl > 1e-3f) side = HMM_DivV3F(side, sl);
                        else side = HMM_V3(0, 1, 0);
                        // World up, made perpendicular to n too
                        const HMM_Vec3 up_w = HMM_V3(0, 1, 0);
                        const float proj_u = HMM_DotV3(up_w, n);
                        HMM_Vec3 up = HMM_SubV3(up_w, HMM_MulV3F(n, proj_u));
                        const float ul = HMM_LenV3(up);
                        if (ul > 1e-3f) up = HMM_DivV3F(up, ul);
                        else up = HMM_V3(0, 0, 1);
                        HMM_Vec3 dir = HMM_AddV3(
                            HMM_AddV3(HMM_MulV3F(n, -0.5f),
                                      HMM_MulV3F(side, 0.3f)),
                            HMM_MulV3F(up,  0.2f));
                        const float dl = HMM_LenV3(dir);
                        return (dl > 1e-3f) ? HMM_DivV3F(dir, dl) : HMM_MulV3F(n, -1.0f);
                    };
                    // Randomness tied to ship id so the kick direction is
                    // stable for THIS contact but varies by ship (the
                    // contacts are spread over multiple frames, so a
                    // stable-per-frame direction is fine).
                    const HMM_Vec3 kick_dir_a = build_kick_dir((uint32_t)a.id ^ (uint32_t)(t_seed * 7.0f));
                    const HMM_Vec3 kick_dir_b = build_kick_dir((uint32_t)b.id ^ (uint32_t)(t_seed * 13.0f) ^ 0xDEADBEEFu);
                    const HMM_Vec3 kick_imp_a = HMM_MulV3F(kick_dir_a, kick_a_str);
                    const HMM_Vec3 kick_imp_b = HMM_MulV3F(kick_dir_b, kick_b_str);
                    if (a.is_player) {
                        g.camera.velocity = HMM_AddV3(g.camera.velocity, kick_imp_a);
                    } else if (a.sprite) {
                        a.sprite->collision_velocity =
                            HMM_AddV3(a.sprite->collision_velocity, kick_imp_a);
                    }
                    if (b.is_player) {
                        g.camera.velocity = HMM_AddV3(g.camera.velocity, kick_imp_b);
                    } else if (b.sprite) {
                        b.sprite->collision_velocity =
                            HMM_AddV3(b.sprite->collision_velocity, kick_imp_b);
                    }
                }

                // Damage: scale with closing speed, with a base so
                // a slow nudge still does something. Apply to both
                // ships, on the facings that hit each other (b's hit
                // is from -n, a's hit is from +n). Player gets a 0.25×
                // multiplier — "reinforced cockpit" handwave so
                // collision feedback doesn't insta-kill the squishy
                // Tarsus the player flies. NPCs eat full damage.
                constexpr float k_player_dmg_multiplier = 0.25f;
                // Collision-damage cooldown (np-2kx): a Talon that wedges
                // inside a Drayman for ~0.6s eats a damage tick every frame
                // at 60fps. Cap how often ONE ship can eat a crunch so
                // multi-frame overlap = one damage event, like a hit shield.
                // Stamped in seconds via a frame-time accumulator since
                // there's no wall-clock in scope here.
                constexpr float k_collide_dmg_cooldown_s = 0.5f;
                static float s_collide_t = 0.0f;
                s_collide_t += dt;
                const bool can_a = (a.is_player) ||
                    (s_collide_t - (float)a.last_collide_dmg_t) >= k_collide_dmg_cooldown_s;
                const bool can_b = (b.is_player) ||
                    (s_collide_t - (float)b.last_collide_dmg_t) >= k_collide_dmg_cooldown_s;
                const float dmg = std::clamp(
                    k_base_dmg + k_dmg_per_mps * std::fabs(v_rel_n),
                    k_base_dmg, k_dmg_max);
                const float dmg_a = (a.is_player ? dmg * k_player_dmg_multiplier : dmg)
                                    * (can_a ? 1.0f : 0.0f);
                const float dmg_b = (b.is_player ? dmg * k_player_dmg_multiplier : dmg)
                                    * (can_b ? 1.0f : 0.0f);
                // Compute facings: hit point is approximately at the
                // midpoint between ship centers (where they touched).
                const HMM_Vec3 hit_point = HMM_AddV3(a_pos,
                    HMM_MulV3F(n, a_r));
                const bool any_hit = (dmg_a > 0.0f) || (dmg_b > 0.0f);
                if (dmg_a > 0.0f) {
                    a.last_collide_dmg_t = s_collide_t;
                    ship::take_damage(a, dmg_a, ship::facing_of_hit(a, hit_point));
                }
                if (dmg_b > 0.0f) {
                    b.last_collide_dmg_t = s_collide_t;
                    ship::take_damage(b, dmg_b, ship::facing_of_hit(b, hit_point));
                }
                if (!any_hit) {
                    // Both sides throttled by cooldown -- still bump the
                    // timestamps so they don't all stack up next frame.
                    a.last_collide_dmg_t = s_collide_t;
                    b.last_collide_dmg_t = s_collide_t;
                }

                // Ram tumble. Pick a random axis-angle for each ship so
                // they lurch independently. Magnitude scales with closing
                // speed (capped) so a slow nudge gives a small wobble,
                // a head-on at speed gives a real spin. 0.5 s lifetime,
                // decay ~4/s -> ~14% remaining at the end. World frame
                // for the player camera, body frame for NPCs (the sprite
                // integrator picks the right composition order).
                auto rand_unit = [](uint32_t seed) {
                    auto frac = [](uint32_t x) {
                        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                        return (float)(x & 0xFFFFFFu) / (float)0xFFFFFFu;
                    };
                    const float u  = frac(seed) * 2.0f - 1.0f;
                    const float th = frac(seed ^ 0x9E3779B9u) * 6.2831853f;
                    const float r  = std::sqrt(std::max(0.0f, 1.0f - u * u));
                    return HMM_V3(r * std::cos(th), r * std::sin(th), u);
                };
                const float closing = std::fabs(v_rel_n);
                // 4 rad/s baseline + up to 4 more from closing speed (cap
                // at ~200 m/s relative). 0.5 s lifetime. Hitting a capital
                // (Drayman/Kamekh/Paradigm) also spikes the spin -- the
                // OTHER-ship size ratio kicks the player much harder.
                const float w_mag_base = 4.0f + std::min(closing * 0.02f, 4.0f);
                constexpr float k_tumble_size_boost = 0.6f;  // 1.0 = scale w/ size; tuned down so it's visceral but not nauseating
                const float w_mag_a = w_mag_base + (scale_a - 1.0f) * k_tumble_size_boost * 8.0f;  // up to +~3 rad/s extra at scale=5
                const float w_mag_b = w_mag_base + (scale_b - 1.0f) * k_tumble_size_boost * 8.0f;
                const uint32_t seed_a = (uint32_t)a.id * 2654435761u
                                      ^ (uint32_t)(t_seed * 1000.0f);
                const uint32_t seed_b = (uint32_t)b.id * 2654435761u
                                      ^ (uint32_t)(t_seed * 1000.0f) ^ 0xDEADBEEFu;
                const HMM_Vec3 axis_a = HMM_MulV3F(rand_unit(seed_a), w_mag_a);
                const HMM_Vec3 axis_b = HMM_MulV3F(rand_unit(seed_b), w_mag_b);
                if (a.is_player) {
                    g.camera.ram_tumble_w_world      = axis_a;
                    g.camera.ram_tumble_t_remaining  = 0.5f;
                } else if (a.sprite) {
                    a.sprite->ram_tumble_w_body      = axis_a;
                    a.sprite->ram_tumble_t_remaining = 0.5f;
                }
                if (b.is_player) {
                    g.camera.ram_tumble_w_world      = axis_b;
                    g.camera.ram_tumble_t_remaining  = 0.5f;
                } else if (b.sprite) {
                    b.sprite->ram_tumble_w_body      = axis_b;
                    b.sprite->ram_tumble_t_remaining = 0.5f;
                }
            }
        }
        (void)k_player_hit_radius;
    }

    // --- render -------------------------------------------------------------
    // --- build the on-screen HUD for this frame -----------------------------
    const float fb_w = (float)sapp_width();
    const float fb_h = (float)sapp_height();
    sdtx_canvas(fb_w * 0.5f, fb_h * 0.5f);

    // Death cinematic overlay (np-ma2.2): a big centered "SHIP DESTROYED"
    // and the impending respawn target while the explosion blooms. Drawn
    // even under capture_clean so screenshots of the death moment carry
    // the banner. Columns = canvas_px / 8 (the debugtext cell width).
    if (dying) {
        const float cols = fb_w * 0.5f / 8.0f;
        const float rows = fb_h * 0.5f / 8.0f;
        sdtx_font(0);
        sdtx_color3f(1.0f, 0.25f, 0.18f);
        sdtx_pos(cols * 0.5f - 11.0f, rows * 0.5f - 1.0f);
        sdtx_puts("*** SHIP DESTROYED ***\n");
        sdtx_color3f(0.9f, 0.8f, 0.4f);
        if (!g.player.last_docked_base.empty()) {
            sdtx_pos(cols * 0.5f - 11.0f, rows * 0.5f + 1.0f);
            sdtx_printf("RESPAWNING AT %s\n", g.player.last_docked_base.c_str());
        } else {
            sdtx_pos(cols * 0.5f - 11.0f, rows * 0.5f + 1.0f);
            sdtx_puts("NO BASE ON RECORD - RESETTING\n");
        }
        sdtx_color3f(0.7f, 1.0f, 0.9f);   // restore default for later blocks
    }

    if (!g.capture_clean && !g.show_title) {
        // Top-centre FLIGHT panel is now drawn alongside the other
        // cockpit_hud MFDs (see the cockpit_hud::build call site below),
        // where simgui_new_frame() has already opened an ImGui frame.
        // sdtx text below (controls reminder etc.) runs independently.
        // Skipped entirely during the title screen (np-3dp.4) so the
        // bottom-left keybind list + perception dump stay hidden.

        sdtx_font(0);
        sdtx_color3f(0.7f, 1.0f, 0.9f);

        // Top-centre flight status block moved to cockpit_hud's boxed
        // FLIGHT MFD (see draw_flight_status_mfd above this block). The
        // old sdtx text here is intentionally removed.
        sdtx_color3f(0.7f, 1.0f, 0.9f);   // restore default for later blocks

        // Ship-sprite frame HUD. Prints, per placed ship sprite, the raw
        // camera-relative az/el AND the authored atlas frame the engine
        // actually picked. Toggled with F3. Hidden when there are no ship
        // sprites in the scene (no signal, just clutter).
        if (g.show_ship_frame_hud && !g.placed_ship_sprites.empty()) {
            sdtx_color3f(1.0f, 0.85f, 0.4f);
            sdtx_puts("\n");                       // 1 blank line gap

            // Player HP block. Sums per-facing shield + armor; if
            // anything's missing (no class) we just don't print.
            if (g.ships.player() && g.ships.player()->klass) {
                const Ship& pl = *g.ships.player();
                if (pl.alive) {
                    sdtx_printf("PLAYER  shield F%.0f A%.0f P%.0f St%.0f  armor F%.0f A%.0f P%.0f St%.0f\n",
                                pl.shield_fore_cm, pl.shield_aft_cm,
                                pl.shield_port_cm, pl.shield_starboard_cm,
                                pl.armor_fore_cm,  pl.armor_aft_cm,
                                pl.armor_port_cm,  pl.armor_starboard_cm);
                } else {
                    sdtx_puts("PLAYER  *** DEAD ***\n");
                }
            }

            sdtx_puts("SHIP SPRITE FRAMES (F3)\n");
            for (size_t i = 0; i < g.placed_ship_sprites.size(); ++i) {
                const ShipSpriteObject& s = g.placed_ship_sprites[i];
                const char* key = (s.atlas ? s.atlas->key.c_str() : "<no-atlas>");
                // Trim a long atlas key like "ships/talon/atlas_manifest"
                // down to the ship name segment so the HUD stays narrow.
                const char* slash1 = std::strchr(key, '/');
                const char* slash2 = slash1 ? std::strchr(slash1 + 1, '/') : nullptr;
                const char* short_key = slash1 ? slash1 + 1 : key;
                const size_t short_len = slash2 ? (size_t)(slash2 - short_key)
                                                : std::strlen(short_key);
                char short_buf[24];
                const size_t copy_len = short_len < sizeof(short_buf) - 1
                                        ? short_len : sizeof(short_buf) - 1;
                std::memcpy(short_buf, short_key, copy_len);
                short_buf[copy_len] = '\0';
                const char* tag = s.manual_frame_enabled ? " [MANUAL]" : "";
                // Perception column: H/A/N counts pulled from the OWNING
                // Ship, found by matching Ship::sprite back-pointers (the
                // old positional ships[i+1] lockstep is gone). Linear scan
                // per row — fine for a debug HUD at demo N. Empty string
                // when no Ship owns this sprite or it has no class. HP
                // column is sum-of-shields and sum-of-armor across all 3
                // facings — compact "how alive is this thing" readout.
                char percept_buf[32] = {0};
                char hp_buf[64] = {0};
                const Ship* owner = nullptr;
                for (const Ship& cand : g.ships) {
                    if (cand.sprite == &s) { owner = &cand; break; }
                }
                if (owner && owner->klass) {
                    const Ship& sh = *owner;
                    const ShipPerception& p = sh.perception;
                    std::snprintf(percept_buf, sizeof(percept_buf),
                                  " [H%d A%d N%d]",
                                  p.n_hostile, p.n_allied, p.n_neutral);
                    if (sh.alive) {
                        const float sh_sum = sh.shield_fore_cm + sh.shield_aft_cm + sh.shield_port_cm + sh.shield_starboard_cm;
                        const float ar_sum = sh.armor_fore_cm  + sh.armor_aft_cm  + sh.armor_port_cm  + sh.armor_starboard_cm;
                        std::snprintf(hp_buf, sizeof(hp_buf),
                                      " S%.0f A%.0f", sh_sum, ar_sum);
                    } else {
                        std::snprintf(hp_buf, sizeof(hp_buf), " DEAD");
                    }
                }
                sdtx_printf(" %zu %-10s cam(az %+4.0f el %+4.0f) -> cell(az %+4.0f el %+4.0f)%s%s%s\n",
                            i, short_buf,
                            s.debug_cam_az_deg, s.debug_cam_el_deg,
                            s.debug_last_az_deg, s.debug_last_el_deg,
                            tag, percept_buf, hp_buf);
            }
            sdtx_color3f(0.7f, 1.0f, 0.9f);   // restore default for any later block
        }

    }

    // --- draw ---------------------------------------------------------------
    const float aspect   = fb_w / fb_h;
    const float time_sec = (float)stm_sec(stm_now());

    // Procedural skybox generation (B1): render the seed's cubemap once, via
    // its own offscreen passes, BEFORE the scene pass opens (can't nest a
    // pass inside another). Idempotent after the first frame. Skipped under
    // capture_clean (sprite-atlas mode wants a pure black background and
    // doesn't draw the skybox anyway).
    if (!g.capture_clean) g.skybox.generate();

    // Pass 1: scene → offscreen. Same draw order as before, just a
    // different attachment. HUD is NOT drawn here — it goes over the
    // composited swapchain so post-process doesn't blur/bloom the text.
    {
        sg_pass pass{};
        pass.action = g.scene_pass_action;
        pass.attachments.colors[0]  = g.rt.scene_color_att;
        pass.attachments.depth_stencil = g.rt.scene_depth_att;
        sg_begin_pass(&pass);
        // 3rd-person orbit camera swap: while the nav autopilot is engaged
        // the whole 3D scene renders from g.orbit_cam (a camera orbiting the
        // ship), not the ship's-eye g.camera. The HUD below still uses
        // g.camera (real ship pose) for nav distance / FLIGHT readout. The
        // ship itself is rendered as a sprite further down so there's
        // something to look at.
        // Title screen rolls the view 180 deg around the forward axis so
        // the upside-down patrol sprites read right-side-up. Done on a
        // LOCAL COPY so g.camera (used by the sim + HUD) is never mutated
        // — the previous in-place version accumulated the roll each
        // frame and flickered (np-3dp).
        Camera title_cam;
        const Camera* scene_cam_ptr = g.orbit_active ? &g.orbit_cam : &g.camera;
        // Chase-cam variant supplies its own orbiting camera pose; use it
        // verbatim (no roll/swoosh — the orbit IS the motion, and the
        // camera moving through space is what makes the warp streaks flow).
        title_scene::ChaseConfig title_cc;
        if (g.show_title) {
            title_cc = title_scene::chase_config(
                g.camera.position, g.camera.forward(),
                g.camera.right(),  g.camera.up());
        }
        if (g.show_title && title_cc.cam_override) {
            title_cam = g.camera;
            title_cam.position    = title_cc.cam_pos;
            // Apply a 180 roll about the camera's LOCAL Z so the cruising
            // hull reads right-side-up, same convention as the patrol
            // sprites. Composing on the right rotates in local space.
            title_cam.orientation = HMM_NormQ(HMM_MulQ(
                title_cc.cam_orient,
                HMM_QFromAxisAngle_RH(HMM_V3(0, 0, 1), 3.14159265358979f)));
            scene_cam_ptr = &title_cam;
        } else if (g.show_title) {
            title_cam = g.camera;
            // Base settled orientation: the 180-deg forward-axis roll.
            const HMM_Quat settled = HMM_NormQ(HMM_MulQ(
                g.camera.orientation,
                HMM_QFromAxisAngle_RH(g.camera.forward(), 3.14159265358979f)));

            // Intro fly-in (np-3dp): over the first ~4s the camera swooshes
            // in — starts pulled back + swung off to the side + extra
            // banked, then eases to the settled pose. Ease-out cubic so it
            // decelerates as it arrives. After the intro it's a no-op
            // (t==1 -> zero offset, identity extra rotation).
            constexpr float k_intro_s = 4.0f;
            float t = s_title_elapsed / k_intro_s;
            if (t > 1.0f) t = 1.0f;
            const float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);  // ease-out cubic
            const float k = 1.0f - ease;   // 1 at start, 0 when settled

            const HMM_Vec3 fwd   = g.camera.forward();
            const HMM_Vec3 right = g.camera.right();
            const HMM_Vec3 up    = g.camera.up();
            // Start offset: 2500m back, 1800m to the right, 600m up.
            const HMM_Vec3 start_off =
                HMM_AddV3(HMM_MulV3F(fwd,   -2500.0f),
                          HMM_AddV3(HMM_MulV3F(right, 1800.0f),
                                    HMM_MulV3F(up,     600.0f)));
            title_cam.position = HMM_AddV3(g.camera.position,
                                           HMM_MulV3F(start_off, k));
            // Extra swing: yaw + bank that unwinds as we settle. Compose
            // BEFORE the settled roll so the unwinding reads as a turn
            // into frame rather than a spin.
            const HMM_Quat swing = HMM_NormQ(HMM_MulQ(
                HMM_QFromAxisAngle_RH(up,  -0.45f * k),     // yaw in from the right
                HMM_QFromAxisAngle_RH(fwd,  0.65f * k)));   // bank that levels out
            title_cam.orientation = HMM_NormQ(HMM_MulQ(swing, settled));
            scene_cam_ptr = &title_cam;
        }
        const Camera& scene_cam = *scene_cam_ptr;
        // Draw order rationale:
        //   1. skybox   — no depth write, paints the background
        //   2. dust     — additive particulate in "empty space"; drawn BEFORE
        //                 opaque geometry so rocks/sun paint over it cleanly.
        //                 (If drawn later, dust's depth test lets individual
        //                 near-camera specks sparkle on top of rock surfaces,
        //                 which reads as "holes" to the eye.)
        //   3. asteroids — opaque, writes depth; covers dust where rocks are.
        //   4. sun sphere— opaque, writes depth.
        //   5. sun gas + corona — additive halos, depth test but no write.
        if (!g.capture_clean) {
            g.skybox.draw(scene_cam, aspect);
            g.dust.draw(scene_cam, aspect);
            // Warp streaks layer over dust (additive). Self-gates on
            // intensity > 0 so this is cheap when autopilot is off.
            g.warp_streaks.draw(scene_cam, aspect);
            for (const auto& f : g.asteroid_fields) {
                f.draw(scene_cam, aspect, time_sec, g.sun.position, g.sun.core_color);
            }
        }
        // Publish the render matrices for the dev_remote /project endpoint
        // (used by tools/render_3d_sprite_atlases.py to project 3D nav-light
        // positions into each sprite cell). Only meaningful in the single-
        // mesh capture scenes, so we publish placed_meshes[0]'s transform;
        // harmless otherwise. model_matrix() is the same helper the mesh
        // renderer uses, so the projection matches the render exactly.
        if (!g.placed_meshes.empty()) {
            const PlacedMesh& pm0 = g.placed_meshes[0];
            const HMM_Mat4 vp    = HMM_MulM4(scene_cam.projection(aspect),
                                             scene_cam.view());
            const HMM_Mat4 model = model_matrix(pm0.position, pm0.euler_deg,
                                                pm0.scale);
            dev_remote::publish_render_matrices(vp, model, scene_cam.position);
        }
        // Skip placed meshes during title: the title scene owns the screen
        // and we want only star + skybox + patrol ships visible (np-3dp).
        if (!g.show_title) {
            g.mesh_render.draw(g.placed_meshes, scene_cam, aspect,
                               g.sun.position, g.sun.core_color);
        }
        // Sprites go AFTER opaque meshes and BEFORE the sun so the sun's
        // additive corona still paints on top of everything. Sprites use
        // alpha blending, which needs opaque depth already in the buffer
        // so translucent edges composite correctly.
        if (g.show_title) {
            // Title mode: skip placed sprites + NPC ships + player ship;
            // only the patrol ships should be in the sprite queue.
            g.frame_sprites.clear();
        } else {
            g.frame_sprites = g.placed_sprites;
            append_ship_sprites_for_camera(g.placed_ship_sprites, scene_cam, g.frame_sprites);
        }
        // Title scene patrol ships (np-3dp): feed canonical patrol ships
        // into the same sprite queue when the title is up. Their poses
        // are advanced by title_scene::tick(raw_dt) earlier in frame_cb.
        if (g.show_title) {
            title_scene::append_to_frame_sprites(scene_cam, g.frame_sprites);
        }
        // Player hull in 3rd-person: feed a one-shot ShipSpriteObject at the
        // ship's pose through the same frame-selection path. The camera uses
        // -Z forward while the sprite atlas uses +Z nose, so rotate the
        // orientation 180 deg around Y to match conventions. Skipped during
        // title so the player ship doesn't show up in the title scene.
        if (!g.show_title && g.orbit_active && g.player_atlas) {
            ShipSpriteObject& ps = g.player_ship_sprite;
            ps.atlas       = g.player_atlas;
            ps.position    = g.camera.position;
            ps.world_size  = 100.0f * world_scale::k_ship_size_scale;
            ps.orientation = HMM_NormQ(HMM_MulQ(
                g.camera.orientation,
                HMM_QFromAxisAngle_RH(HMM_V3(0.0f, 1.0f, 0.0f), 3.14159265358979f)));
            ps.angular_velocity = HMM_V3(0, 0, 0);
            ps.forward_speed    = 0.0f;
            g.player_sprite_scratch.clear();
            g.player_sprite_scratch.push_back(ps);
            append_ship_sprites_for_camera(g.player_sprite_scratch, scene_cam, g.frame_sprites);
        }
        g.sprite_render.draw(g.frame_sprites, scene_cam, aspect, time_sec);

        // Jump-gate spheres: translucent additive shells at every
        // kind=="jump" nav point. Drawn AFTER opaque ships/stations/rocks
        // so the depth-test correctly hides shell pixels where a hull is
        // in front of the gate, but BEFORE the additive sun/tracer/bolt
        // glow layer so the gate sits in the same translucent stack.
        // Capture-clean skips it for the same reason it skips tracers/etc.
        if (!g.capture_clean) {
            std::vector<HMM_Vec3> gate_positions;
            gate_positions.reserve(g.system.nav_points.size() + 1);
            if (g.show_title &&
                title_scene::variant() == title_scene::Variant::ChaseCam) {
                // Title galaxy tour (np-3dp.13): render ONLY the approaching
                // jump hole the hero ship is cruising toward. The loaded
                // system's own gates aren't relevant to the chase shot.
                gate_positions.push_back(title_scene::jump_hole_pos());
            } else {
                for (const NavPointDef& n : g.system.nav_points) {
                    if (n.kind == "jump") gate_positions.push_back(n.position);
                }
            }
            g.jump_gate.draw(scene_cam, aspect, time_sec, gate_positions);
        }

        // Additive glow billboards via the sprite spot pipeline.
        // Combines projectile tracers + explosion FX (flash + shockwave)
        // into one tracer list and submits in one draw call. HDR color
        // values >1.0 are intentional — bloom catches them and blooms
        // explosions look properly bright. Capture-clean mode skips these
        // dynamic effects so atlas reference renders contain only the
        // target object on the cleared background.
        if (!g.capture_clean) {
            std::vector<SpriteRenderer::Tracer> tracers;
            tracers.reserve(g.missiles.size()
                            + g.explosions.size() * 2);
            std::vector<SpriteRenderer::Bolt> bolts;
            bolts.reserve(g.projectiles.size());

            // Projectile bolts. Guns with extracted sprite art (all but
            // steltek) render as textured additive billboards; the laser
            // aligns its elongated ray with travel direction. Guns
            // without art fall back to the procedural glow below.
            for (const Projectile& p : g.projectiles) {
                if (!p.alive) continue;
                if (g.bolt_art.has_bolts(p.type)) {
                    SpriteRenderer::Bolt b;
                    b.position = p.position;
                    HMM_Vec3 v  = p.velocity;
                    float    vl = HMM_LenV3(v);
                    b.velocity_dir = vl > 0.0f
                        ? HMM_MulV3F(v, 1.0f / vl)
                        : HMM_V3(0, 0, 1);
                    b.texture_id = g.bolt_tex_offsets[(int)p.type]
                                 + g.bolt_art.frame_index(p.type, time_sec);
                    if (p.type == GunType::Laser) {
                        // Velocity-stretched 3D ray. size = thickness,
                        // beam_length = world-space dash length.
                        b.beam        = true;
                        b.size        = 12.0f;    // thickness (world units)
                        b.beam_length = 160.0f;   // dash length (world units)
                    } else {
                        b.size   = 5.0f;    // uniform sphere size, all guns
                        b.aspect = g.bolt_art.aspect(p.type);
                    }
                    bolts.push_back(b);
                } else {
                    const GunStats& gs = g_gun_stats[(int)p.type];
                    SpriteRenderer::Tracer t;
                    t.position = p.position;
                    t.color    = gs.tracer_color;
                    t.size     = 5.0f + p.damage_cm;
                    tracers.push_back(t);
                }
            }

            // Missile tracers (np-zte.2). Bigger + hotter than a bullet so a
            // missile reads as a distinct burning mote streaking toward its
            // mark; a faint per-type tint (DF white, HS orange, IR cyan)
            // hints at what's inbound. Reuses the same additive-glow path —
            // no bespoke missile mesh in v1.
            for (const Missile& m : g.missiles) {
                if (!m.alive) continue;
                SpriteRenderer::Tracer t;
                t.position = m.position;
                switch (m.type) {
                    case MissileType::HS: t.color = HMM_V3(2.4f, 1.2f, 0.5f); break;
                    case MissileType::IR: t.color = HMM_V3(0.7f, 1.8f, 2.4f); break;
                    default:              t.color = HMM_V3(2.2f, 2.2f, 2.0f); break;
                }
                t.size = 22.0f;
                tracers.push_back(t);
            }

            // Explosions: per-explosion two-layer composite.
            //   * Flash: small, near-white, exponentially decaying. Most
            //     of its life is concentrated in the first ~150ms.
            //   * Shockwave: large, expanding, orange. Reads as fireball.
            // Color values >1.0 push past the LDR clamp so bloom
            // amplifies; the bigger the value, the brighter the halo.
            for (const Explosion& e : g.explosions) {
                if (!e.alive) continue;
                const float p = (e.lifetime_s > 0.0f)
                    ? std::clamp(e.age_s / e.lifetime_s, 0.0f, 1.0f) : 1.0f;

                // Flash — exponential decay, peaks at t=0. The (1-p)
                // factor is a small linear taper so it fully extinguishes
                // by end-of-life rather than just asymptoting near zero.
                const float flash_i = std::exp(-p * 8.0f) * (1.0f - p);
                if (flash_i > 0.005f) {
                    SpriteRenderer::Tracer t;
                    t.position = e.position;
                    t.color = HMM_V3(3.5f * flash_i, 3.0f * flash_i, 2.4f * flash_i);
                    t.size  = 80.0f + 60.0f * p;   // small, slowly grows
                    tracers.push_back(t);
                }

                // (Shield flash drawing happens after the explosion
                // loop; see the next block.)

                // Shockwave — radius grows aggressively early then
                // plateaus (the (1 - exp(-3p)) curve). Intensity fades
                // linearly so the halo dims as it expands. Orange-red
                // tint shifts slightly toward red over time so the late
                // frames read as smouldering rather than bright fire.
                const float wave_size  = 50.0f + 450.0f * (1.0f - std::exp(-p * 3.0f));
                const float wave_i     = (1.0f - p) * 1.8f;
                if (wave_i > 0.005f) {
                    SpriteRenderer::Tracer t;
                    t.position = e.position;
                    t.color = HMM_V3(2.5f * wave_i,
                                      1.0f * wave_i * (1.0f - 0.5f * p),
                                      0.3f * wave_i * (1.0f - p));
                    t.size  = wave_size;
                    tracers.push_back(t);
                }
            }

            // Shield flashes — short cyan glow at the ship's center,
            // sized to the hit sphere. Intensity decays linearly with
            // a slight bias toward the front of life so a fresh hit
            // pops bright and fades fast. Color tuned to match the UI
            // shield bar (cyan-blue) so the player intuitively links
            // "flash" to "shield".
            for (const AppState::ShieldFlash& f : g.shield_flashes) {
                const float p = (f.lifetime_s > 0.0f)
                    ? std::clamp(f.age_s / f.lifetime_s, 0.0f, 1.0f) : 1.0f;
                const float intensity = (1.0f - p) * (1.0f - p) * 2.5f;
                if (intensity <= 0.005f) continue;
                SpriteRenderer::Tracer t;
                t.position = f.position;
                t.color = HMM_V3(0.4f * intensity,
                                  1.6f * intensity,
                                  2.6f * intensity);
                t.size = f.radius;
                tracers.push_back(t);
            }

            // Armor flashes — orange-red sparks at the hull. Smaller +
            // shorter than the shield bubble so it reads as "hits ON
            // the metal" rather than "shield envelope flickering".
            // Color matches the orange UI armor bar (and the explosion
            // shockwave palette) for visual consistency.
            for (const AppState::ArmorFlash& f : g.armor_flashes) {
                const float p = (f.lifetime_s > 0.0f)
                    ? std::clamp(f.age_s / f.lifetime_s, 0.0f, 1.0f) : 1.0f;
                const float intensity = (1.0f - p) * (1.0f - p) * 3.0f;
                if (intensity <= 0.005f) continue;
                SpriteRenderer::Tracer t;
                t.position = f.position;
                t.color = HMM_V3(2.8f * intensity,
                                  1.0f * intensity,
                                  0.2f * intensity);
                t.size = f.radius;
                tracers.push_back(t);
            }

            g.sprite_render.draw_tracers(tracers, scene_cam, aspect);
            if (!bolts.empty()) {
                g.sprite_render.draw_bolts(bolts, g.bolt_textures,
                                           scene_cam, aspect, time_sec);
            }
        }

        if (!g.capture_clean) {
            g.sun.draw(scene_cam, aspect, time_sec);
        }
        sg_end_pass();
    }

    // Pass 2+3: bright-pass + separable gaussian into bloom_b.
    // Capture-clean atlas screenshots need deterministic object-only refs,
    // not stale bloom texture ghosts or lens flare artifacts.
    if (!g.capture_clean) {
        g.post.apply_bloom(g.rt);
    }

    // Build the ImGui frame OUTSIDE any pass. This is only widget state;
    // no draw calls are issued yet. Slider mutations feed back into the
    // live PlacedMesh list so changes take effect on the *next* frame.
    debug_panel::build(g.placed_meshes, g.placed_ship_sprites, g.game,
                       g.ship_debug, g.audio_debug, g.player);

    // --- Live gun-mount tuner (dev tool) --------------------------------
    // Drag the player's muzzle offset_body values in real time, then hit
    // "copy JSON" to paste the tuned numbers straight into the hull's
    // ship.json default_guns. Flight-only; ImGui draw cmds get flushed by
    // the debug_panel::render() call later this frame.
    if (!g.show_title && g_show_mount_tuner) {
        if (Ship* pl = g.ships.player()) {
            ImGui::SetNextWindowSize(ImVec2(340.0f, 0.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Gun Mount Tuner", &g_show_mount_tuner)) {
                ImGui::TextDisabled("offset_body (m):  +X right   +Y down   -Z fwd");
                ImGui::Separator();
                for (size_t i = 0; i < pl->mounts.size(); ++i) {
                    ImGui::PushID((int)i);
                    ImGui::DragFloat3("", &pl->mounts[i].offset_body.X,
                                      0.1f, -60.0f, 60.0f, "%.2f");
                    ImGui::SameLine();
                    ImGui::Text("mount %d", (int)i);
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::Button("copy JSON")) {
                    std::string js = "\"default_guns\": [\n";
                    for (size_t i = 0; i < pl->mounts.size(); ++i) {
                        char line[160];
                        std::snprintf(line, sizeof(line),
                            "    { \"offset_body\": [%.2f, %.2f, %.2f], \"type\": \"mass_driver\" }%s\n",
                            pl->mounts[i].offset_body.X,
                            pl->mounts[i].offset_body.Y,
                            pl->mounts[i].offset_body.Z,
                            (i + 1 == pl->mounts.size()) ? "" : ",");
                        js += line;
                    }
                    js += "  ],";
                    ImGui::SetClipboardText(js.c_str());
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(paste into ship.json)");
            }
            ImGui::End();
        }
    }

    // Surface every ship-sprite atlas cell as an extra editable target so
    // F2 can author lights on individual frames (engine glow, nav strobes,
    // etc.). Built fresh each frame because cell pointers are stable but
    // membership in the dropdown should reflect any future hot-reload.
    std::vector<sprite_light_editor::EditableArt> ship_cell_targets;
    ship_cell_targets.reserve(g.ship_sprite_atlases.size() * 16);
    for (auto& [atlas_key, atlas] : g.ship_sprite_atlases) {
        for (auto& frame : atlas.frames) {
            // Cells inside an atlas live in the shared `sprite_art` cache,
            // but we don't want them shown twice if also placed standalone.
            // Filter by name: a placed sprite's stem won't end with `_cell`.
            if (!frame.art) continue;
            sprite_light_editor::EditableArt e;
            e.name = frame.art->name;
            e.art  = const_cast<SpriteArt*>(frame.art);
            ship_cell_targets.push_back(std::move(e));
        }
    }
    sprite_light_editor::build(g.placed_sprites, ship_cell_targets);
    // F5 — mesh orientation editor. Mutates PlacedMesh.euler_deg in place.
    mesh_orient_editor::build(g.placed_meshes);
    // F10 — navmap auditor. Read-only map/coordinate inspection across systems.
    navmap_auditor::build();
    // F4 — atlas grid viewer. Mutates ShipSpriteFrame fields directly,
    // so changes flow into the next render frame with no apply step.
    atlas_grid_viewer::build(g.ship_sprite_atlases);
    // F7 — sound labeler. Auditions + names the extracted SOUNDFX.PAK clips,
    // saving ground-truth labels to docs/sound_labels.json. Self-contained;
    // no game state to pass in.
    sound_labeler::build();
    // F8 — music labeler. Auditions + names the rendered AdLib music tracks,
    // saving ground-truth labels to docs/music_labels.json. Ducks the live
    // music layer while previewing; restores it on Stop/close. Self-contained.
    music_labeler::build();
    // F9 — speech labeler. Auditions + names the converted SPEECH.PAK clips,
    // saving ground-truth labels to docs/speech_labels.json. Self-contained.
    speech_labeler::build();
    // F6 — sprite-generation workbench. Front-end only; launches Python jobs.
    sprite_generation_tool::build();
    if (!g.capture_clean) {
        // Docking prompt for the NAV MFD (np-9cu.1). Probe can_request
        // against the selected nav: cleared -> "PRESS D TO DOCK" (green),
        // dockable-but-not-yet -> "DOCK: <reason>" (amber). Non-dockable
        // navs leave the line blank.
        const char* dock_prompt = nullptr;
        bool        dock_ready  = false;
        if (g.selected_nav >= 0 && g.selected_nav < (int)g.system.nav_points.size()) {
            const NavPointDef& nav = g.system.nav_points[g.selected_nav];
            if (nav.kind == "jump") {
                // Jump gate selected (np-6al.3): surface the J prompt in the
                // SAME NAV MFD slot dock uses — a gate is never also a dock
                // base, so they can't collide. jump::prompt feeds the green/
                // amber line straight off the eligibility verdict.
                const jump::Eligibility e = jump::evaluate(
                    g.camera, g.system, g.galaxy, g.player.current_system,
                    g.selected_nav, g.player.has_jump_drive);
                bool ready = false;
                dock_prompt = jump::prompt(e, &ready);
                dock_ready  = ready;
            } else {
                const DockResult r = docking::can_request(
                    g.docking, g.camera.position, g.camera.velocity, nav);
                static char buf[48];
                if (r == DockResult::Cleared) {
                    dock_prompt = "PRESS D TO DOCK";
                    dock_ready  = true;
                } else if (r != DockResult::NotDockable) {
                    std::snprintf(buf, sizeof(buf), "DOCK: %s", docking::result_str(r));
                    dock_prompt = buf;
                }
            }
        }
        // Skip the entire cockpit HUD while the title is up: the title
        // owns the screen and we want a clean star+ships background. The
        // sim is also frozen at this point (dt=0), so no overlay makes
        // sense anyway (np-3dp).
        if (!g.show_title) {
            // World glyphs (nav reticle + mission objective diamonds) are
            // hidden when autopilot owns the ship or the navmap overlay is
            // up — the autopilot HUD already shows what's en route, and
            // the navmap panel renders the same info textually.
            const bool draw_world =
                !autopilot::engaged(g.autopilot) && !g.show_navmap;
            cockpit_hud::build(g.camera, g.system, g.selected_nav,
                               g.mouse_x, g.mouse_y, g.fly_by_wire,
                               g.ships, g.player_target_id,
                               g.player_atlas,
                               dock_prompt, dock_ready,
                               draw_world);

            // Mission objective markers + progress readout (#18). Read-only
            // over the player's accepted missions + this system's nav set.
            cockpit_hud::build_mission_objectives(
                g.camera, g.system, g.player.current_system, g.player,
                draw_world);

            // Sun-proximity warning overlay (np-3dp). Centre-screen banner
            // when inside the 20k avoid bubble; big red "DESTRUCTION
            // IMMINENT" once inside the 15k damage zone.
            cockpit_hud::draw_sun_warning(g.camera, g.sun.position);
        }
        // floating sdtx text up at the HUD-build step ran before that.
        if (!g.show_title && !g.capture_clean) {
            const HMM_Vec3 pp = g.camera.position;
            cockpit_hud::FlightStatusHudState fs;
            fs.speed = HMM_LenV3(g.camera.velocity);
            fs.mode  = (g.camera.cruise_level > 0.5f)  ? "CRUISE"
                     : (g.camera.cruise_level > 0.05f) ? "SPOOL "
                     :                                   "NORMAL";
            fs.d_sun = HMM_LenV3(HMM_SubV3(g.sun.position, pp));
            fs.pos_x = pp.X; fs.pos_y = pp.Y; fs.pos_z = pp.Z;
            if (const Ship* pl = g.ships.player()) {
                fs.energy     = pl->energy_gj;
                fs.energy_max = pl->klass ? pl->klass->energy_max : 0.0f;
            }
            if (autopilot::engaged(g.autopilot))
                fs.autopilot_nav = g.autopilot.nav_name.c_str();
            if (g.autopilot.msg_timer_s > 0.0f)
                fs.autopilot_msg = g.autopilot.msg;
            cockpit_hud::draw_flight_status_mfd(fs);
        }
        // Weapons + ordnance status (np-zte.2). Afterburner fuel bar
        // removed — the energy bar in the STATUS panel already shows the
        // shared bank that drives both guns and the burner. Hidden during
        // the title screen (np-3dp.4).
        if (!g.show_title) {
            cockpit_hud::WeaponsHudState w;
            const MissileStats& sel = g_missile_stats[g.selected_missile];
            w.missile_name  = sel.short_name;
            w.missile_count = player::missile_count(g.player, g.selected_missile);
            // Torpedo uses its own label so the player can tell DUMBFIRE
            // (DF) and TORPEDO apart at a glance.
            if ((MissileType)g.selected_missile == MissileType::TORPEDO)
                w.no_lock_label = "TORPEDO";
            w.needs_lock    = sel.needs_lock;
            w.lock_state    = g.missile_lock.locked ? 2
                            : (g.player_target_id != 0 && sel.needs_lock ? 1 : 0);
            w.lock_progress = sel.lock_buildup ? (g.missile_lock.progress_s / 1.5f) : 1.0f;
            cockpit_hud::build_weapons_status(w);
        }
        // Big system navmap (N to open/cycle). Drawn AFTER the regular
        // HUD so it overlays on top. Mutates selected_nav when the
        // player clicks a nav point — same effect as the N-cycle.
        cockpit_hud::build_navmap(g.camera, g.system, g.selected_nav,
                                   g.ships, g.player, g.player.current_system,
                                   g.galaxy, g.show_navmap);
        // Reputation + comm-taunt feed (np-ma2.1), drawn over the HUD.
        // ---- HIDDEN TEMPORARILY (re-enable by uncommenting the line below) ----
        // comm::draw();
        (void)0;   // silence unused-function warnings when re-enabled

        // Alpha welcome/briefing overlay — drawn last so it sits on top of
        // the whole HUD. The sim is frozen (dt=0) while this is up.
        // Alpha welcome/briefing overlay — REMOVED in np-3dp. The title
        // screen owns the chrome now; the briefing text was redundant
        // and aged out of usefulness (np-3dp.4). Kept the helper for
        // potential debugging hooks but no longer called from the render
        // pass.
        // if (g.show_welcome) draw_welcome_overlay();

        if (g.show_title) {
            const title_screen::Action a = title_screen::draw();
            // Hyperspace flash overlay (np-3dp.13): a full-screen white
            // wash that ramps to peak as the ship reaches the jump hole,
            // then fades to reveal the new system. Driven by title_scene's
            // approach timing. Drawn on the foreground list so it covers
            // the chrome + scene. Squared for a snappier ease.
            const float jflash = title_scene::jump_flash();
            if (jflash > 0.0f) {
                const ImGuiViewport* vpf = ImGui::GetMainViewport();
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                const float a01 = jflash * jflash;
                const ImU32 wash = IM_COL32(255, 255, 255,
                                            (int)(a01 * 255.0f + 0.5f));
                fg->AddRectFilled(vpf->WorkPos,
                                  ImVec2(vpf->WorkPos.x + vpf->WorkSize.x,
                                         vpf->WorkPos.y + vpf->WorkSize.y),
                                  wash);
            }
            // Flight pause banner (np-pau.28). Drawn on the foreground
            // list so it sits on top of the HUD even mid-fight. Amber so
            // it reads as a UI element, not an in-fiction warning.
            if (g.paused) {
                const ImGuiViewport* vpf = ImGui::GetMainViewport();
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                const ImVec2 sz(280.0f, 44.0f);
                const ImVec2 pos(vpf->WorkPos.x + (vpf->WorkSize.x - sz.x) * 0.5f,
                                 vpf->WorkPos.y + 24.0f);
                fg->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y),
                                  IM_COL32(20, 16, 8, 220), 6.0f);
                fg->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y),
                            IM_COL32(255, 200, 60, 255), 6.0f, 0, 2.0f);
                fg->AddText(ImVec2(pos.x + 16.0f, pos.y + 12.0f),
                            IM_COL32(255, 220, 120, 255),
                            "PAUSED  -  press P to resume");
            }
            if (a != title_screen::Action::None) {
                if (a == title_screen::Action::NewGame) {
                    // Reset to a pristine new game (np-3dp.19): wipe
                    // credits / reputation / cargo / kills / missions back
                    // to the new_game baseline so NEW never inherits a
                    // prior session's (or a loaded save's) state, and heal
                    // the player hull to full. Keep the currently-loaded
                    // system as the start system.
                    g.player = player::new_game(g.system_name);
                    g.apply_health_pending = false;
                    // Refit the live ship to the fresh Tarsus + single-laser
                    // loadout (np-3dp.25) — not just heal — so NEW after a
                    // ship-swap save flies the stock starter hull/guns.
                    if (Ship* pl = g.ships.player()) apply_player_loadout(*pl, g.player);
                    // new_game starts docked at Achilles: drop into that
                    // base's concourse rather than free flight.
                    if (g.player.docked && !g.player.last_docked_base.empty())
                        game_state::request_mode(g.game, GameMode::Landed);
                    g.show_title = false;
                    // Rebuild the start system from scratch (np-3dp.24):
                    // the title galaxy tour reskins the skybox + repicks
                    // the sun as it flies through random systems, so NEW
                    // must restore the loaded system's CANONICAL sky/sun
                    // (and re-roll a fresh NPC wave + reset the camera to
                    // player_start) rather than inherit whatever the tour
                    // last landed on. pending_goto re-runs the canonical
                    // build_system_scene at the next frame boundary.
                    g.pending_goto = g.system_name;
                    std::printf("[title] NEW clicked — fresh game, rebuilding %s, entering flight\n",
                                g.system_name.c_str());
                } else if (a == title_screen::Action::LoadGame) {
                    // Open the save picker (np-3dp.19): a scrollable list of
                    // every accumulated save, newest first. Selection loads
                    // that file + enters flight (handled below).
                    g.show_load_menu = true;
                    std::printf("[title] LOAD clicked — opening save picker\n");
                } else if (a == title_screen::Action::Options) {
                    std::printf("[title] OPTIONS clicked — coming soon\n");
                } else if (a == title_screen::Action::Quit) {
                    std::printf("[title] QUIT clicked — request app quit\n");
                    sapp_request_quit();
                }
            }

            // Save picker (np-3dp.19): a centered modal listing every
            // accumulated save, newest first, each row the full timestamped
            // title. Click a row to load it + enter flight; Close/Esc backs
            // out to the menu. Rebuilt each frame from disk so a fresh
            // autosave shows up without a restart.
            if (g.show_load_menu) {
                const ImGuiViewport* vp = ImGui::GetMainViewport();
                ImGui::SetNextWindowPos(
                    ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                           vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(
                    ImVec2(vp->WorkSize.x * 0.7f, vp->WorkSize.y * 0.6f),
                    ImGuiCond_Always);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.03f, 0.05f, 0.96f));
                ImGui::PushStyleColor(ImGuiCol_Text,     ImVec4(1.0f, 0.78f, 0.24f, 1.0f));
                if (ImGui::Begin("LOAD GAME", nullptr,
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoCollapse)) {
                    const std::vector<savegame::SlotInfo> saves = savegame::list_saves();
                    if (saves.empty()) {
                        ImGui::TextUnformatted("No saves yet — land at a base to autosave.");
                    } else {
                        ImGui::Text("%zu save(s):", saves.size());
                        ImGui::Separator();
                        ImGui::BeginChild("save_list", ImVec2(0, -40), true);
                        int row_id = 0;
                        for (const savegame::SlotInfo& s : saves) {
                            // Per-row PushID: two saves made the same minute
                            // share a label, so the label alone is a
                            // colliding ImGui ID. The unique row index fixes
                            // it (np-3dp.19).
                            ImGui::PushID(row_id++);
                            if (ImGui::Selectable(s.label.c_str())) {
                                PlayerState restored;
                                if (savegame::load(restored, s.path)) {
                                    g.player = restored;
                                    g.apply_health_pending = g.player.hp_valid;
                                    // Re-target the world to the saved system
                                    // so the scene matches the save. ALWAYS
                                    // rebuild — even when the saved system is
                                    // the one already loaded — so the title
                                    // galaxy tour's reskinned skybox/sun get
                                    // reset to the system's canonical look
                                    // (np-3dp.24), not just when the id
                                    // differs.
                                    if (!g.player.current_system.empty()) {
                                        g.pending_goto = g.player.current_system;
                                    }
                                    // Resume WHERE you saved (np-3dp.21):
                                    // the autosave fires on dock, so a save
                                    // with a base lands you back in THAT
                                    // base's concourse — not adrift in space
                                    // at the system's default spawn. Manual
                                    // in-flight saves (no base) resume in
                                    // free flight.
                                    if (g.player.docked &&
                                        !g.player.last_docked_base.empty()) {
                                        game_state::request_mode(g.game, GameMode::Landed);
                                        std::printf("[title] LOAD -> %s (landed at %s)\n",
                                                    s.path.c_str(),
                                                    g.player.last_docked_base.c_str());
                                    } else {
                                        g.player.docked = false;
                                        game_state::request_mode(g.game, GameMode::Flight);
                                        std::printf("[title] LOAD -> %s (free flight)\n",
                                                    s.path.c_str());
                                    }
                                    g.show_title     = false;
                                    g.show_welcome   = false;
                                    g.show_load_menu = false;
                                }
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndChild();
                    }
                    ImGui::Separator();
                    if (ImGui::Button("Close", ImVec2(120, 28)) ||
                        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                        g.show_load_menu = false;
                    }
                }
                ImGui::End();
                ImGui::PopStyleColor(2);
            }
        }
    }

    // ---- ship-target indicator ------------------------------------
    // If the player has a target ship locked (T cycles through nearby
    // contacts), draw a screen-space marker. On-screen targets get a
    // four-corner bracket; off-screen ones get an arrow on the screen
    // edge pointing toward where they are. Text below shows class
    // name + distance + faction stance. Drawn into the simgui
    // foreground draw list so it renders on top of everything.
    if (!g.capture_clean && g.player_target_id != 0 && g.ships.player()) {
        const Ship* target = g.ships.find_by_id(g.player_target_id);
        if (target && target->alive) {
            // Engine quirks (mirror cockpit_hud.cpp's well-tested path):
            //   1. ImGui draw lists work in LOGICAL pixels; sapp_width/
            //      _height return PHYSICAL (HiDPI-scaled) pixels, so
            //      divide by sapp_dpi_scale() to get the logical canvas.
            //   2. Our perspective pipeline maps world-up to +screen_y
            //      already (no NDC Y-flip needed), so the textbook
            //      `(1 - ndc_y) * 0.5` inversion would put the bracket
            //      vertically MIRRORED from the ship.
            const float dpi  = sapp_dpi_scale();
            const float fb_w = (float)sapp_width()  / dpi;
            const float fb_h = (float)sapp_height() / dpi;
            const float aspect_loc = fb_w / fb_h;

            // Use the LIVE sprite position when available — the Ship's
            // `position` field is synced from the sprite at frame start,
            // before kinematic integration. By render time the sprite
            // has moved one tick further, and reading sprite->position
            // keeps the bracket locked on the visual ship instead of
            // lagging it by a frame.
            const HMM_Vec3 target_pos = target->sprite ? target->sprite->position
                                                       : target->position;
            const HMM_Mat4 vp_loc = HMM_MulM4(g.camera.projection(aspect_loc), g.camera.view());
            const HMM_Vec4 clip   = HMM_MulM4V4(vp_loc,
                HMM_V4(target_pos.X, target_pos.Y, target_pos.Z, 1.0f));
            const bool behind     = clip.W < 0.0f;

            // NDC. When the target is BEHIND the camera, dividing by a
            // negative W produces a sign-flipped projection — the
            // "true" screen direction is the NEGATIVE of what the
            // divide gives. Negating restores the geometric direction
            // so off-screen arrows point correctly. (My first cut here
            // wrote `-clip.X / -clip.W` which is algebraically the same
            // as `clip.X / clip.W` — a no-op bug.)
            float ndc_x = clip.X / clip.W;
            float ndc_y = clip.Y / clip.W;
            if (behind) { ndc_x = -ndc_x; ndc_y = -ndc_y; }
            const bool offscreen = behind
                                || std::fabs(ndc_x) > 1.0f
                                || std::fabs(ndc_y) > 1.0f;

            // Stance from the player's perception entry for this ship.
            // Drives indicator color: red=hostile, yellow=neutral,
            // green=allied. Default red if not in perception (e.g. the
            // moment after acquisition before the next perception tick).
            ImU32 col_hostile = IM_COL32(255,  80,  80, 255);
            ImU32 col_neutral = IM_COL32(255, 220,  60, 255);
            ImU32 col_allied  = IM_COL32( 80, 255,  80, 255);
            ImU32 color = col_hostile;
            float distance_m = HMM_LenV3(HMM_SubV3(target->position, g.ships.player()->position));
            const ShipPerception& pp = g.ships.player()->perception;
            for (const PerceivedContact& c : pp.visible) {
                if (c.ship_id == g.player_target_id) {
                    distance_m = c.distance_m;
                    color = (c.stance == Stance::Hostile) ? col_hostile
                          : (c.stance == Stance::Allied)  ? col_allied
                          :                                  col_neutral;
                    break;
                }
            }

            ImDrawList* dl = ImGui::GetForegroundDrawList();
            if (!offscreen) {
                // NDC -> screen. Engine convention: NDC y maps DIRECTLY
                // to screen y (no `(1 - ndc_y)` flip — see cockpit_hud's
                // header notes for the documented quirk).
                const float sx = (ndc_x * 0.5f + 0.5f) * fb_w;
                const float sy = (ndc_y * 0.5f + 0.5f) * fb_h;
                const float r  = 30.0f;
                const float k  = 10.0f;
                const float th = 2.0f;
                // 8 line segments, two per corner (L shape).
                dl->AddLine(ImVec2(sx-r, sy-r), ImVec2(sx-r+k, sy-r), color, th);
                dl->AddLine(ImVec2(sx-r, sy-r), ImVec2(sx-r, sy-r+k), color, th);
                dl->AddLine(ImVec2(sx+r, sy-r), ImVec2(sx+r-k, sy-r), color, th);
                dl->AddLine(ImVec2(sx+r, sy-r), ImVec2(sx+r, sy-r+k), color, th);
                dl->AddLine(ImVec2(sx-r, sy+r), ImVec2(sx-r+k, sy+r), color, th);
                dl->AddLine(ImVec2(sx-r, sy+r), ImVec2(sx-r, sy+r-k), color, th);
                dl->AddLine(ImVec2(sx+r, sy+r), ImVec2(sx+r-k, sy+r), color, th);
                dl->AddLine(ImVec2(sx+r, sy+r), ImVec2(sx+r, sy+r-k), color, th);

                // Label below the bracket.
                char buf[96];
                const char* tname = target->klass ? target->klass->name.c_str()
                                  : target->is_player ? "player" : "?";
                std::snprintf(buf, sizeof(buf), "%s   %.1f km", tname, distance_m * 0.001f);
                dl->AddText(ImVec2(sx - r, sy + r + 6.0f), color, buf);
            } else {
                // Off-screen: arrow on screen-edge box pointing in the
                // (ndc_x, ndc_y) direction from screen center. Margin
                // pulls the arrow inward so it doesn't get clipped.
                const float cx = fb_w * 0.5f;
                const float cy = fb_h * 0.5f;
                const float margin = 80.0f;
                const float half_w = fb_w * 0.5f - margin;
                const float half_h = fb_h * 0.5f - margin;
                const float dxlen = std::sqrt(ndc_x * ndc_x + ndc_y * ndc_y);
                if (dxlen > 1e-6f) {
                    const float dx = ndc_x / dxlen;
                    const float dy = ndc_y / dxlen;
                    // Scale to land on the rectangular edge. NO Y-flip
                    // here either — same engine convention as the
                    // bracket path: NDC y already maps to screen y.
                    const float scale = std::min(
                        half_w / std::max(std::fabs(dx), 1e-6f),
                        half_h / std::max(std::fabs(dy), 1e-6f));
                    const float ax = cx + dx * scale;
                    const float ay = cy + dy * scale;
                    const float arrow = 14.0f;
                    const float perp_x = -dy;
                    const float perp_y =  dx;
                    ImVec2 tip(   ax + dx * arrow,            ay + dy * arrow);
                    ImVec2 base_l(ax + perp_x * arrow * 0.6f, ay + perp_y * arrow * 0.6f);
                    ImVec2 base_r(ax - perp_x * arrow * 0.6f, ay - perp_y * arrow * 0.6f);
                    dl->AddTriangleFilled(tip, base_l, base_r, color);

                    // Label tucked just inside the arrow toward center.
                    char buf[96];
                    const char* tname = target->klass ? target->klass->name.c_str()
                                      : target->is_player ? "player" : "?";
                    std::snprintf(buf, sizeof(buf), "%s  %.1f km", tname, distance_m * 0.001f);
                    const float lx = ax - dx * 60.0f - 30.0f;
                    const float ly = ay - dy * 60.0f - 7.0f;
                    dl->AddText(ImVec2(lx, ly), color, buf);
                }
            }

            // ---- ITTS (Improved Targeting and Tracking System) -------
            // Wing-Commander-style lead reticle: where to aim to hit the
            // moving target given the player's projectile flight time.
            // Compute the target's world-frame velocity (forward * speed
            // for NPCs that have a sprite), the average projectile
            // speed across the player's mounts, and predict an
            // intercept point one iteration deep:
            //
            //   t_int = |target - me| / proj_speed
            //   lead  = target_pos + target_vel * t_int
            //
            // Single-pass is within a few metres at our engagement
            // ranges; a two-pass refinement (re-evaluate distance from
            // lead) tightens it further but isn't worth the math.
            //
            // Drawn ONLY when on-screen (in the camera's view frustum)
            // — an off-screen ITTS would be confusing because it isn't
            // the target itself, just where to aim. Off-screen targets
            // already have the directional arrow above.
            if (!offscreen) {
                const Ship& player = *g.ships.player();
                HMM_Vec3 t_pos = target->sprite ? target->sprite->position
                                                 : target->position;
                HMM_Vec3 t_vel = HMM_V3(0, 0, 0);
                if (target->sprite) {
                    const HMM_Mat4 tR  = HMM_QToM4(target->orientation);
                    const HMM_Vec4 tf  = HMM_MulM4V4(tR, HMM_V4(0, 0, 1, 0));
                    t_vel = HMM_MulV3F(HMM_V3(tf.X, tf.Y, tf.Z),
                                        target->sprite->forward_speed);
                }

                // Average player projectile speed (skip null-stat guns).
                float proj_speed = 1100.0f;
                {
                    int n_complete = 0; float sum = 0.0f;
                    for (const auto& m : player.mounts) {
                        if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
                        const GunStats& gs = g_gun_stats[(int)m.type];
                        if (!gs.complete) continue;
                        sum += gs.speed_mps; ++n_complete;
                    }
                    if (n_complete > 0) proj_speed = sum / n_complete;
                }

                const HMM_Vec3 to_t = HMM_SubV3(t_pos, player.position);
                const float    dist = std::sqrt(HMM_DotV3(to_t, to_t));
                const float    t_int = (proj_speed > 1.0f) ? dist / proj_speed : 0.0f;
                const HMM_Vec3 lead = HMM_AddV3(t_pos, HMM_MulV3F(t_vel, t_int));

                // Project lead to screen with the same vp_loc + Y-quirk
                // we used for the target bracket.
                const HMM_Vec4 lc = HMM_MulM4V4(vp_loc,
                    HMM_V4(lead.X, lead.Y, lead.Z, 1.0f));
                if (lc.W > 0.0f) {
                    // Project regardless of frustum bounds — at long
                    // range with fast lateral targets the lead point
                    // can land outside the view frustum even though
                    // the target itself is on-screen, and the previous
                    // |ndc| <= 1 gate hid the reticle exactly when the
                    // player needed it most. ImGui clips to its window
                    // anyway, so an off-screen reticle just won't be
                    // visible (no further hiding needed).
                    const float lndc_x = lc.X / lc.W;
                    const float lndc_y = lc.Y / lc.W;
                    const float lsx = (lndc_x * 0.5f + 0.5f) * fb_w;
                    const float lsy = (lndc_y * 0.5f + 0.5f) * fb_h;
                    // Reticle: open circle + small inset crosshair,
                    // bright green so it pops against any backdrop.
                    const ImU32 itts_col = IM_COL32(140, 255, 140, 230);
                    dl->AddCircle(ImVec2(lsx, lsy), 9.0f, itts_col, 16, 1.5f);
                    dl->AddLine(ImVec2(lsx - 5, lsy),
                                ImVec2(lsx + 5, lsy), itts_col, 1.0f);
                    dl->AddLine(ImVec2(lsx, lsy - 5),
                                ImVec2(lsx, lsy + 5), itts_col, 1.0f);
                }
            }
        } else {
            // Target died or fell off the world — clear so next T press
            // starts fresh from the nearest current contact.
            g.player_target_id = 0;
        }
    }

    // ---- player damage vignette ------------------------------------
    // Red screen-edge gradient when the player took damage recently.
    // Built from four edge rectangles, each with a multi-color
    // gradient: red+alpha at the screen edge, transparent toward the
    // center. Reads as a ring of damage glow without needing a
    // dedicated post-process shader. Intensity decays exponentially
    // every frame; sustained fire keeps refreshing it so the player
    // sees a steady red ring while being hit.
    if (!g.capture_clean && g.player_hit_intensity > 0.005f) {
        const float dpi = sapp_dpi_scale();
        const float w   = (float)sapp_width()  / dpi;
        const float h   = (float)sapp_height() / dpi;
        const float band = std::min(w, h) * 0.18f;   // band thickness ~18% of min dim
        const int   alpha = (int)std::clamp(g.player_hit_intensity * 200.0f, 0.0f, 200.0f);
        const ImU32 hit  = IM_COL32(255, 30, 30, alpha);
        const ImU32 zero = IM_COL32(255, 30, 30, 0);

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        // Top band — red along screen-top edge, fades downward.
        dl->AddRectFilledMultiColor(ImVec2(0,    0),    ImVec2(w, band),
                                    hit, hit, zero, zero);
        // Bottom band — red along screen-bottom edge, fades upward.
        dl->AddRectFilledMultiColor(ImVec2(0,    h-band), ImVec2(w, h),
                                    zero, zero, hit, hit);
        // Left band — red along screen-left edge, fades rightward.
        dl->AddRectFilledMultiColor(ImVec2(0,    0),    ImVec2(band, h),
                                    hit, zero, zero, hit);
        // Right band — red along screen-right edge, fades leftward.
        dl->AddRectFilledMultiColor(ImVec2(w-band, 0),  ImVec2(w, h),
                                    zero, hit, hit, zero);
    }

    // Pass 4: composite to swapchain with lens flare + HUD + ImGui
    // overlay — all in ONE swapchain pass (Metal only tolerates one
    // drawable acquisition per frame; a second pass was flickering).
    const HMM_Mat4 vp = HMM_MulM4(g.camera.projection(aspect), g.camera.view());
    const HMM_Vec3 flare_tint = g.sun.glow_color;
    g.post.composite_to_swapchain(g.rt, g.sun.position, vp, flare_tint,
                                  sapp_width(), sapp_height(),
                                  [] { debug_panel::render(); });

    sg_commit();

    // End-of-frame hook for the dev remote. If a /screenshot is pending
    // this takes it now — after sg_commit the final composited frame is
    // on the macOS window, so the PNG captures what the user sees.
    dev_remote::maybe_capture_screenshot();

    // --- HUD in the terminal ------------------------------------------------
    g.frames_since++;
    const double elapsed = stm_sec(stm_diff(now, g.last_fps_ticks));
    if (elapsed >= 1.0) {
        const HMM_Vec3 p = g.camera.position;
        const float    speed = HMM_LenV3(g.camera.velocity);
        const float    dist_to_sun = HMM_LenV3(HMM_SubV3(g.sun.position, p));
        const char*    mode = (g.camera.cruise_level > 0.5f) ? "CRUISE"
                            : (g.camera.cruise_level > 0.05f) ? "spool"
                            : "normal";
        const int fps = (int)(g.frames_since / elapsed);
        if (!g.placed_ship_sprites.empty()) {
            const ShipSpriteObject& ship = g.placed_ship_sprites.front();
            std::printf("[new_privateer] %4d fps  %6s  v=%7.1f u/s  d(sun)=%.0f  pos=(%.0f,%.0f,%.0f)  ship_frame=(az %.0f el %.0f)\n",
                        fps, mode, speed, dist_to_sun,
                        p.X, p.Y, p.Z,
                        ship.debug_last_az_deg, ship.debug_last_el_deg);
        } else {
            std::printf("[new_privateer] %4d fps  %6s  v=%7.1f u/s  d(sun)=%.0f  pos=(%.0f,%.0f,%.0f)\n",
                        fps, mode, speed, dist_to_sun,
                        p.X, p.Y, p.Z);
        }
        dev_remote::publish_fps(fps);
        g.last_fps_ticks = now;
        g.frames_since   = 0;
    }
}

void cleanup_cb() {
    dev_remote::stop();
    audio::shutdown();
    sdtx_shutdown();
    g.post.destroy();
    g.rt.destroy();
    debug_panel::shutdown();
    atlas_grid_viewer::shutdown();
    sprite_generation_tool::shutdown();
    for (auto& pm : g.placed_meshes) {
        // Mesh::destroy also frees every Material's textures — no more
        // per-placement teardown now that textures live on the mesh.
        pm.mesh.destroy();
    }
    g.mesh_render.destroy();
    for (auto& [_, art] : g.sprite_art) art.destroy();
    g.sprite_render.destroy();
    g.bolt_art.destroy();
    for (auto& f : g.asteroid_fields) f.destroy();
        g.dust.destroy();
        g.warp_streaks.destroy();
        g.jump_gate.destroy();
    g.sun.destroy();
    g.skybox.destroy();
    sg_shutdown();
    // Buffered logger (issue #26): join the flush thread and close the log
    // file before sokol teardown finishes — after this point we lose any
    // log lines that didn't get drained.
    tracelog::shutdown();
}

void event_cb(const sapp_event* ev) {
    // Track mouse-button state FIRST, before any ImGui / dev-editor handler
    // can early-return and swallow the event. Firing is a core flight input
    // and must never be eaten by debug tooling or an ImGui hover. (HUD
    // windows are NoInputs so they don't capture, but the debug panel and
    // dev editors do — without this, opening one silently kills the guns.)
    if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
        if (ev->mouse_button == 0) g.mouse_left_held  = true;
        if (ev->mouse_button == 1) g.mouse_right_held = true;
    } else if (ev->type == SAPP_EVENTTYPE_MOUSE_UP) {
        if (ev->mouse_button == 0) g.mouse_left_held  = false;
        if (ev->mouse_button == 1) g.mouse_right_held = false;
    }

    // 3rd-person orbit zoom: scroll wheel changes camera distance while the
    // autopilot freelook camera is active. Handled up top so ImGui/editor
    // handlers can't swallow it; clamped in update_orbit_camera.
    if (g.orbit_active && ev->type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
        g.orbit_dist *= (ev->scroll_y > 0.0f) ? 0.90f : 1.111f;
    }

    // Welcome / alpha-briefing overlay: REMOVED in np-3dp. The title
    // screen now owns the chrome and the briefing was redundant. We just
    // swallow key-downs while title is up (below).

    // Title screen owns input entirely. New press must come from one of
    // the buttons — keyboard / window events are all swallowed so a
    // stray tap can't accidentally start the game.
    if (g.show_title) {
        if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) return;   // eat keys while up
    }

    // Give ImGui first crack at the event. If the panel is focused or the
    // mouse is over a widget it'll swallow the input; we only forward to
    // the camera / keymap when it doesn't.
    // Light editor sees events first so its F2 toggle beats any widget
    // focus or ImGui input capture in debug_panel. Same reason for
    // putting the F4 atlas grid viewer ahead of debug_panel.
    if (sprite_light_editor::handle_event(ev)) return;
    if (atlas_grid_viewer::handle_event(ev)) return;
    // F7 — sound labeler. Ahead of debug_panel so the toggle beats ImGui
    // focus, same as the F2/F4 tools.
    if (sound_labeler::handle_event(ev)) return;
    // F8 — music labeler. Ahead of debug_panel so the toggle beats ImGui
    // focus, same as the F4/F7 tools.
    if (music_labeler::handle_event(ev)) return;
    // F9 — speech labeler. Same as the F7/F8 tools.
    if (speech_labeler::handle_event(ev)) return;
    if (sprite_generation_tool::handle_event(ev)) return;
    // F5 — live PlacedMesh orientation slider. Sits ahead of debug_panel
    // so the F5 toggle works even when an ImGui window has focus.
    if (mesh_orient_editor::handle_event(ev))    return;
    // F10 — navmap auditor. Same focus-beating toggle behavior.
    if (navmap_auditor::handle_event(ev))        return;
    if (debug_panel::handle_event(ev)) return;

    // Non-Flight modes: the sim is paused, so game input is ignored.
    // Escape is the lone affordance — request a return to Flight (the
    // transition lands at the top of the next frame_cb). The dev-editor
    // and ImGui handlers above still see events so the panels stay usable.
    if (g.game.mode != GameMode::Flight) {
        if (ev->type == SAPP_EVENTTYPE_KEY_DOWN &&
            ev->key_code == SAPP_KEYCODE_ESCAPE) {
            // Landed: Escape first backs out of a sub-screen to the
            // Concourse (np-9cu.4); on the Concourse it falls through to
            // the docking launch path (np-9cu.1) — places the ship off
            // the pad, clears the docked flag, arms the re-dock cooldown,
            // and requests Flight. Dying/Loading keep the plain np-eag.2
            // debug return-to-Flight.
            if (g.game.mode == GameMode::Landed) {
                if (!base_screens::handle_escape()) {
                    docking::launch(g.docking, g.camera, g.game, g.player, g.sun.position);
                }
            } else if (g.game.mode == GameMode::Dying) {
                // Death cinematic owns the transition (auto-respawn at
                // k_death_cinematic_s). Swallow Escape so a panicked tap
                // can't dump the player into Flight as a dead wreck.
            } else {
                game_state::request_mode(g.game, GameMode::Flight);
            }
        }
        return;
    }

    switch (ev->type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
        if (ev->key_code == SAPP_KEYCODE_ESCAPE) {
            const uint64_t now  = stm_now();
            const double   gap  = g.escape_armed_ticks
                                ? stm_sec(stm_diff(now, g.escape_armed_ticks))
                                : 999.0;
            if (gap < 1.0) {
                sapp_request_quit();
            } else {
                g.escape_armed_ticks = now;
                std::printf("[new_privateer] escape armed — tap again within 1s to quit\n");
            }
        }
        // N — cycle target through nav_points. KEY_DOWN (not keys_down
        // polled per frame) so a single press advances exactly one slot,
        // not however-many frames the key was physically held.
        // Pressing N with the navmap closed opens it; with it open the
        // map stays up and N cycles inside (matching the outside-map
        // behaviour, so the player can pick a target visually). Esc or
        // the X button still closes.
        if (ev->key_code == SAPP_KEYCODE_N) {
            if (g.show_navmap) {
                // Map already up — let the per-frame N-cycle inside
                // cockpit_hud::build_navmap handle advancing. Doing it
                // here too would double-cycle on every press.
            } else {
                // Map closed — open it on this press (no cycle yet).
                g.show_navmap = true;
                std::printf("[navmap] OPEN\n");
            }
        }
        // A — toggle the nav autopilot (np-opa.3). KEY_DOWN edge so one
        // press = one toggle. Engaged → cancel (hand control back); idle
        // → try to engage toward the selected nav. try_engage handles the
        // no-nav and hostile-gate refusals (stashing the HUD banner +
        // logging); we just route the press. Meaningless outside Flight,
        // and event_cb already early-returns for non-Flight modes above.
        if (ev->key_code == SAPP_KEYCODE_A) {
            if (autopilot::engaged(g.autopilot)) {
                autopilot::disengage(g.autopilot, g.camera,
                                     "AUTOPILOT DISENGAGED");
                std::printf("[autopilot] cancelled by pilot (A)\n");
            } else {
                autopilot::try_engage(g.autopilot, g.camera, g.system,
                                      g.selected_nav);
            }
        }
        // G — cycle gun arm-mode (np-3dp). Modes: 0=unarmed, 1=mesons
        // only, 2=ionics only, 3=all. Each press advances one step and
        // wraps. The mode is stored on Ship::gun_mode_idx; firing.cpp
        // consults Ship::gun_armed[i] to gate which mounts can fire.
        // If a mode would target zero mounts (e.g. mode 1 when no
        // mesons are fitted) it still flips the bits and just lets the
        // player see nothing happen — better than skipping and
        // desyncing the cycle.
        if (ev->key_code == SAPP_KEYCODE_G && g.ships.player()) {
            Ship& p = *g.ships.player();
            // Cycle through {UNARMED, [one mode per unique gun type], ALL}.
            // Mode list is rebuilt from the ship's CURRENT mount list so it
            // adapts as the player buys/sells guns in outfitting.
            const int modes = firing::gun_mode_count_for_mounts(p.mounts);
            if (modes > 0) {
                p.gun_mode_idx = (uint8_t)((p.gun_mode_idx + 1) % modes);
                firing::apply_gun_mode(p, p.gun_mode_idx);
            }
            // Match on-fire HUD: if the player isn't holding the trigger,
            // force fire_guns off so the cycle is unambiguous.
            if (!g.keys_down[SAPP_KEYCODE_X] && !g.keys_down[SAPP_KEYCODE_TAB]) {
                p.controller.fire_guns = false;
            }
            const auto& u  = firing::gun_unique_types_cache(p.mounts);
            const char* lbl = firing::gun_mode_label(u, p.gun_mode_idx);
            std::printf("[guns] mode=%u (%s) -- %zu mount(s)\n",
                        p.gun_mode_idx, lbl, p.mounts.size());
            sfx::ui_click();
        }
        // D — request docking at the selected nav point (np-9cu.1).
        // Strafe moved off D to Q/E (np-opa.3), so D is now a clean
        // docking-only key: this down-edge only DOES anything when the
        // nav is a base and we're cleared. Rejections log their reason
        // for HUD/console feedback.
        if (ev->key_code == SAPP_KEYCODE_D &&
            g.selected_nav >= 0 &&
            g.selected_nav < (int)g.system.nav_points.size()) {
            docking::request(g.docking, g.camera.position, g.camera.velocity,
                             g.system.nav_points[g.selected_nav]);
        }
        // J — jump through the selected jump gate (np-6al.3). Twin of the D
        // docking key: down-edge only, only acts when the selected nav is a
        // surveyed jump point we're cleared to take (in range, no hostiles).
        // On success we queue the destination + arrival gate and flip to the
        // Loading hyperspace cinematic; execute_jump() does the warp once the
        // flash has held. Refusals log their reason (the HUD already shows it
        // via the jump prompt). Re-checks eligibility here so a stale prompt
        // frame can't smuggle through an out-of-range / under-fire jump.
        if (ev->key_code == SAPP_KEYCODE_J && g.autopilot.phase == AutopilotPhase::Idle) {
            const jump::Eligibility e = jump::evaluate(
                g.camera, g.system, g.galaxy, g.player.current_system,
                g.selected_nav, g.player.has_jump_drive);
            if (e.status == jump::Status::Ready) {
                const char* src_nav = g.system.nav_points[g.selected_nav].name.c_str();
                std::printf("[jump] %s -> %s via %s (%.0fu out) — engaging\n",
                            g.player.current_system.c_str(), e.dest_id.c_str(),
                            src_nav, e.distance_m);
                g.pending_jump_system = e.dest_id;
                g.pending_jump_nav    = e.arrival_nav;
                sfx::jump();
                game_state::request_mode(g.game, GameMode::Loading);
            } else if (e.status != jump::Status::NotJumpNav) {
                if (e.status == jump::Status::NoRoute) {
                    const std::string& nm = g.system.nav_points[g.selected_nav].name;
                    std::fprintf(stderr, "[jump-dbg] REFUSE NOROUTE: current_system='%s' "
                                 "g.system.name='%s' selected_nav=%d nav='%s' "
                                 "galaxy(sys=%zu,jumps=%zu) direct_lookup=%d\n",
                                 g.player.current_system.c_str(), g.system.name.c_str(),
                                 g.selected_nav, nm.c_str(),
                                 g.galaxy.systems.size(), g.galaxy.jumps.size(),
                                 (int)g.galaxy.jump_target(g.player.current_system, nm).ok);
                }
                std::printf("[jump] refused at %s: %s\n",
                            g.system.nav_points[g.selected_nav].name.c_str(),
                            jump::status_str(e.status));
            }
        }
        // T — cycle target through nearby ships (player's perception).
        // Sorted by distance ascending so repeated presses sweep nearest
        // -> farthest -> wrap. Picking up the cycle from "no target"
        // selects the nearest contact; if the current target has fallen
        // out of perception range since last frame, the search-by-id
        // fails and we restart at index 0.
        if (ev->key_code == SAPP_KEYCODE_T && g.ships.player()) {
            const Ship& player = *g.ships.player();
            std::vector<PerceivedContact> sorted;
            sorted.reserve(player.perception.visible.size());
            // Hard 15 km cap: contacts past that are off-radar and not
            // lockable. Same number that drops a stale lock per-frame
            // up in the firing block — keep the two in lockstep.
            for (const PerceivedContact& c : player.perception.visible) {
                if (c.distance_m <= 15000.0f) sorted.push_back(c);
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](const PerceivedContact& a, const PerceivedContact& b) {
                          return a.distance_m < b.distance_m;
                      });
            if (sorted.empty()) {
                g.player_target_id = 0;
                std::printf("[target] no contacts in range\n");
            } else {
                int cur = -1;
                for (size_t i = 0; i < sorted.size(); ++i) {
                    if (sorted[i].ship_id == g.player_target_id) {
                        cur = (int)i; break;
                    }
                }
                const int next = (cur + 1) % (int)sorted.size();
                g.player_target_id = sorted[next].ship_id;
                sfx::ui_click();
                // Look up the ship to print a friendly name.
                const char* name = "?";
                if (const Ship* s = g.ships.find_by_id(g.player_target_id); s) {
                    name = s->klass ? s->klass->name.c_str()
                         : s->is_player ? "player" : "?";
                }
                std::printf("[target] → %s (id=%u, %.0f m)\n",
                            name, g.player_target_id, sorted[next].distance_m);
            }
        }
        // ENTER — fire the selected missile (np-zte.2). Edge-triggered:
        // key_repeat suppressed so holding it doesn't dump the whole rack;
        // we just RAISE a request here and frame_cb spawns it where the
        // player pose is fresh. The ammo/lock checks live there too.
        if (ev->key_code == SAPP_KEYCODE_ENTER && !ev->key_repeat) {
            g.missile_fire_request = true;
        }
        // M — cycle the selected missile type (DF -> HS -> IR -> DF). Pure
        // UI state; resets the lock so switching to a lock type re-acquires.
        if (ev->key_code == SAPP_KEYCODE_RIGHT_BRACKET && !ev->key_repeat) {
        // ']' cycles the sim time scale: 1x -> 2x -> 4x -> 8x -> 1x.
        g_time_scale = (g_time_scale >= 8.0f) ? 1.0f : g_time_scale * 2.0f;
        std::printf("[time_scale] sim now %.0fx wall time\n", g_time_scale);
    }
    if (ev->key_code == SAPP_KEYCODE_LEFT_BRACKET && !ev->key_repeat) {
        // '[' resets to 1x immediately.
        g_time_scale = 1.0f;
        std::printf("[time_scale] sim reset to 1x\n");
    }
    if (ev->key_code == SAPP_KEYCODE_M && !ev->key_repeat) {
            g.selected_missile = (g.selected_missile + 1) % kMissileTypeCount;
            g.missile_lock = AppState::MissileLock{};   // fresh lock for the new type
            sfx::ui_click();
            std::printf("[missile] selected %s (x%d)\n",
                        missile::to_name((MissileType)g.selected_missile),
                        g.player.missiles[g.selected_missile]);
        }
        // F3 — toggle the ship-sprite frame HUD. Useful while flying around a
        // sprite ship: lets you see exactly which atlas cell the engine picks
        // for your current camera angle, and how close the snapped cell is
        // to the raw camera direction.
        if (ev->key_code == SAPP_KEYCODE_F3) {
            g.show_ship_frame_hud = !g.show_ship_frame_hud;
            std::printf("[hud] ship-frame HUD %s\n",
                        g.show_ship_frame_hud ? "on" : "off");
        }
        // F4 — toggle the dev Gun Mount Tuner overlay (off by default).
        if (ev->key_code == SAPP_KEYCODE_F4 && !ev->key_repeat) {
            g_show_mount_tuner = !g_show_mount_tuner;
            std::printf("[mount-tuner] %s\n", g_show_mount_tuner ? "on" : "off");
        }
        // SPACE — toggle fly-by-wire vs free-cursor mode. Hides/shows
        // the OS cursor in lockstep so the visual matches the input
        // semantics without needing an extra polling check elsewhere.
        if (ev->key_code == SAPP_KEYCODE_SPACE) {
            g.fly_by_wire = !g.fly_by_wire;
            sapp_show_mouse(!g.fly_by_wire);
            std::printf("[input] fly-by-wire %s\n",
                        g.fly_by_wire ? "ENGAGED" : "PAUSED (cursor free)");
        }
        // P — toggle Flight pause (np-pau.28). Edge-triggered so a held
        // key can't strobe. Only valid in Flight mode — in Landed the
        // sim is already frozen by the mode itself, and in Dying/Loading
        // the player should be on the cinematic, not a pause overlay.
        if (ev->key_code == SAPP_KEYCODE_P && !ev->key_repeat &&
            g.game.mode == GameMode::Flight && !g.show_title) {
            g.paused = !g.paused;
            std::printf("[pause] sim %s\n", g.paused ? "PAUSED" : "RESUMED");
        }
        if ((size_t)ev->key_code < g.keys_down.size()) g.keys_down[ev->key_code] = true;
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        if ((size_t)ev->key_code < g.keys_down.size()) g.keys_down[ev->key_code] = false;
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE: {
        // Convert framebuffer (HiDPI) px to logical px for everything
        // downstream — same convention as ImGui drawlist coords and
        // sapp_width()/dpi-corrected screen size used in cockpit_hud.
        const float dpi = sapp_dpi_scale();
        g.mouse_x = ev->mouse_x / dpi;
        g.mouse_y = ev->mouse_y / dpi;
        break;
    }
    default:
        break;
    }
}

} // namespace

#ifdef _WIN32
#include "platform/win32_desktop_res.h"
#endif

namespace {

sapp_desc make_app_desc() {
    sapp_desc desc{};
    desc.init_cb      = init_cb;
    desc.frame_cb     = frame_cb;
    desc.cleanup_cb   = cleanup_cb;
    desc.event_cb     = event_cb;
    desc.width        = 1280;
    desc.height       = 800;
    static std::string title = "new_privateer — " + g.system_name;
    desc.window_title = title.c_str();
    desc.high_dpi     = true;
    desc.sample_count = kSceneSampleCount;   // MSAA off → matches offscreen
    desc.logger.func  = slog_func;
    return desc;
}

} // namespace

sapp_desc sokol_main(int argc, char** argv) {
    // _IONBF (unbuffered) so every printf flushes immediately — same
    // practical effect as line-buffering for our diagnostic prints,
    // and unlike _IOLBF (size>=2 required) it accepts size=0/null buf,
    // which MSVC's setvbuf strictly enforces. The old _IOLBF/size=0
    // combo was the cause of the silent-exit-with-0xC0000409 on the
    // first Windows port: setvbuf hit invalid_parameter -> __fastfail.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fprintf(stderr, "[trace] sokol_main entered, argc=%d\n", argc);
    std::fflush(stderr);

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--system") == 0 && i + 1 < argc) {
            g.system_name     = argv[i + 1];
            g.system_explicit = true;   // explicit --system wins over a saved system
            ++i;
        } else if (std::strcmp(argv[i], "--ship") == 0 && i + 1 < argc) {
            g_player_ship_override = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--capture-clean") == 0) {
            g.capture_clean = true;
        } else if (std::strcmp(argv[i], "--skip-title") == 0) {
            g.skip_title_at_boot = true;   // dev: drop straight into flight
        } else if (std::strcmp(argv[i], "--dev-land") == 0 && i + 1 < argc) {
            g.dev_land_base = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--dev-missions") == 0) {
            g.dev_seed_missions = true;   // accept a few generated jobs on boot
        } else if (std::strcmp(argv[i], "--load") == 0 && i + 1 < argc) {
            g.load_slot = std::atoi(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--continue") == 0) {
            g.load_slot = savegame::k_autosave_slot;   // resume the autosave
        } else if (std::strcmp(argv[i], "--dev-kill-at") == 0 && i + 1 < argc) {
            g.dev_kill_at_s = (float)std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--goto") == 0 && i + 1 < argc) {
            // Deferred single switch to <system> a couple seconds after boot
            // — proves runtime system switching WITHOUT the jump mechanic.
            g.goto_system = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--goto-at") == 0 && i + 1 < argc) {
            g.goto_at_s = (float)std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--goto-soak") == 0 && i + 1 < argc) {
            // Cycle the galaxy's systems <n> times then quit cleanly — the
            // resource-leak / crash soak for repeated teardown+rebuild.
            g.soak_remaining = std::atoi(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--goto-interval") == 0 && i + 1 < argc) {
            g.soak_interval = (float)std::atof(argv[i + 1]);
            ++i;
        } else if (std::strcmp(argv[i], "--dev-jump-soak") == 0 && i + 1 < argc) {
            // np-6al.3: auto-fire the J jump through the first surveyed gate
            // <n> times on an interval (teleporting into range first), then
            // quit cleanly. Exercises the full jump path — eligibility,
            // Loading cinematic, reciprocal arrival, repeated teardown+build
            // — headlessly, so the round-trip + leak/stability soak can run
            // without a human at the J key. Ping-pongs Troy<->Pyrenees.
            g.dev_jump_remaining = std::atoi(argv[i + 1]);
            g.show_welcome = false;   // soak is headless; don't freeze on the briefing
            ++i;
        } else if (std::strcmp(argv[i], "--dev-jump-interval") == 0 && i + 1 < argc) {
            g.dev_jump_interval = (float)std::atof(argv[i + 1]);
            ++i;
        }
    }

    sapp_desc desc = make_app_desc();

#ifdef _WIN32
    int desktop_w = 0, desktop_h = 0;
    if (win32::desktop_resolution(&desktop_w, &desktop_h)) {
        desc.width      = desktop_w;
        desc.height     = desktop_h;
        desc.fullscreen = true;
        std::fprintf(stderr, "[launch] using desktop resolution %dx%d fullscreen\n",
                     desktop_w, desktop_h);
        std::fflush(stderr);
    }
#endif

    return desc;
}
// 1781714421
// touch 1781714977133978000
// 1781714994388554000
// 1781715631188585000
// 1781715641475509000

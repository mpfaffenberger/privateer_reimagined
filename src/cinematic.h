#pragma once
// -----------------------------------------------------------------------------
// cinematic.h — in-engine cutscene director (Phase 1).
//
// A data-driven, hot-reloadable cutscene player modelled on the scripted-
// encounter director (scripted_encounters.h): it loads a timeline of cues
// from `assets/cinematics/<id>.json`, ticks it per-frame, and drives the
// camera on Catmull-Rom splines, spawns/animates real ships through the
// shared encounters::SpawnFn seam, plays audio by filepath, and layers a
// 2D overlay (letterbox bars, fades, subtitles, comm-VDU portrait panels).
//
// See docs/cinematic_format.md + assets/cinematics/schema.json for the DSL.
//
// -------------------------- how it gates on the world -----------------------
// A cinematic runs as an OVERLAY on Flight — exactly like the death cam
// (main.cpp: the render camera swaps to a director-owned Camera while
// active, and pilot input is muted). We deliberately did NOT add a
// GameMode::Cinematic: the mode machine already treats Flight as "the sim
// is live", and a cutscene wants the live world (real spawned ships) under
// its scripted camera. Rules:
//
//   * A cinematic only STARTS from Flight (play() is a no-op otherwise).
//   * While active(), the director owns the camera EXCLUSIVELY — the host
//     renders the scene from camera() instead of the ship/orbit camera.
//   * The death cinematic (GameMode::Dying) INTERRUPTS a cinematic: the
//     host stops() the cutscene the frame it enters Dying. A cinematic
//     never interrupts the death cam.
//
// -------------------------- degradation policy ------------------------------
// Missing / unparseable cinematic files, missing PNGs, missing audio: log
// one line and no-op. Same "log + carry on" contract as voice::load /
// comm::load / scripted::load. A cinematic never crashes the game.
//
// NO network / AI / image-gen code lives here or anywhere in the engine:
// the game only ever LOADS PNGs by path. All portrait art is pre-generated
// by the Python tooling in a later phase.
// -----------------------------------------------------------------------------

#include "encounters.h"   // encounters::SpawnFn / SpawnRequest

#include "HandmadeMath.h"

#include <functional>
#include <string>

struct Camera;
class ShipRegistry;
struct PlayerState;

namespace cinematic {

struct Outcome;   // cinematic_parse.h — post-cinematic world state (Phase A2)

// Parse the cinematic file at `path` into the module's single active slot,
// WITHOUT starting playback. Idempotent; clears any prior parse. Missing /
// unparseable file is NON-fatal (logs one line and leaves the slot empty).
// Phase 2's /cinematic/reload re-calls this mid-play to iterate live.
void load(const std::string& path);

// Load `assets/cinematics/<id>.json` and begin playback from t=0. Returns
// false (and no-ops) if the file didn't parse or a cinematic is already
// active. The CALLER guarantees we're in Flight (main.cpp checks the mode).
bool play(const std::string& id);

// Same, but takes an explicit file path (any location). Used by debug hooks.
bool play_file(const std::string& path);

// Read ONLY the optional top-level "location" block (entry-point system +
// nav) of `assets/cinematics/<id>.json` WITHOUT touching the active slot or
// starting playback. The HOST calls this before every play so it can
// teleport the player to the authored backdrop first (queueing a system
// switch when the location is cross-system). Both out-params are "" when no
// location was authored. Returns false + sets `err` for a missing /
// unparseable file (the same failures play() would hit). Never throws.
bool peek_location(const std::string& id, std::string& out_system,
                   std::string& out_nav, std::string& err);

// Stop the active cinematic immediately: kill its music bed, clear the
// overlay, release camera ownership, and DESPAWN the actors the director
// spawned so the world returns to its pre-cinematic population (repeated
// plays must not pile up ships). Routes through the same teardown path as a
// natural/skipped end. No-op when nothing is playing.
void stop();

// True while a cinematic is playing (camera + overlay are live).
bool active();

// The id of the active cinematic ("" when inactive) — for HUD / logs /
// Phase-2 status.
const char* current_id();

// Current timeline position in seconds (0 when inactive). Phase-2 status.
float time();

// Total duration in seconds of the loaded cinematic (last cue's effect end).
// Meaningful whether or not it's playing (a loaded-but-stopped file still
// reports its length). Phase-2 status.
float duration();

// The last load/parse/validation error ("" once a load succeeds). Phase-2
// surfaces this in /cinematic/status so the authoring agent gets structured
// feedback without scraping the game log.
const char* last_error();

// Advance the timeline by dt. Fires due cues, evaluates the active
// camera/actor splines, and expires finished line/subtitle overlays.
// Spawns/animates actors via `spawn` (the same recipe the encounter
// director uses). No-op when inactive, so the host can call it every
// Flight frame unconditionally. Ends the cinematic when the last cue's
// effect elapses or an `end` cue fires (running its plot actions).
void tick(float dt, ShipRegistry& ships, PlayerState& player,
          const encounters::SpawnFn& spawn);

// The camera the director currently commands. Only meaningful while
// active(); the host renders the scene from this pose.
const Camera& camera();

// World-space velocity of the director camera (from its spline motion), or
// zero when inactive / not moving. main.cpp feeds this to the WarpStreaks
// field so the "cruise streaks" flow along the cinematic camera's travel —
// the same motion cue autopilot uses, but driven by the scripted camera
// instead of player speed + autopilot engagement.
HMM_Vec3 camera_velocity();

// Jump the active timeline to `t` seconds (the Phase-2 /cinematic/seek
// iterate primitive). Rewinds latches and SILENTLY fast-forwards to `t`:
// it re-establishes the camera cue + re-spawns any actors due by `t` (via
// `spawn`, skipping actors already live), and restarts the music bed, but
// does NOT replay one-shot sfx/voice/subtitles or run `end` plot actions —
// a scrub shouldn't spam the world. Cues after `t` fire normally on the
// next tick. Returns false (err set) when nothing is playing. Runs on the
// main thread (needs the live registry), same as tick().
bool seek(float t, ShipRegistry& ships, PlayerState& player,
          const encounters::SpawnFn& spawn, std::string& err);

// Re-parse the named cinematic from disk (empty id = the current/last one)
// so edits to the JSON take effect live — the core Phase-2 iterate loop.
// If a cinematic is playing it stays playing and is re-established at its
// current timeline position (via seek). Returns false (err set) on a
// missing file / parse error — the caller surfaces `err` in the HTTP reply.
// Runs on the main thread.
bool reload(const std::string& id, ShipRegistry& ships, PlayerState& player,
            const encounters::SpawnFn& spawn, std::string& err);

// Register a sink for cinematic beat events (Phase 2). The director calls
// it on the main thread at every meaningful beat with a compact, machine-
// parseable line ("started id=intro dur=12.00", "line speaker=grayson ...",
// "ended reason=natural t=12.34"). main.cpp wires it to
// dev_remote::push_event("cinematic", text). No-op sink by default, so the
// engine never learns dev_remote exists.
void set_event_tap(std::function<void(const std::string&)> fn);

// Register the host's despawn recipe. The director calls it for each actor it
// spawned on teardown (natural end / skip / stop) and on backward seeks. It
// must free BOTH the registry ship AND its sprite slot; otherwise the sprite
// lingers in the render pool as a "ghost" after the cinematic ends and piles
// up across repeated plays. main wires it to encounter_despawn.
void set_despawn_hook(encounters::DespawnFn fn);

// Register the host's outcome applier (Cinematic Studio Phase A2 — same
// seam pattern as set_despawn_hook). Fired from the single teardown path
// ONLY when the cinematic authored an "outcome" block AND it ended
// naturally or was skipped (a skip must not strand the story); an external
// /cinematic/stop never applies it. Called AFTER the director's own actors
// are despawned and the overlay/camera released, so the hook's teleport +
// encounter_spawn groups land in a clean post-cinematic world. main.cpp
// implements teleport-to-nav + spawn groups; the engine stays world-agnostic.
void set_outcome_hook(std::function<void(const Outcome&)> fn);

// LIVE camera offset override (Cinematic Studio): while a cinematic is
// playing, the Studio panel can push a new follow-cam offset for the
// currently-active camera_path cue. The override is cleared on each
// camera cue change (hard cut) so it only affects the current shot.
// Pass {0,0,0} to clear. No-op when nothing is playing.
void set_follow_offset_override(HMM_Vec3 offset);
void clear_follow_offset_override();

// Request a skip. Honored only if the active cinematic is `skippable`
// (else ignored). Runs pending `end` actions before teardown so skipping
// cannot strand plot progression or make one-shot cinematics repeat.
// Wire this to the skip key (Esc) while active.
void skip(PlayerState& player);

// Draw the 2D overlay for this frame (letterbox, fade, subtitles, portrait
// panels) via the ImGui foreground draw list. Call from the HUD/overlay
// pass with the framebuffer size. No-op when inactive.
void draw_overlay(float fb_w, float fb_h);

} // namespace cinematic

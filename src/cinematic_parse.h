#pragma once
// -----------------------------------------------------------------------------
// cinematic_parse.h — the PURE (engine-free) half of the cutscene director.
//
// This is the parsed data model + the JSON->Cue translation, split out of
// cinematic.cpp so it can be:
//   * unit-tested headlessly (no imgui / sokol / audio / GL) — see
//     tools/test_cinematic.cpp, and
//   * kept small (the director .cpp was over the 600-line ceiling).
//
// It depends ONLY on json.h + HandmadeMath. It NEVER throws: every field is
// read through the defaulted find()/*_or() accessors, so a malformed or
// truncated cinematic degrades to "as much as we could parse" + an error
// string, exactly the voice::load / comm::load "log + carry on" contract.
// The heavy playback/overlay code lives in cinematic.cpp.
// -----------------------------------------------------------------------------

#include "json.h"

#include <HandmadeMath.h>

#include <string>
#include <vector>

namespace cinematic {

// One authored beat. `line` is the composite portrait+subtitle+voice cue.
enum class Cmd { FadeIn, FadeOut, Music, Sfx, CameraPath, Spawn, ActorPath,
                 Line, Subtitle, End };

struct CamKey {
    HMM_Vec3    pos{ 0, 0, 0 };
    bool        look_is_ship = false;
    std::string look_actor;               // when look_is_ship
    HMM_Vec3    look_pt{ 0, 0, 0 };        // when !look_is_ship
    bool        has_look = false;
};

struct Cue {
    float t   = 0.0f;
    Cmd   cmd = Cmd::Subtitle;
    float dur = 1.0f;

    // camera_path follow mode: when follow_actor is set, the camera position
    // = live ship.position + cam_keys[0].pos (offset) every frame, naturally
    // matching the ship's velocity and heading. The look_at should also
    // target the same ship for a proper chase-cam close-up.
    std::string follow_actor;              // "" = spline mode (default)

    // audio (music / sfx)
    std::string file;
    bool        has_pos = false;
    HMM_Vec3    pos{ 0, 0, 0 };

    // camera_path
    std::vector<CamKey> cam_keys;
    bool                ease_smooth = true;

    // spawn / actor_path
    std::string           actor, klass, faction, display_name;
    std::vector<HMM_Vec3> actor_keys;

    // line / subtitle
    std::string speaker, portrait, text, voice_file;
    bool        side_left = true;

    // end
    std::vector<std::string> actions;

    bool fired = false;   // one-shot cues (music/sfx/spawn/end/voice) latch this
};

// ---- outcome (Cinematic Studio Phase A2) ------------------------------------
// The world state a cutscene leaves behind, parsed from the OPTIONAL top-level
// "outcome" block (docs/cinematic_studio.md §2). Applied by the HOST via
// cinematic::set_outcome_hook when a cinematic ends naturally or is skipped
// (never on an external stop) — a skip must not strand the story.

struct OutcomeSpawn {
    std::string klass;                 // ship_class::find key, e.g. "stiletto"
    std::string faction = "civilian";  // faction::from_name key
    int         count   = 1;
    bool        hostile = false;       // additionally aggro the player
    // Optional plot flag to stamp once THIS spawned group is fully gone
    // (all tracked ids dead / despawned). Lets a follow-up cinematic trigger
    // on "pirates cleared" after an outcome-spawned battle.
    std::string cleared_flag;
};

struct Outcome {
    bool                      present = false;   // was an "outcome" block authored?
    std::string               player_at_nav;     // "" = leave the player be
    // Optional exact drop point (world coords). Takes precedence over
    // player_at_nav — for finales that end AWAY from any nav (e.g. an ambush
    // 100k out in the approach corridor).
    bool                      has_player_pos = false;
    HMM_Vec3                  player_pos{ 0, 0, 0 };
    std::vector<OutcomeSpawn> spawns;
};

// ---- location (entry-point, Cinematic Studio follow-up) ---------------------
// The OPTIONAL top-level "location" block: the system + nav a cinematic was
// authored against. The HOST teleports the player there (queueing a system
// switch when needed) BEFORE starting playback, so the scene always runs on
// its intended backdrop and the outcome's player_at_nav can't silently no-op
// in the wrong system. Parsed here (engine-free); applied in main.cpp.

struct Location {
    bool        present = false;   // was a "location" block authored?
    std::string system;            // galaxy id, e.g. "penders_star" ("" = here)
    std::string nav;               // nav name in that system, e.g. "Asteroid Field"
};

struct Cinematic {
    std::string      id;
    bool             letterbox = true;
    bool             skippable = true;
    std::vector<Cue> cues;
    Outcome          outcome;          // optional post-cinematic world state
    Location         location;         // optional entry-point (pre-play teleport)
    bool             valid = false;
};

// Read a JSON [x,y,z] array (any absent/wrong-typed element defaults to 0).
HMM_Vec3 read_vec3(const json::Value& v);

// Parse an "outcome" block into `out`. NEVER throws: a non-object `v` (or
// any missing/mistyped field) degrades to defaults; spawn entries without a
// "class" are skipped. Sets out.present = true only for an object block, so
// the host can distinguish "no outcome authored" from "empty outcome".
void parse_outcome(const json::Value& v, Outcome& out);

// Parse a "location" block into `out`. NEVER throws: a non-object `v` (or
// any missing/mistyped field) degrades to defaults. Sets out.present = true
// only for an object block, mirroring parse_outcome's contract.
void parse_location(const json::Value& v, Location& out);

// Translate one timeline entry into `out`. Returns false (out untouched or
// partial) for a non-object / missing-cmd / unknown-cmd entry — the caller
// skips it. NEVER throws: every optional field uses a defaulted lookup.
bool parse_cue(const json::Value& v, Cue& out);

// Parse a whole (already-decoded) cinematic document into `out`. Fills id /
// letterbox / skippable, translates + time-sorts the timeline, sets
// out.valid = true on success. Returns false + sets `err` only when `root`
// isn't a JSON object (an empty / end-only timeline is valid). NEVER throws.
bool parse_document(const json::Value& root, const std::string& default_id,
                    Cinematic& out, std::string& err);

} // namespace cinematic

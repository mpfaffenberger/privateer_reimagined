#pragma once
// -----------------------------------------------------------------------------
// cinematic_studio_io.h — the PURE (engine-free) half of the Cinematic Studio
// panel (Phase B, docs/cinematic_studio.md §3).
//
// Everything file-shaped lives here: the request-JSON writer (the contract
// consumed by Phase C's studio_bridge.py), the requests/responses directory
// join, the voice-profile table, and the cinematic-file listing. Split out of
// cinematic_studio.cpp for the same two reasons cinematic_parse was split out
// of cinematic.cpp:
//   * headless testability — this TU depends ONLY on json.h + <filesystem>,
//     so tools/test_cinematic.cpp round-trips the writer with zero engine
//     deps (no imgui / sokol / GL);
//   * the panel .cpp stays under the 600-line ceiling.
//
// Writing policy: a tiny hand-rolled emitter (json.h deliberately doesn't
// emit). Strings go out as raw UTF-8 — only the JSON-mandated escapes
// (quote, backslash, control chars) are applied, NEVER \uXXXX for printable
// text, so briefs with em-dashes stay human-readable in the request file.
// Reading policy: json.h with the non-throwing find()/*_or() accessors —
// a malformed response degrades to "pending-looking", never a crash.
// -----------------------------------------------------------------------------

#include <string>
#include <vector>

namespace cinematic::studio_io {

// Where the Studio protocol lives (docs §3). Requests are written to
// kStudioDir + "/requests", responses read from kStudioDir + "/responses".
inline constexpr const char* kStudioDir = "assets/cinematics/studio";

// ---- request (UI -> bridge) -------------------------------------------------

// One per-line tweak for a kind:"refine" request. Only fields flagged
// `has_*` are emitted — the bridge treats absence as "keep what's there".
// `index` counts LINE cues only (0 = first `line` in the timeline), matching
// the regen index space in docs §3.
struct LineOverride {
    int  index = -1;
    bool has_text            = false;  std::string text;
    bool has_emotion         = false;  std::string emotion;
    bool has_portrait_extra  = false;  std::string portrait_prompt_extra;
    bool has_voice_id        = false;  std::string voice_id;
    bool has_speed           = false;  float       speed = 1.0f;
    bool has_seed_image      = false;  std::string seed_image;

    bool any() const {
        return has_text || has_emotion || has_portrait_extra ||
               has_voice_id || has_speed || has_seed_image;
    }
};

// The full request document (docs §3). kind:"author" fills the *_text
// fields; kind:"refine" fills line_overrides + regen lists. Optional keys
// (empty text fields, empty override/regen lists) are OMITTED from the
// emitted JSON so the bridge never has to distinguish "" from absent.
struct Request {
    std::string id;                       // "req_<unixtime>"
    std::string kind = "author";          // "author" | "refine"
    std::string cinematic_id;             // target (refine) / suggested (author)
    std::string brief;
    std::string triggers_text;
    std::string outcome_text;
    std::string image_quality = "medium"; // "low" | "medium" | "high"
    std::string image_style_extra;
    std::vector<LineOverride> line_overrides;
    std::vector<int> regen_portraits;     // indices into line cues
    std::vector<int> regen_voices;
};

// Serialize per docs §3 (2-space pretty print, trailing newline). Pure.
std::string request_to_json(const Request& r);

// Write `r` to <studio_dir>/requests/<r.id>.json, creating directories as
// needed. Returns false + sets `err` on any IO failure. Never throws.
bool write_request(const Request& r, const std::string& studio_dir,
                   std::string& err);

// ---- request/response listing (bridge -> UI) --------------------------------

// One row in the panel's Requests tab: the request file joined with its
// response file (if any). status defaults to "pending" until the bridge
// writes responses/<id>.json.
struct RequestEntry {
    std::string id;                 // filename stem, e.g. "req_1730000000"
    std::string kind;               // from the request file
    std::string cinematic_id;       // response's wins over the request's
    long long   mtime = 0;          // request file mtime (unix secs) for "age"
    std::string status = "pending"; // pending | working | done | error
    std::string message;            // bridge's human-readable status line
};

// Scan <studio_dir>/{requests,responses} and join by id, newest first.
// Missing directories / unreadable files degrade to an empty (or partial)
// list — never throws. The CALLER throttles (this walks the filesystem).
std::vector<RequestEntry> scan_requests(const std::string& studio_dir);

// Delete <studio_dir>/requests/<id>.json AND responses/<id>.json (either
// may be absent). Returns true if at least one file was removed.
bool delete_request(const std::string& studio_dir, const std::string& id);

// ---- voice profiles + cinematic listing --------------------------------------

// One entry of assets/data/voice_profiles.json (a TOP-LEVEL ARRAY, docs §4).
struct VoiceProfile {
    std::string id;      // MiniMax cloned voice id, e.g. "PrivFlightV1501"
    std::string label;   // human combo label, e.g. "Pirate male — raspy taunt"
    std::string flavor;  // tooltip text
};

// Load the profile table. Missing/malformed file = empty list (log-and-
// carry-on policy), entries without an id are skipped.
std::vector<VoiceProfile> load_voice_profiles(const std::string& path);

// List cinematic ids: *.json stems directly in `dir`, minus the non-
// cinematic residents (schema.json, triggers.json). Sorted. Missing dir =
// empty list.
std::vector<std::string> list_cinematic_ids(const std::string& dir);

// Guess the character folder from a line cue's portrait path
// ("portraits/pirate/ambush_troy_01.png" -> "pirate"; "" on no match).
// Used to build the seed-image helper path portraits/<char>/_ref.png.
std::string character_from_portrait(const std::string& portrait_path);

} // namespace cinematic::studio_io

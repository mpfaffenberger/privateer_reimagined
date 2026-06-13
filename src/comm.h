#pragma once
// -----------------------------------------------------------------------------
// comm.h — faction comm chatter + reputation HUD feed (np-ma2.1).
//
// Two lightweight, UI-facing jobs that ride on top of the reputation
// math in faction.{h,cpp}:
//
//   1. A DATA-DRIVEN taunt table (assets/data/comm_lines.json) keyed by
//      faction + event type. pick_line() returns one random flavour line
//      ("Confed Patrol: 'Nice shooting, civilian.'"). The table is the
//      source of truth; the engine ships none of these strings inline.
//
//   2. A rolling on-screen COMM FEED: short-lived lines (rep deltas +
//      taunts) drawn over the HUD that fade after a few seconds. This is
//      the visible feedback the spec asks for — no audio voice needed.
//
// report_player_kill() ties the two halves to faction::apply_player_kill:
// it's the single call the damage pass (and the debug "simulate kill"
// button) makes when the player earns a kill. It mutates rep, pushes feed
// lines, picks taunts, and logs stance-threshold flips.
//
// This module is deliberately a higher layer than faction (it depends on
// faction, never the reverse) and owns its own feed state — same pattern
// as sfx.{h,cpp}. cockpit/main just call load() once, tick() per frame,
// and draw() inside the HUD pass.
// -----------------------------------------------------------------------------

#include "faction.h"

#include <string>
#include <vector>

struct PlayerState;

namespace comm {

enum class Event : uint8_t {
    KillTheirEnemy,        // player killed a ship this faction hates -> praise
    KilledByPlayerCrime,   // player killed this faction or its ally -> threat
};

// Load the taunt table from `path`. Idempotent (replaces any prior table).
// A missing/unparseable file is NON-fatal: pick_line() then returns "" and
// the feed simply shows the bare rep-delta lines. Logs one summary line.
bool load(const std::string& path);

// Random flavour line for (faction, event). "" when none authored — the
// caller should treat that as "no taunt this time", not an error.
std::string pick_line(Faction f, Event e);

// Damage-pass entry point: the player just destroyed a `victim`-faction
// ship. Applies the reputation fallout (faction::apply_player_kill),
// pushes HUD feed lines (per-faction rep deltas + a taunt or two), and
// logs every rep change + stance-threshold flip to stdout.
void report_player_kill(PlayerState& player, Faction victim);

// ---- HUD comm feed -------------------------------------------------------
struct FeedLine {
    std::string text;          // already-formatted, ready to draw
    float       age_s = 0.0f;  // seconds since pushed
    bool        taunt = false; // true: comm chatter (amber); false: rep status (white)
};

// Push a pre-formatted line into the rolling feed (oldest drop off the top
// once the cap is hit). `taunt` selects the draw colour.
void push(const std::string& text, bool taunt);

// Age every feed line by dt and drop expired ones. Call once per frame.
void tick(float dt);

// Current feed contents (most-recent last). Exposed for tests / inspection.
const std::vector<FeedLine>& feed();

// Render the feed via ImGui's foreground draw list. Call inside the HUD
// pass (after simgui_new_frame, before simgui_render).
void draw();

} // namespace comm

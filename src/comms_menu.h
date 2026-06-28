#pragma once
// -----------------------------------------------------------------------------
// comms_menu.h — the canonical Privateer 2-step Comms menu (np-comms).
//
// Lives INSIDE the STATUS panel (cockpit_hud) when the player flips to the
// Comms screen (C key). A pure-data, two-step interaction:
//
//   1. DestSelect — pick who you're hailing: a local base/planet (from the
//      system's nav points) or your current target ship.
//   2. LineSelect — pick a canned line. The pool is chosen by the
//      destination's STANCE toward the player: Hostile destinations get the
//      "hostile" pool (taunts + pleas), everyone else the "friendly" pool.
//
// The line content is data-driven (assets/data/player_comms.json) so the
// barks can be retuned without a recompile — same philosophy as comm.h /
// voice.h. After a line is picked it's pushed to a small rolling comm log
// shown at the bottom of the panel and a placeholder pilot voice plays.
//
// State lives module-static here (like the rest of the cockpit HUD owns its
// transient widget state) because the menu is a singleton overlay: there is
// exactly one player, one open Comms screen at a time. draw() is called from
// cockpit_hud::draw_player_status; open()/select()/close() are driven by the
// main.cpp key handler (and the dev_remote /comms/select endpoint).
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>

struct StarSystem;
struct Ship;
struct PlayerReputation;

namespace comms_menu {

// Load the player comm-line pools from `path` (player_comms.json). Missing /
// unparseable file is NON-fatal: the menu degrades to a built-in minimal set
// so the screen still works (one log line). Idempotent — replaces any prior
// table.
void load(const std::string& path);

// Reset the interaction to DestSelect (call when the Comms screen opens).
void open();

// 1-based menu pick. In DestSelect it chooses a destination and advances to
// LineSelect; in LineSelect it picks a line, logs + voices it, and returns to
// DestSelect. Out-of-range picks are ignored.
void select(int n);

// Back to DestSelect (e.g. when the Comms screen is left). Cheap reset.
void close();

// Render the menu inside the already-open STATUS ImGui window. Read-only over
// the system nav points, the (optional) target ship, and the player's
// reputation — the stance lookup that picks friendly vs hostile lines.
void draw(const StarSystem& sys, const Ship* target, const PlayerReputation& rep);

// Per-frame pump (call once each Flight frame with the process-uptime clock
// in seconds). Two jobs: (1) stash `now_s` so select() — which has no clock
// of its own — can schedule a hailed party's delayed reply; (2) when a
// pending reply comes due, pick a random response line from the recipient
// faction's bank, push it to the comm log, and voice it. Cheap no-op when
// nothing is pending.
void tick(float now_s);

// Provoke-via-comms hand-off (see #4). When the player TAUNTS a target ship
// that isn't already hostile, there's a ~60% chance the comms layer flags
// that ship's id here. main.cpp drains it ONCE per frame (near tick()) and,
// if the ship is still around, flips its AI to aggro_player. Returns 0 when
// nothing is pending; reading it resets the latch so a single taunt provokes
// at most one ship, once.
uint32_t take_provoke_target();

// Rumor hand-off (np-comms "ask for rumors"). When the player ASKS a
// non-hostile recipient for news there's a small chance (~4%) the comms
// layer flags a rumor here. main.cpp drains it ONCE per frame (near
// take_provoke_target()) and, on success, drops a fresh nav lead. Returns
// false when nothing is pending; reading it resets the latch so one ask
// yields at most one lead.
bool take_rumor_pending();

} // namespace comms_menu

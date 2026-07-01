#pragma once
// -----------------------------------------------------------------------------
// fixers.h — bar fixers: the campaign's face (#137, epic #136).
//
// A FIXER is a named character who appears in a base's bar when their
// placement + plot-flag gate match, delivers a linear conversation, and
// (optionally) puts an OFFER in front of the player. Accepting runs a list
// of data-driven ACTIONS (plot flags, plot items, or campaign-handler
// tokens). Everything is authored in assets/data/fixers.json — the C++
// knows the grammar, never the story.
//
// MULTI-STAGE CONVERSATIONS ARE EMERGENT, NOT MODELLED: one json entry ==
// one appearance. "Sandoval offers the iron run" (forbids sandoval_accepted)
// and "Tayla replaces him after the run" (requires sandoval_delivered) are
// two independent entries whose gates can't both pass. The registry stays a
// flat list; the campaign becomes a chain of gates over plot state (#138).
//
// PLACEMENT — two modes, unioned:
//   * "base":       exact base id ("new_detroit_industrial")
//   * "archetypes": any base whose concourse archetype is listed
//                   ("mining", ...), minus "exclude_bases". This is the
//                   M22 requirement (Goodin: any mining base, not rygannon)
//                   designed in from day one.
//
// GATING — requires_flags (ALL must be set) + forbids_flags (NONE set),
// evaluated against PlayerState::plot_flags via plot::has_flag.
//
// ACTIONS — strings, executed in order on accept/refuse via the SHARED
// plot::run_action grammar (plot.h): native plot verbs, else the global
// campaign handler (plot::set_action_handler). One grammar for fixers AND
// scripted encounters — they can never drift.
//
// HEADLESS SPLIT: the model (load / present_at / accept / refuse) is pure
// logic; the ImGui bar body compiles only when FIXERS_HEADLESS is undefined
// (same pattern as MISSIONS_HEADLESS) so tools/test_fixers.cpp links the
// real gating/action code with no render/audio stack.
//
// Observability: set_observer mirrors offered/accepted/refused into
// dev_remote /events (category "fixer") via main.cpp — the agentic judge's
// hook. Same one-seam pattern as plot::set_observer.
// -----------------------------------------------------------------------------

#include <functional>
#include <string>
#include <vector>

struct PlayerState;

namespace fixers {

// One authored fixer appearance (see header comment: one entry == one
// gate-scoped appearance, not one character).
struct FixerDef {
    std::string id;              // unique: "sandoval_offer"
    std::string name;            // display: "Ernesto Sandoval"

    // ---- placement (union of the two modes) ----
    std::string              base_id;        // "" = not base-placed
    std::vector<std::string> archetypes;     // empty = not archetype-placed
    std::vector<std::string> exclude_bases;  // subtracted from archetype mode

    // ---- plot gate ----
    std::vector<std::string> requires_flags; // ALL must be set
    std::vector<std::string> forbids_flags;  // NONE may be set

    // ---- conversation ----
    std::vector<std::string> dialogue;       // linear paragraphs
    std::string              offer_text;     // "" = pure dialogue, no offer

    // ---- actions (see grammar in the header comment) ----
    std::vector<std::string> accept_actions;
    std::vector<std::string> refuse_actions;
    // Pure-dialogue entries run these when the conversation finishes —
    // how a handoff scene advances the plot without an accept step.
    std::vector<std::string> done_actions;
};

// Load assets/data/fixers.json. Idempotent (replaces the table). A missing
// or unparseable file is NON-fatal: logs one line, registry goes empty.
// Returns the number of fixers loaded.
int load(const std::string& path);

// Every fixer whose placement matches (base_id, archetype) AND whose plot
// gate passes for this player. Pointers are stable until the next load().
std::vector<const FixerDef*> present_at(const std::string& base_id,
                                        const std::string& archetype,
                                        const PlayerState& player);

// Look up by id (regardless of placement/gate). nullptr on miss.
const FixerDef* find(const std::string& id);

// Run a fixer's accept/refuse/done action list against the player.
// Exposed for the headless test + the campaign layer; the bar UI calls
// these on button press. Each also fires the observer.
void accept(const FixerDef& f, PlayerState& player);
void refuse(const FixerDef& f, PlayerState& player);
void dialogue_done(const FixerDef& f, PlayerState& player);

// Observability seam (main.cpp -> dev_remote /events, category "fixer"):
// "offered: sandoval_offer @ new_detroit_industrial" / "accepted: ..." /
// "refused: ..." / "dialogue_done: ...".
void set_observer(std::function<void(const std::string& what)> fn);

// Fire the "offered" observer line once per bar entry (the UI calls this
// when it first shows a fixer this visit; re-entering the bar re-fires).
void note_offered(const FixerDef& f, const std::string& base_id);

#ifndef FIXERS_HEADLESS
// Register the Bar screen body with the base-screen framework (#137).
// Draws bartender flavor + the fixers present + the conversation panel.
void register_bar_screen();
#endif

} // namespace fixers

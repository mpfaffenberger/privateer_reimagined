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
// CONVERSATION SHAPE — a scene runs in up to two phases. The MAIN dialogue
// plays to the offer; then ACCEPT/REFUSE roll into the optional
// accept_/refuse_ exchange so the fixer can react and the player gets a last
// word. The accept/refuse ACTIONS fire when that exchange finishes (or
// immediately, if none is authored) — never on the button press itself, and
// never twice. A conversation should end on a line, not on a click.
//
// PRESENTATION (optional) — a fixer may carry a `portrait` PNG and a `voice`
// clip per dialogue paragraph, both paths relative to assets/cinematics/ so
// the bar shares its art and audio with the cutscene system rather than
// duplicating it. `prop[i]` overrides the panel art for one line so the object
// under discussion (the Steltek artifact) can hold the frame at the beat where
// it matters. Portrait art is the character's canonical _ref.png; voices
// are their cloned ORIGINAL 1993 actor (tools/cinematics/gen_fixer_voices.py).
// Every field is optional and degrades independently: no portrait renders the
// original text-only panel, a short/absent voice array leaves those paragraphs
// silent, and an undecodable PNG is negative-cached after one log line. The
// engine still only ever opens a file by path — no AI, no network.
//
// HEADLESS SPLIT: the model (load / present_at / accept / refuse) is pure
// logic; the ImGui bar body compiles only when FIXERS_HEADLESS is undefined
// (same pattern as MISSIONS_HEADLESS) so tools/test_fixers.cpp links the
// real gating/action code with no render/audio stack. Note the presentation
// FIELDS still parse in headless builds — only their rendering is excluded.
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
    // Vanilla bar conversations are TWO-HANDERS: the fixer talks, the player
    // (Grayson) answers, back and forth. dialogue[i] is the line; speaker[i]
    // names who says it -- "" (or a short/absent array) means the fixer, and
    // "pc" means the player-character. Keeping this parallel rather than
    // nesting means old single-voice entries stay valid untouched.
    std::vector<std::string> dialogue;       // linear paragraphs
    std::vector<std::string> speaker;        // ""=fixer, "pc"=player
    // prop[i] is a PNG (relative to assets/cinematics/) shown INSTEAD of the
    // speaker portrait for that line -- the object being discussed takes the
    // frame at the moment it matters, e.g. the Steltek artifact when Sandoval
    // hands it over. "" (or a short/absent array) keeps the portrait.
    std::vector<std::string> prop;
    std::string              offer_text;     // "" = pure dialogue, no offer

    // ---- post-decision exchange (optional) ----
    // A conversation shouldn't end on a button. These play AFTER the player
    // commits, before the actions run: the fixer reacts, the player gets a
    // last word. Same parallel-array shape as the main conversation.
    std::vector<std::string> accept_dialogue;
    std::vector<std::string> accept_speaker;
    std::vector<std::string> accept_voice;
    std::vector<std::string> accept_prop;
    std::vector<std::string> refuse_dialogue;
    std::vector<std::string> refuse_speaker;
    std::vector<std::string> refuse_voice;

    // ---- presentation (optional; absent = the old text-only panel) ----
    // portrait: path relative to assets/cinematics/ (same convention as the
    // cinematic DSL's `line` cue) -- e.g. "portraits/tayla/_ref.png". Missing
    // or undecodable is non-fatal: the panel just renders without art.
    std::string              portrait;
    // portrait_pc: the player-character's portrait, shown in place of the
    // fixer's on "pc" lines so a two-hander reads as an exchange.
    std::string              portrait_pc;
    // voice[i] is the audio for dialogue[i], relative to assets/cinematics/.
    // Short/absent arrays are fine -- any paragraph without a clip is silent.
    std::vector<std::string> voice;
    // voice_offer: optional clip for the offer line shown with the final
    // paragraph (the ACCEPT/REFUSE beat).
    std::string              voice_offer;

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

// Expand the original Privateer dialogue substitution tokens in authored
// text. The 1993 script (and therefore the extracted VO transcripts in
// assets/speech/) addresses the player as:
//   $NM  surname     — formal address ("Ah, Captain $NM.")
//   $CS  callsign    — familiar address ("Feel lucky, $CS?")
// Keeping the tokens in fixers.json means authored lines stay drop-in
// compatible with verbatim vanilla lines lifted from bar_speakers.json.
// Unknown '$' sequences are passed through untouched.
std::string expand_tokens(const std::string& text);

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

#pragma once
// -----------------------------------------------------------------------------
// campaign.h — the Privateer story campaign's mission logic (epic #136).
//
// The fixers (fixers.h) are the campaign's FACE: they gate on plot flags and
// run action tokens. This module is the campaign's HANDS: it owns the
// non-native action tokens (plot::set_action_handler) and the world-side
// mission checks that don't fit a one-line action — cargo loading, dock
// settles, failure detection.
//
// One handler, many missions: tokens are namespaced per mission
// ("m01:accept"). Each mission's logic is a small set of static functions;
// the plot flags (plot.h) are the ONLY cross-module state. Flag convention
// per mission (documented once here, reused by every chain):
//
//   m<NN>_active     accepted and in progress
//   m<NN>_delivered  mid-mission milestone (cargo dropped, target reached)
//   <chain>_done     chain milestone the next fixer entry gates on
//
// M01 — Sandoval (#113): iron New Detroit -> Liverpool, settle back at the
// New Detroit bar (Tayla hands over the artifact; Sandoval never pays).
//   accept:   "m01:accept" loads k_m01_units of iron (hold-space checked —
//             a full hold refuses and the offer stays on the table).
//   deliver:  on_dock(liverpool*) with the iron aboard -> m01_delivered.
//   fail:     docking ANYWHERE without the consignment -> m01_active
//             cleared, offer re-appears at New Detroit.
//   settle:   the Tayla bar entry (fixers.json) requires m01_delivered and
//             grants steltek_artifact + sandoval_done via done_actions.
//
// M02..M05 — the Tayla smuggling arc (#114-#117). Same consignment shape,
// one table row each (see campaign.cpp k_cargo_missions): accept token
// loads the goods, docking at the destination pays out and sets the
// delivered flag, docking anywhere with the consignment missing fails the
// run (flag cleared, fixer re-offers). Chain milestones (tayla_1_done ...
// tayla_done) are advanced by the fixers' done_actions in fixers.json.
// Extra mechanics owned here:
//   * "tayla_employed" (set on m02 accept, cleared by the final debrief)
//     arms the PIRATE-NEUTRAL stance override while the player is in
//     Pentonville (tick() re-derives it every frame — see faction.h).
//   * "m04:install_compartment" grants the secret_compartment plot item.
//   * "m05:accept" stows the brilliance INSIDE the compartment (hidden,
//     scan-exempt) when the player owns it.
// Riordian (M05) lives in assets/data/scripted_encounters.json (on_launch
// ambush + at_nav re-ambush gated on the killed:riordian kill-memory).
//
// M06..M09 — the Lynch arc (#118-#121). M06 is pure data: the fixer sets
// m06_active, the Seelig scenario's on_dialogue_done marks the message
// delivered, and Lynch's debrief pays via the "pay:<credits>" token here.
// M07 is a cargo row (Kroiz = scenario data); M08 is a PASSENGER row
// (plot item lynch_cousin, completes on landing, can't be lost); M09 is a
// bare go-to-Oxford row (the Miggs ambush + m09_reveal objective rewrite
// are scenario data; landing at Oxford settles with lynch_done).
//
// The sandbox never calls in here; with no campaign flags set every
// function is a no-op.
// -----------------------------------------------------------------------------

#include <string>

struct PlayerState;

namespace campaign {

// Register the campaign action handler with plot::set_action_handler.
// Call once at startup (after commodity/ship_class tables load).
void init();

// Dock settle hook: call once per NEW dock commit (main.cpp's
// fail_cargo_on_dock site) with the raw nav base id ("liverpool_refinery").
// Runs every active mission's delivery/failure checks.
void on_dock(PlayerState& p, const std::string& base_id);

// Per-frame world-state derivation: re-arms/clears the campaign's faction
// stance overrides from plot flags + the player's current system (#114
// pirate neutrality). Cheap (a couple of flag probes); call once per frame
// in any mode. Idempotent.
void tick(const PlayerState& p, const std::string& system_id);

} // namespace campaign

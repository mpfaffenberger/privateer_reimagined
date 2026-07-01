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
//   fail:     on_dock(liverpool*) WITHOUT the iron (player sold it) ->
//             m01_active cleared, offer re-appears at New Detroit.
//   settle:   the Tayla bar entry (fixers.json) requires m01_delivered and
//             grants steltek_artifact + sandoval_done via done_actions.
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

} // namespace campaign

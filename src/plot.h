#pragma once
// -----------------------------------------------------------------------------
// plot.h — campaign plot flags + plot items (#138, epic #136).
//
// The story campaign's durable memory. Two flat string lists live on
// PlayerState (player.h) and are serialized by savegame.cpp (format v7):
//
//   * plot_flags — milestones and world-state switches. Convention:
//     lowercase snake_case, set-once-per-milestone ("sandoval_done",
//     "tayla_3_done", "drone_active", "campaign_complete"). Flags CAN be
//     cleared (clear_flag) for reversible states like drone_active.
//
//   * plot_items — story artifacts the player carries ("steltek_artifact",
//     "steltek_map", "secret_compartment"). NOT cargo: they occupy no hold
//     space, can't be sold/jettisoned/scanned, and survive hull swaps.
//     Anything tradeable belongs in the cargo/inventory systems instead.
//
// This module is the ONLY mutation path. Query helpers are pure reads;
// mutators are idempotent (setting a set flag / giving a held item is a
// no-op returning false) so scripted encounters can re-fire safely.
//
// Layering: plot depends on nothing but PlayerState (forward-declared).
// Gameplay modules (fixers, scripted encounters, docking gates, jump
// gates) query plot::has_flag; NOTHING in the sandbox path writes here.
// A fresh save or a pre-v7 save has both lists empty == campaign off.
//
// Observability: main.cpp registers an observer (set_observer) that
// mirrors every successful mutation into the dev_remote /events stream
// (category "plot") — the agentic judge's assertion hook. Same one-seam
// pattern as comm::set_feed_tap; plot.cpp never includes dev_remote.h.
// -----------------------------------------------------------------------------

#include <functional>
#include <string>
#include <string_view>

struct PlayerState;

namespace plot {

// ---- flags ------------------------------------------------------------

// True iff `flag` is set on this player.
bool has_flag(const PlayerState& p, std::string_view flag);

// Set `flag`. Returns true if it was newly set, false if already present
// (no duplicate entry is added) or `flag` is empty.
bool set_flag(PlayerState& p, std::string_view flag);

// Clear `flag`. Returns true if it was present.
bool clear_flag(PlayerState& p, std::string_view flag);

// ---- items ------------------------------------------------------------

// True iff the player carries plot item `id`.
bool has_item(const PlayerState& p, std::string_view id);

// Grant plot item `id`. Returns true if newly granted, false if already
// held (never duplicates) or `id` is empty.
bool give_item(PlayerState& p, std::string_view id);

// Take plot item `id` away. Returns true if it was held.
bool remove_item(PlayerState& p, std::string_view id);

// ---- observability (the dev_remote seam) --------------------------------

// Invoked after every SUCCESSFUL mutation with a one-line description:
// "flag set: sandoval_done" / "flag cleared: drone_active" /
// "item granted: steltek_artifact" / "item removed: steltek_map".
// One consumer today: main.cpp -> dev_remote::push_event("plot", ...).
// Main-thread only, like every mutator here.
void set_observer(std::function<void(const std::string& what)> fn);

} // namespace plot

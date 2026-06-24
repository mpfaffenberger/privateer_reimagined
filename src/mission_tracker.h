#pragma once
// -----------------------------------------------------------------------------
// mission_tracker.h — in-flight nav-reach + clear tracking (#13, part of #4).
//
// The gameplay GLUE between the live world and the mission MODEL. Every Flight
// frame, after perception/threat are updated, main.cpp hands us the player's
// world position + the current system's nav points and we:
//
//   * resolve each active mission's nav targets (stored as NAMES in
//     ActiveMission, see player.h) to live positions in the CURRENT system,
//   * flip nav_done[i] once the player flies within k_nav_reach_m of a target
//     (the same proximity feel as the nav-marker / autopilot arrival), and
//   * ask the model to settle the payout via missions:: helpers
//     (mark_nav_reached + complete_if_objectives_met) the moment the per-type
//     objectives are met.
//
// We OWN no mission state of our own beyond a tiny "were hostiles ever seen at
// this objective" memory for the Attack / DefendBase clear check — everything
// persistent lives on ActiveMission (and is saved by savegame.cpp). The model
// helpers are the single payout truth; we never touch credits directly.
//
// Pure-ish: takes the world by reference/value, no globals beyond threat::'s
// already-wired world oracle. Unit-testable offline against a hand-built
// PlayerState + nav list (see tools/test_mission_tracker.cpp).
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <string>
#include <vector>

struct PlayerState;
struct NavPointDef;

namespace mission_tracker {

// How close (world units) the player must fly to a mission nav point for it to
// count as "reached/surveyed". Tied to the nav-marker arrival feel — same order
// as autopilot's stop distance, well inside its k_threat_radius_m (8 km) bubble.
constexpr float k_nav_reach_m = 2000.0f;

// Radius (world units) around an Attack / DefendBase objective that must be
// free of hostiles for the objective to count as CLEARED. Sized to the
// autopilot/jump threat scale (k_threat_radius_m == 8 km) but a touch tighter
// so you don't have to chase the last straggler across the whole sector.
constexpr float k_clear_radius_m = 6000.0f;

// Call once per Flight frame, AFTER perception/threat are updated and the
// missions::on_player_kill hook. Resolves each active mission's nav targets to
// positions in the CURRENT system, flips nav_done / progress, and triggers
// completion through the missions:: model helpers. No-op for missions whose
// target_system != current_system (cross-system jobs wait until you jump there)
// and for Bounty / CargoDelivery (those settle via their own paths).
void tick(PlayerState& p, const std::string& current_system,
          const std::vector<NavPointDef>& navs, HMM_Vec3 player_pos);

} // namespace mission_tracker

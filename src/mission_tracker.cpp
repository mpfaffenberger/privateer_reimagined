// -----------------------------------------------------------------------------
// mission_tracker.cpp — see mission_tracker.h.
//
// One job: walk the player's active missions each Flight frame and flip their
// in-flight objective state from the live world, then let the model settle the
// payout. We deliberately do NOT iterate p.missions while mutating it through
// the model helpers (complete_if_objectives_met erases the completed mission),
// so we snapshot the ids first and re-resolve each by id — any that got dropped
// mid-pass simply resolve to nullptr and are skipped.
// -----------------------------------------------------------------------------

#include "mission_tracker.h"

#include "comm.h"
#include "missions.h"
#include "player.h"
#include "system_def.h"   // NavPointDef
#include "threat.h"

#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace mission_tracker {
namespace {

// "Were hostiles ever seen at this objective?" memory, keyed by mission id.
// Lives here (not on ActiveMission) because it's transient combat context, not
// progress worth saving: on reload you simply re-establish presence by flying
// back to the objective. Cleaned of stale ids each tick so it can't grow
// unbounded across a long career.
std::unordered_set<std::string> g_hostiles_seen;

// Resolve a nav point's live position by NAME in the current system. Returns
// false when the name isn't present (cross-system or stale id) so the caller
// can skip rather than treat (0,0,0) as a real position.
bool nav_pos_by_name(const std::vector<NavPointDef>& navs,
                     const std::string& name, HMM_Vec3& out) {
    for (const NavPointDef& n : navs) {
        if (n.name == name) { out = n.position; return true; }
    }
    return false;
}

// Resolve an objective base's live position by base_id in the current system
// (DefendBase targets a base, not a free nav). Falls back to a nav whose NAME
// matches when no base_id hit, so authored data that stores the base under
// either key still resolves.
bool base_pos_by_id(const std::vector<NavPointDef>& navs,
                    const std::string& base_id, HMM_Vec3& out) {
    if (base_id.empty()) return false;
    for (const NavPointDef& n : navs)
        if (n.base_id == base_id) { out = n.position; return true; }
    return nav_pos_by_name(navs, base_id, out);
}

float dist(HMM_Vec3 a, HMM_Vec3 b) { return HMM_LenV3(HMM_SubV3(a, b)); }

// Reach detection for the nav-visiting types (Scout / Patrol / Attack). Flips
// any not-done nav the player is now within k_nav_reach_m of and pushes a comm
// progress line. Returns true if at least one new nav flipped this tick.
bool update_nav_reach(PlayerState& p, ActiveMission& am,
                      const std::vector<NavPointDef>& navs,
                      HMM_Vec3 player_pos) {
    // Tolerate a save predating nav_done (resize-on-load also covers this).
    if (am.nav_done.size() != am.nav_targets.size())
        am.nav_done.assign(am.nav_targets.size(), (uint8_t)0);

    bool any = false;
    for (size_t i = 0; i < am.nav_targets.size(); ++i) {
        if (am.nav_done[i]) continue;
        HMM_Vec3 pos;
        if (!nav_pos_by_name(navs, am.nav_targets[i], pos)) continue;
        if (dist(player_pos, pos) > k_nav_reach_m) continue;
        if (!missions::mark_nav_reached(p, am.id, i)) continue;

        // Count how many are done now for the "(k/n)" progress line.
        int done = 0;
        for (uint8_t d : am.nav_done) done += (d != 0);
        char line[96];
        std::snprintf(line, sizeof(line), "Nav point surveyed (%d/%d)",
                      done, (int)am.nav_done.size());
        comm::push(line, /*taunt=*/false);
        any = true;
    }
    return any;
}

// Clear detection for the combat-at-a-place types (Attack / DefendBase). Once
// the player is AT the objective and hostiles were present, completion fires
// the moment the bubble is hostile-free. Drives the shared `progress` counter
// (so complete_if_objectives_met's progress>=hostiles_required gate settles it),
// which keeps a single completion truth shared with the #15 kill router.
void update_clear(PlayerState& p, ActiveMission& am,
                  const std::vector<NavPointDef>& navs, HMM_Vec3 player_pos) {
    HMM_Vec3 target;
    bool have = false;
    if ((missions::MissionType)am.type == missions::MissionType::DefendBase)
        have = base_pos_by_id(navs, am.target_base, target);
    if (!have && !am.nav_targets.empty())
        have = nav_pos_by_name(navs, am.nav_targets[0], target);
    if (!have) return;

    // Only engage the clear logic while the player is loitering at the
    // objective — you can't clear a furball you've flown away from.
    if (dist(player_pos, target) > k_clear_radius_m) return;

    const bool hostiles = threat::hostiles_near(target, k_clear_radius_m);
    if (hostiles) {
        g_hostiles_seen.insert(am.id);   // remember the fight happened here
        return;
    }
    // No hostiles right now. Only counts as a CLEAR if a fight was present.
    if (!g_hostiles_seen.count(am.id)) return;

    // Mark the objective cleared by satisfying the model's completion gate.
    if (am.hostiles_required > 0 && am.progress < am.hostiles_required)
        am.progress = am.hostiles_required;
    std::printf("[mission_tracker] CLEARED %s at objective\n", am.title.c_str());
}

ActiveMission* find_active(PlayerState& p, const std::string& id) {
    for (ActiveMission& am : p.missions)
        if (am.id == id) return &am;
    return nullptr;
}

} // namespace

void tick(PlayerState& p, const std::string& current_system,
          const std::vector<NavPointDef>& navs, HMM_Vec3 player_pos) {
    using MT = missions::MissionType;

    // Snapshot ids up front: the completion helpers erase the mission they
    // settle, so we can't hold a reference across the call.
    std::vector<std::string> ids;
    ids.reserve(p.missions.size());
    for (const ActiveMission& am : p.missions) ids.push_back(am.id);

    // Garbage-collect the hostiles-seen memory: drop ids that are no longer
    // active so it tracks the live mission set, not a growing career log.
    std::unordered_set<std::string> live(ids.begin(), ids.end());
    for (auto it = g_hostiles_seen.begin(); it != g_hostiles_seen.end(); ) {
        if (!live.count(*it)) it = g_hostiles_seen.erase(it);
        else                  ++it;
    }

    for (const std::string& id : ids) {
        ActiveMission* am = find_active(p, id);
        if (!am) continue;   // already settled earlier this pass

        // Cross-system jobs wait until the player actually jumps there.
        if (am->target_system != current_system) continue;

        switch ((MT)am->type) {
            case MT::Scout:
            case MT::Patrol:
                update_nav_reach(p, *am, navs, player_pos);
                missions::complete_if_objectives_met(p, id);
                break;
            case MT::Attack:
                // Nav reach lets the player "arrive"; clear settles it.
                update_nav_reach(p, *am, navs, player_pos);
                update_clear(p, *am, navs, player_pos);
                missions::complete_if_objectives_met(p, id);
                break;
            case MT::DefendBase:
                update_clear(p, *am, navs, player_pos);
                missions::complete_if_objectives_met(p, id);
                break;
            case MT::Bounty:
            case MT::CargoDelivery:
                // Settle via on_target_destroyed() / complete_delivery() — not here.
                break;
        }
    }
}

} // namespace mission_tracker

// -----------------------------------------------------------------------------
// escort.cpp — campaign escort lifecycle (see escort.h).
// -----------------------------------------------------------------------------

#include "escort.h"

#include "comm.h"
#include "plot.h"
#include "player.h"
#include "ship.h"
#include "ship_registry.h"
#include "ship_sprite.h"   // ShipSpriteObject (companion-warp position sync)
#include "system_def.h"
#include "faction.h"

#include <cstdio>

namespace escort {
namespace {

// "Landed" when the escortee closes within this of the destination nav.
// Deliberately SMALLER than ship_ai's k_travel_arrive_m (6 km): the
// Traveler AI would re-pick a random waypoint at 6 km, but tick() re-pins
// the destination every frame, so the ship keeps boring in until we call
// it landed here.
constexpr float k_land_radius_m = 4000.0f;

// One live escort (single-slot by design; see header).
struct Active {
    bool     on = false;
    uint32_t ship_id = 0;
    Def      def;
};
Active g_active;

const NavPointDef* find_nav(const StarSystem& system, const std::string& name) {
    for (const NavPointDef& n : system.nav_points)
        if (n.name == name) return &n;
    return nullptr;
}

void fail(PlayerState& player, const char* why) {
    std::printf("[escort] '%s' FAILED (%s)\n", g_active.def.name.c_str(), why);
    for (const std::string& f : g_active.def.fail_clear_flags)
        plot::clear_flag(player, f);
    g_active = Active{};
}

} // namespace

bool begin(const Def& def, HMM_Vec3 spawn_pos, ShipRegistry& ships,
           const StarSystem& system, PlayerState& player,
           const encounters::SpawnFn& spawn) {
    (void)player;
    if (g_active.on) {
        std::fprintf(stderr, "[escort] begin refused: '%s' already active\n",
                     g_active.def.name.c_str());
        return false;
    }
    const NavPointDef* dest = find_nav(system, def.dest_nav);
    if (!dest) {
        std::fprintf(stderr, "[escort] begin refused: no nav '%s' in %s\n",
                     def.dest_nav.c_str(), system.name.c_str());
        return false;
    }

    encounters::SpawnRequest req;
    req.class_name       = def.ship_class;
    const Faction fac    = faction::from_name(def.faction);
    req.faction          = (fac == Faction::Count) ? Faction::Merchant : fac;
    req.position         = HMM_AddV3(spawn_pos, HMM_V3(800.0f, 200.0f, 800.0f));
    req.initial_ai_state = AIState::Patrol;
    req.patrol_anchor    = req.position;
    const uint32_t id = spawn(req);
    if (id == 0) {
        std::fprintf(stderr, "[escort] spawn failed for '%s'\n",
                     def.name.c_str());
        return false;
    }

    Ship* s = ships.find_by_id(id);
    if (s) {
        // Travel goal: bore straight for the destination. tick() re-pins
        // this every frame so the Traveler AI can't wander off-lane.
        s->ai.civ_role        = CivRole::Traveler;
        s->ai.travel_dest     = dest->position;
        s->ai.has_travel_dest = true;
        if (def.hull_scale > 0.0f && def.hull_scale != 1.0f) {
            s->armor_fore_cm      *= def.hull_scale;
            s->armor_aft_cm       *= def.hull_scale;
            s->armor_port_cm      *= def.hull_scale;
            s->armor_starboard_cm *= def.hull_scale;
        }
    }

    g_active.on      = true;
    g_active.ship_id = id;
    g_active.def     = def;
    comm::push(def.name + ": 'Good to see a friendly wing. Making my run for " +
               def.dest_nav + " - stay close.'", true);
    std::printf("[escort] '%s' underway: %s -> %s (id %u, hull x%.2f)\n",
                def.name.c_str(), def.ship_class.c_str(),
                def.dest_nav.c_str(), id, def.hull_scale);
    return true;
}

void tick(ShipRegistry& ships, const StarSystem& system, PlayerState& player,
          const encounters::DespawnFn& despawn,
          bool player_warp, HMM_Vec3 player_pos) {
    if (!g_active.on) return;

    Ship* s = ships.find_by_id(g_active.ship_id);
    if (!s || !s->alive) {
        comm::push(g_active.def.name + " has been destroyed. The escort has failed.",
                   false);
        fail(player, "escortee destroyed");
        return;
    }

    const NavPointDef* dest = find_nav(system, g_active.def.dest_nav);
    if (!dest) { fail(player, "destination nav vanished"); return; }

    // Autopilot companionship: while the player warps between navs the
    // escortee keeps formation (teleported alongside, slightly toward its
    // destination). This is how vanilla escorts crossed a system without
    // a half-hour merchant crawl.
    if (player_warp) {
        HMM_Vec3 toward = HMM_SubV3(dest->position, player_pos);
        const float len = HMM_LenV3(toward);
        toward = (len > 1e-3f) ? HMM_DivV3F(toward, len) : HMM_V3(0, 0, 1);
        // Hold the escortee 6 km AHEAD (toward its destination): it leads
        // the formation, so when the player's autopilot drops out at the
        // 15 km arrival ring the freighter is already deep into its final
        // approach instead of trailing behind.
        const HMM_Vec3 hold = HMM_AddV3(player_pos,
                                        HMM_MulV3F(toward, 6000.0f));
        s->position = hold;
        if (s->sprite) s->sprite->position = hold;
    }

    // Re-pin the travel goal (the Traveler AI re-picks random lanes at its
    // own arrive radius; we want a beeline). Combat states override this
    // per-frame anyway — a fleeing/fighting Drayman resumes course once
    // the shooting stops. Full throttle: the cargo is late already.
    s->ai.civ_role        = CivRole::Traveler;
    s->ai.travel_dest     = dest->position;
    s->ai.has_travel_dest = true;
    // Inside the Traveler's own 6 km arrive shell, civilian_behavior
    // re-picks a RANDOM waypoint every frame (drunken orbit, never
    // reaching our tighter land ring). We run after ship_ai and before
    // ship::tick, so overwrite the BEHAVIOR too while not in combat.
    if (s->ai.state == AIState::Idle || s->ai.state == AIState::Patrol) {
        s->behavior.kind       = ShipBehavior::PursueTarget;
        s->behavior.target_pos = dest->position;
        s->controller.speed_scale = 1.0f;
    }

    const HMM_Vec3 d = HMM_SubV3(dest->position, s->position);
    if (HMM_DotV3(d, d) < k_land_radius_m * k_land_radius_m) {
        comm::push(g_active.def.name +
                   ": 'Docking clearance received - I'm down safe. My thanks, "
                   "pilot. Drinks are on me planetside.'", true);
        if (!g_active.def.landed_flag.empty())
            plot::set_flag(player, g_active.def.landed_flag);
        std::printf("[escort] '%s' landed at %s\n",
                    g_active.def.name.c_str(), g_active.def.dest_nav.c_str());
        const uint32_t id = g_active.ship_id;
        g_active = Active{};
        if (despawn) despawn(id);
    }
}

uint32_t active_ship_id() {
    return g_active.on ? g_active.ship_id : 0;
}

void reset(PlayerState* player) {
    if (!g_active.on) return;
    if (player) {
        comm::push("You left " + g_active.def.name +
                   " behind. The escort has failed.", false);
        fail(*player, "abandoned (launch/system switch)");
    } else {
        g_active = Active{};
    }
}

} // namespace escort

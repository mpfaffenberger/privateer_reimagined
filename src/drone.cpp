// -----------------------------------------------------------------------------
// drone.cpp — Steltek drone lifecycle. See drone.h for the design.
// -----------------------------------------------------------------------------

#include "drone.h"

#include "comm.h"
#include "faction.h"
#include "player.h"
#include "plot.h"
#include "ship.h"
#include "ship_registry.h"
#include "ship_sprite.h"

#include <cstdio>

namespace {

uint32_t g_drone_id    = 0;
double   g_next_try_at = -1.0;   // <0 = not armed yet (reset() arms it)

// Pseudo-random-ish lateral offset so it doesn't always appear dead
// astern; deterministic per spawn count is plenty.
int g_spawn_count = 0;

} // namespace

namespace drone {

void reset() {
    g_drone_id    = 0;
    g_next_try_at = -1.0;
}

uint32_t active_ship_id() { return g_drone_id; }

void tick(ShipRegistry& ships, PlayerState& p, HMM_Vec3 player_pos,
          double now_s, const encounters::SpawnFn& spawn,
          const encounters::DespawnFn& despawn) {
    if (!plot::has_flag(p, "drone_active")) return;
    if (plot::has_flag(p, "killed:steltek_drone")) return;

    // ---- live drone upkeep ------------------------------------------------
    if (g_drone_id != 0) {
        Ship* d = ships.find_by_id(g_drone_id);
        if (!d || !d->alive) {
            if (d && !d->alive) {
                // Dead. Only possible once the boost event opened the
                // whitelist — stamp the kill-memory for the M23 settle.
                plot::set_flag(p, "killed:steltek_drone");
                comm::push("The drone comes apart in a soundless green "
                           "flash. The signal it was screaming stops.",
                           false);
                std::printf("[drone] DESTROYED\n");
            }
            g_drone_id = 0;
            g_next_try_at = now_s + k_respawn_delay_s;
            return;
        }
        // Always damage_immune — Confed's finale fleet must bounce off it
        // forever (#135). The boost event opens the ONE-gun whitelist
        // instead; re-derived every frame so the mid-route boost scene
        // takes effect immediately.
        d->damage_immune     = true;
        d->immune_bypass_gun = plot::has_flag(p, "steltek_gun_boosted")
                                   ? GunType::SteltekGun : GunType::Count;
        d->ai.aggro_player = true;   // never loses interest
        // Outrun rule: past the leash it loses the scent (and the player
        // earns a breather) instead of chasing across the whole map.
        const float dist = HMM_LenV3(HMM_SubV3(
            d->sprite ? d->sprite->position : player_pos, player_pos));
        if (dist > k_leash_m) {
            std::printf("[drone] outrun (%.0f m) — despawn, re-stalk in %.0fs\n",
                        (double)dist, k_respawn_delay_s);
            despawn(g_drone_id);
            g_drone_id    = 0;
            g_next_try_at = now_s + k_respawn_delay_s;
        }
        return;
    }

    // ---- stalking: waiting for the next appearance -------------------------
    if (g_next_try_at < 0.0) {              // fresh launch/jump: arm timer
        g_next_try_at = now_s + k_first_spawn_delay_s;
        return;
    }
    if (now_s < g_next_try_at) return;

    encounters::SpawnRequest req;
    req.class_name = "drone";
    req.faction    = Faction::Steltek;
    ++g_spawn_count;
    const float lat = ((g_spawn_count % 3) - 1) * 0.5f;   // -0.5 / 0 / +0.5
    req.position   = HMM_AddV3(player_pos,
                               HMM_V3(k_spawn_dist_m * lat, 800.0f,
                                      k_spawn_dist_m));
    req.patrol_anchor = req.position;
    const uint32_t id = spawn(req);
    if (id == 0) {
        // Class/atlas not loaded — log once per attempt window, retry later.
        std::fprintf(stderr, "[drone] spawn failed (class 'drone'); retrying\n");
        g_next_try_at = now_s + k_respawn_delay_s;
        return;
    }
    g_drone_id = id;
    if (Ship* d = ships.find_by_id(id)) {
        d->ai.aggro_player   = true;
        d->damage_immune     = true;
        d->immune_bypass_gun = plot::has_flag(p, "steltek_gun_boosted")
                                   ? GunType::SteltekGun : GunType::Count;
    }
    comm::push("Sensors scream: an impossible energy signature just "
               "resolved out of the dark. It is coming STRAIGHT AT YOU.",
               true);
    std::printf("[drone] spawned id=%u (%s)\n", id,
                plot::has_flag(p, "steltek_gun_boosted")
                    ? "VULNERABLE to the boosted gun" : "invulnerable");
}

} // namespace drone

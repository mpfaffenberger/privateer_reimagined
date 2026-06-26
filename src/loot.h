#pragma once
// -----------------------------------------------------------------------------
// loot.h — in-world loot drops (Phase 4c, np-#86).
//
// Player kills spawn a brief, floating "loot drop" anchored at the wreck
// position. The drop exists for k_loot_ttl_s and then ages out (cleanup
// only — Phase 4d/e will let the player tractor a drop into the cargo hold
// to PULL it; for now this is a render-only marker so the loot system has
// a visible surface to validate). Drops carry their full InventoryItem
// payload (id, kind, rarity, qty, mods) so the future pickup path can just
// read `drop.item` without any secondary lookup.
//
// Design notes:
//   * Single file-static std::mt19937 with a fixed seed. Drops are
//     cosmetic / cosmetic-test — reproducibility matters more than entropy.
//   * Drops DO NOT survive a system reload (no savegame hook here yet);
//     the main.cpp reset sites (system load + landing) call clear().
//   * Schema is read from assets/data/loot_tables.json on first load.
//     Missing file = no-op; we never crash on the loot table because loot
//     is a quality-of-life feature, not a core mechanic.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>
#include "HandmadeMath.h"
#include "inventory.h"

enum class Faction : uint8_t;
struct Camera;
struct PlayerState;

namespace loot {

// One in-world drop. `age_s` is the elapsed lifetime; once it crosses
// k_loot_ttl_s the drop is removed on the next tick().
struct LootDrop {
    HMM_Vec3 pos = HMM_V3(0.0f, 0.0f, 0.0f);
    inventory::InventoryItem item;
    float    age_s = 0.0f;
};

// Read the loot tables from disk and prime the RNG side. The drop tables
// are indexed by faction::to_name(); missing file = no-op + warn-once.
void load(const std::string& path);

// Roll the table for `faction` and push exactly one drop at `pos` +
// a small ~150m scatter offset. `is_ace` adds the global ace_bonus to
// the legendary outcome. No-op if the faction has no entry.
void spawn_for(Faction faction, HMM_Vec3 pos, bool is_ace);

// Age every drop by `dt` (seconds) and drop any past k_loot_ttl_s.
void tick(float dt);

// All live drops, in spawn order. Read-only for rendering + the future
// tractor/pull phase; do not mutate from outside.
const std::vector<LootDrop>& all();

// Draw every live drop as a small diamond + tiny label at the projected
// screen position. Cheap (one quad + one AddText per drop); call once
// per Flight frame after cockpit_hud::build_mission_objectives. No-op
// when `all()` is empty.
void render(const Camera& cam, bool draw_world);

// Drop everything. Called from the same main.cpp sites that reset
// hailing::reset / scripted::reset (system load, launch re-roll).
void clear();

// Time-out for a drop. Public so the HUD/tests can use the same value.
constexpr float k_loot_ttl_s = 180.0f;

// Tractor/pull a drop into the cargo hold (#84 + #83, universal). Walks
// every live drop within `range` world units of `player_pos`; for each
// in-range drop it tries to add to the hold via the unified-hold
// helpers. A Commodity-kind item goes through `add_cargo` (qty units,
// free-price); every other kind goes through `add_item` (one slot per
// InventoryItem, qty included). On success the drop is removed. On
// hold-full (the add refuses) the drop is LEFT IN PLACE so the next
// press can try again. Returns the number of drops successfully
// pulled (0 if no in-range drops or all adds were refused).
//
// `capacity` is the unified-hold cap the caller resolved (typically
// `player::cargo_capacity(g.player, ship_class::find(g.player.ship_class_name))`).
int try_pull(HMM_Vec3 player_pos, float range, PlayerState& player,
             int capacity);

} // namespace loot

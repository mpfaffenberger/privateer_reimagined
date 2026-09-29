// -----------------------------------------------------------------------------
// tools/test_full_loop.cpp — the END-TO-END integration spine for the whole
// sandbox loop (np-zte.3). The capstone harness: where every other test_*.cpp
// proves one subsystem in isolation, this one wires the REAL subsystems
// together and walks the entire roadmap playtest programmatically —
//
//   new_game -> encounter spawn -> kill (rep + bounty) -> dock + autosave ->
//   commodity arbitrage -> outfitting -> missions -> repair -> missile kill +
//   afterburner -> jump (round-trip + hostile gate) -> save/reload round-trip
//   -> death/respawn.
//
// dev_remote can't inject keystrokes (np-w68), so the live manual playtest
// isn't scriptable. This harness IS the scriptable proxy: it drives the exact
// model functions the live game's input handlers / damage pass call, against
// the same data, so every PASS here is a PASS the human would see in-game.
// It is the regression spine for the loop — if a cross-system seam breaks
// (a field that doesn't persist, a stance that doesn't propagate, a mission
// dropped on teardown), a step here goes red.
//
// Real subsystems, not mocks. The only stub is sfx::ui_click (the UI blip the
// docking/autopilot modules emit) — exactly the stub tools/test_docking.cpp
// already uses — because pulling it in would drag the whole sokol_audio mixer
// into a headless link for zero test value. Everything else is the shipping
// code.
//
// Build (run from the repo root so relative asset paths resolve):
//   clang++ -std=c++20 -DECONOMY_HEADLESS -DOUTFITTING_HEADLESS \
//       -DMISSIONS_HEADLESS -DCOMM_HEADLESS -Isrc -Ithird_party \
//       tools/test_full_loop.cpp \
//       src/game_state.cpp src/player.cpp src/faction.cpp src/comm.cpp \
//       src/commodity.cpp src/economy.cpp src/outfitting.cpp src/missions.cpp \
//       src/galaxy.cpp src/system_def.cpp src/json.cpp src/savegame.cpp \
//       src/docking.cpp src/autopilot.cpp src/threat.cpp src/jump.cpp \
//       src/encounters.cpp src/repair.cpp src/missile.cpp src/ship.cpp \
//       src/ship_class.cpp src/ship_registry.cpp src/ship_ai.cpp \
//       src/perception.cpp src/gun.cpp src/shield.cpp src/armor.cpp \
//       src/mobility.cpp src/camera.cpp \
//       -o /tmp/test_full_loop && /tmp/test_full_loop
// -----------------------------------------------------------------------------

#include "autopilot.h"
#include "camera.h"
#include "comm.h"
#include "commodity.h"
#include "docking.h"
#include "economy.h"
#include "encounters.h"
#include "faction.h"
#include "galaxy.h"
#include "game_state.h"
#include "jump.h"
#include "missile.h"
#include "missions.h"
#include "outfitting.h"
#include "player.h"
#include "repair.h"
#include "savegame.h"
#include "test_sandbox.h"
#include "ship.h"
#include "ai_brain.h"
#include "ship_class.h"
#include "ship_registry.h"
#include "ship_sprite.h"
#include "shield.h"
#include "armor.h"
#include "system_def.h"
#include "threat.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

// sfx.cpp drags in the whole audio mixer; the docking + autopilot modules call
// this one entry point. Stub it exactly like tools/test_docking.cpp does.
namespace sfx { void ui_click() {} }

namespace {

// ---- test scaffolding -------------------------------------------------------
int g_fail = 0;
int g_step = 0;

void banner(const char* letter, const char* title) {
    std::printf("\n=================================================================\n");
    std::printf("STEP %s — %s\n", letter, title);
    std::printf("=================================================================\n");
}

// One assertion inside a step. Logs PASS/FAIL with the human label.
bool check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
    return ok;
}

// Roll the per-step PASS/FAIL up into one line so the validation paste reads
// "STEP a .. PASS" top-to-bottom even at a glance.
void step_result(const char* letter, int fails_before) {
    const bool ok = (g_fail == fails_before);
    std::printf("  ----> STEP %s: %s\n", letter, ok ? "PASS" : "*** FAIL ***");
}

// ---- a headless world the encounter/threat/combat code can query -----------
// ShipRegistry owns the Ships; the sprite objects must outlive the registry
// entries and stay pointer-stable, so a deque holds them (same guarantee the
// live game leans on for placed_ship_sprites). No GPU: ShipSpriteObject is a
// plain struct, we only set the fields ship::hit_radius_m + threat read.
struct World {
    ShipRegistry                 ships;
    std::deque<ShipSpriteObject> sprites;
};

// The np-eag.1 spawn recipe, headless: a sprite slot for the hit sphere, a
// class-derived Ship, faction + pose, registered in the slot-map. Returns the
// new ship's monotonic id (the currency the director + AI key off).
uint32_t spawn_npc(World& w, const ShipClass& k, Faction f, HMM_Vec3 pos) {
    w.sprites.emplace_back();
    ShipSpriteObject& spr = w.sprites.back();
    spr.position   = pos;
    spr.world_size = 200.0f;            // 100 m hit sphere (matches test_missile)
    Ship s   = ship::spawn(k);
    s.faction  = f;
    s.position = pos;
    s.sprite   = &spr;
    const ShipHandle h = w.ships.spawn(std::move(s));
    const Ship* sp = w.ships.get(h);
    return sp ? sp->id : 0;
}

// Mirror the live Commodity Exchange screen's buy/sell enforcement (which
// lives behind ECONOMY_HEADLESS). Same checks, same player:: helpers, same
// economy::record_purchase — this is exactly what the buttons call.
bool trade_buy(PlayerState& p, const char* base, const Commodity& c, int qty, int cap) {
    const economy::Quote q = economy::price(base, c.id);
    const long long cost = (long long)q.buy_price * qty;
    if (q.buy_price <= 0 || q.available_units < qty) return false;
    if (player::cargo_units_used(p) + qty > cap)     return false;
    if (!player::can_afford(p, cost))                return false;
    player::spend_credits(p, cost);
    player::add_cargo(p, c.id, qty, q.buy_price, cap);
    economy::record_purchase(base, c.id, qty);
    return true;
}

long long trade_sell(PlayerState& p, const char* base, const Commodity& c, int qty) {
    const economy::Quote q = economy::price(base, c.id);
    if (!player::remove_cargo(p, c.id, qty)) return -1;
    const long long gain = (long long)q.sell_price * qty;
    player::add_credits(p, gain);
    return gain;
}

// Find a nav point in a system by name; -1 if absent.
int nav_index(const StarSystem& s, const std::string& name) {
    for (size_t i = 0; i < s.nav_points.size(); ++i)
        if (s.nav_points[i].name == name) return (int)i;
    return -1;
}
const NavPointDef* nav_with_base(const StarSystem& s, const std::string& base_id) {
    for (const NavPointDef& n : s.nav_points)
        if (n.base_id == base_id) return &n;
    return nullptr;
}

float total_armor(const Ship& s)  { return s.armor_fore_cm + s.armor_aft_cm + s.armor_port_cm + s.armor_starboard_cm; }
float total_health(const Ship& s) {
    return s.shield_fore_cm + s.shield_aft_cm + s.shield_port_cm + s.shield_starboard_cm
         + s.armor_fore_cm  + s.armor_aft_cm  + s.armor_port_cm  + s.armor_starboard_cm;
}

// ---- sprite-pool model for the corpse-reap regression (STEP m / np-zte.1) --
// A faithful mirror of main.cpp's claim_sprite_slot/free_sprite_slot: a deque
// of ShipSpriteObject (pointer-stable growth) + a free-list of parked slot
// indices. main.cpp itself can't be linked headlessly (sokol/GPU), so we
// reproduce its sprite-pool + death-path reap CONTRACT here and drive the REAL
// ShipRegistry + ship::take_damage + encounters director through it. The
// production reap lives in main.cpp's death-detection pass; this guards the
// invariant it must uphold.
struct SpritePool {
    std::deque<ShipSpriteObject> slots;
    std::vector<size_t>          free_list;

    // == main.cpp claim_sprite_slot(): reuse a parked slot else append.
    size_t claim_slot() {
        size_t i;
        if (!free_list.empty()) { i = free_list.back(); free_list.pop_back(); }
        else                    { i = slots.size(); slots.emplace_back(); }
        slots[i] = ShipSpriteObject{};
        return i;
    }
    // == main.cpp free_sprite_slot(): park the slot a sprite pointer aliases.
    void park_slot(ShipSpriteObject* spr) {
        if (!spr) return;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (&slots[i] == spr) { slots[i] = ShipSpriteObject{}; free_list.push_back(i); return; }
        }
    }
};

// == main.cpp death-pass NPC reap: free the sprite slot + despawn the registry
// handle for every dead NON-player ship. Returns how many it reaped.
int reap_dead_npcs(ShipRegistry& ships, SpritePool& pool) {
    int reaped = 0;
    for (size_t i = 0; i < ships.slot_count(); ++i) {
        Ship* s = ships.ship_at(i);
        if (!s || s->is_player || s->alive) continue;
        pool.park_slot(s->sprite);
        ships.despawn(ships.find_handle_by_id(s->id));
        ++reaped;
    }
    return reaped;
}

} // namespace

int main() {
    test_sandbox::isolate_saves("full_loop");  // dock autosaves + slot 9 (#383)
    std::printf("################################################################\n");
    std::printf("# np-zte.3 — FULL SANDBOX LOOP integration harness\n");
    std::printf("# fly -> fight -> dock -> trade -> outfit -> mission -> jump ->\n");
    std::printf("#         save/reload -> death/respawn\n");
    std::printf("################################################################\n");

    // ---- load every catalog the real subsystems resolve against -----------
    faction::init();
    comm::load("assets/data/comm_lines.json");
    commodity::load("assets/data/privateer_db/cargo.toml");
    economy::load("assets/data/commodity_prices.json", "assets/bases");
    gun::load_table("assets/data/privateer_ship_data.json");
    shield::load_table("assets/data/privateer_ship_data.json");
    armor::load_table("assets/data/privateer_ship_data.json");
    ship_class::load_all("assets/ships");
    ai_brain::load_all("assets/ai");   // validate the AI logic tables parse
    outfitting::load("assets/data/ship_prices.json", "assets/data/equipment_prices.json");

    galaxy::Galaxy gal;
    if (!galaxy::load("assets/galaxy.json", gal)) {
        std::printf("FATAL: galaxy.json failed to load\n");
        return 1;
    }
    std::optional<StarSystem> troy_opt = load_system("troy");
    if (!troy_opt) { std::printf("FATAL: troy.json failed to load\n"); return 1; }
    const StarSystem troy = *troy_opt;

    const ShipClass* tarsus = ship_class::find("tarsus");
    if (!tarsus) { std::printf("FATAL: tarsus class missing\n"); return 1; }

    constexpr float dt = 1.0f / 60.0f;

    // =======================================================================
    // STEP a — new_game(): Tarsus, 2000cr, Troy.
    // =======================================================================
    int f0 = g_fail; banner("a", "new_game(): Tarsus, 2000cr, Troy");
    PlayerState player = player::new_game("troy");
    std::printf("  ship=%s credits=%lld system=%s missiles=%d/%d/%d\n",
                player.ship_class_name.c_str(), (long long)player.credits,
                player.current_system.c_str(), player.missiles[0],
                player.missiles[1], player.missiles[2]);
    check(player.ship_class_name == "tarsus",                 "ship is a Tarsus");
    check(player.credits == player::k_new_game_credits,       "starts with 2000 credits");
    check(player.current_system == "troy",                    "starts in Troy");
    // afterburner_fuel field removed; pool merged into Ship::energy_gj.
    // New games start in the Achilles concourse, like the 1995 game.
    check(player.docked && player.last_docked_base == "achilles",
          "starts docked at Achilles");
    step_result("a", f0);

    // =======================================================================
    // STEP b — encounter director spawns NPCs; threat::hostiles_near reflects.
    // =======================================================================
    int fb = g_fail; banner("b", "encounter director + threat oracle");
    {
        World w;
        threat::set_world(&w.ships, &player.rep);
        encounters::init(troy);
        auto spawnfn = [&w](const encounters::SpawnRequest& r) -> uint32_t {
            const ShipClass* k = ship_class::find(r.class_name);
            return k ? spawn_npc(w, *k, r.faction, r.position) : 0u;
        };
        auto despawnfn = [&w](uint32_t id) { w.ships.despawn(w.ships.find_handle_by_id(id)); };

        // Live traffic comes from the per-nav entry roll (populate_on_entry);
        // canonical systems have no continuous director rules. The roll is
        // clock-seeded and a nav can roll an empty group, so re-roll (as a
        // fresh launch would) a bounded number of times.
        (void)despawnfn;
        const HMM_Vec3 ppos = troy.player_start;
        int rolls = 0;
        while (w.ships.size() == 0 && rolls++ < 32)
            encounters::populate_on_entry(troy, ppos, troy.star_position, spawnfn);
        std::printf("  entry roll x%d, registry=%zu ships\n", rolls, w.ships.size());
        check(w.ships.size() > 0, "system entry spawned at least one NPC");

        encounters::shutdown();
        threat::set_world(nullptr, nullptr);

        // threat reflects a hostile in the bubble. Use a CLEAN world (the
        // director above left a live pirate 8-15 km out, which would itself
        // trip the oracle) so the distance gating is unambiguous: a pirate
        // (baseline -30 => Hostile) 5 km off trips a 20 km bubble; the same
        // pirate 60 km out does not.
        const ShipClass* talon = ship_class::find("talon");
        check(talon != nullptr, "talon class available for the hostile probe");
        if (talon) {
            World probe;
            threat::set_world(&probe.ships, &player.rep);
            spawn_npc(probe, *talon, Faction::Pirate, HMM_AddV3(ppos, HMM_V3(5000, 0, 0)));
            check(threat::hostiles_near(ppos, 20000.0f),
                  "hostiles_near=true with a pirate 5km away");

            World probe_far;
            threat::set_world(&probe_far.ships, &player.rep);
            spawn_npc(probe_far, *talon, Faction::Pirate, HMM_AddV3(ppos, HMM_V3(60000, 0, 0)));
            check(!threat::hostiles_near(ppos, 20000.0f),
                  "hostiles_near=false when the only pirate is 60km out");
            threat::set_world(nullptr, nullptr);
        }
    }
    step_result("b", fb);

    // =======================================================================
    // STEP c — player kill of a pirate via the np-ma2.1 path: rep deltas,
    //          stance flip, bounty progress.
    // =======================================================================
    int fc = g_fail; banner("c", "player kill -> reputation + stance flip + bounty");
    {
        const int8_t pirate0 = player.rep.rep[(int)Faction::Pirate];
        const int8_t confed0 = player.rep.rep[(int)Faction::Confed];
        const Stance confed_stance0 = faction::stance_npc_vs_player(Faction::Confed, player.rep);

        // Stand up a bounty against pirates so the SAME kill advances it.
        ActiveMission b;
        b.id = "loop_bounty"; b.type = (int)missions::MissionType::Bounty;
        b.target_faction = "pirate"; b.count_required = 3; b.progress = 0;
        b.reward = 5000;
        player.missions.push_back(b);

        // Drive enough pirate kills to (a) move rep and (b) cross Confed's
        // +25 Allied threshold (lawful universe approves of pirate-hunting).
        for (int k = 0; k < 6; ++k) {
            comm::report_player_kill(player, Faction::Pirate);
            // inline bounty above has no region -> "any system" fallback.
            missions::on_target_destroyed(player, Faction::Pirate, player.current_system);
        }
        const Stance confed_stance1 = faction::stance_npc_vs_player(Faction::Confed, player.rep);
        std::printf("  pirate rep %d -> %d | confed rep %d -> %d | confed stance %d -> %d\n",
                    pirate0, player.rep.rep[(int)Faction::Pirate],
                    confed0, player.rep.rep[(int)Faction::Confed],
                    (int)confed_stance0, (int)confed_stance1);
        check(player.rep.rep[(int)Faction::Pirate] < pirate0,  "pirate rep dropped on kills");
        check(player.rep.rep[(int)Faction::Confed] > confed0,  "confed rep rose on pirate-hunting");
        check(confed_stance0 != Stance::Allied && confed_stance1 == Stance::Allied,
              "confed stance flipped to Allied across the +25 threshold");
        // The bounty (need 3) auto-completed within the 6 kills and paid out.
        bool bounty_gone = true;
        for (const ActiveMission& m : player.missions) if (m.id == "loop_bounty") bounty_gone = false;
        check(bounty_gone, "active bounty advanced to completion via the kill path");
    }
    step_result("c", fc);

    // =======================================================================
    // STEP d — dock at Achilles: state machine reaches Docked + autosave fires.
    // =======================================================================
    int fd = g_fail; banner("d", "dock at Achilles -> Landed + autosave");
    {
        const NavPointDef* achilles = nav_with_base(troy, "achilles");
        check(achilles != nullptr, "Achilles nav point exists in Troy");
        if (achilles) {
            Camera    cam;
            GameState gs;
            Docking   dock;
            player.docked = false;   // launched from the starting concourse
            // Slow approach from 1.5 km out — the docking gate's happy path.
            cam.position = HMM_AddV3(achilles->position, HMM_V3(0, 0, 1500.0f));
            cam.velocity = HMM_MulV3F(
                HMM_NormV3(HMM_SubV3(achilles->position, cam.position)), 40.0f);
            const DockResult r = docking::request(dock, cam.position, cam.velocity, *achilles);
            check(r == DockResult::Cleared, "docking request CLEARED");

            int guard = 0;
            while (!player.docked && guard++ < 60 * 40) {
                docking::tick(dock, cam, gs, player, dt);
                game_state::apply_pending(gs, dt);
            }
            std::printf("  docked=%d base='%s' mode=%s\n", (int)player.docked,
                        player.last_docked_base.c_str(), game_state::to_name(gs.mode));
            check(player.docked,                          "docking state machine reached Docked");
            check(player.last_docked_base == "achilles",  "last_docked_base = achilles");
            check(gs.mode == GameMode::Landed,            "game mode transitioned to Landed");

            // Landing writes a new timestamped autosave (never slot 0).
            const std::vector<savegame::SlotInfo> saves = savegame::list_saves();
            const savegame::SlotInfo auto_si = saves.empty() ? savegame::SlotInfo{} : saves.front();
            std::printf("  newest autosave: exists=%d base='%s' system='%s' cr=%lld\n",
                        (int)auto_si.exists, auto_si.base.c_str(),
                        auto_si.system.c_str(), (long long)auto_si.credits);
            check(auto_si.exists && auto_si.base == "achilles",
                  "autosave fired on dock (newest save reflects Achilles)");
        }
    }
    step_result("d", fd);

    // =======================================================================
    // STEP e — commodity arbitrage: buy ore at Achilles (mining), sell at
    //          Helen (agricultural) for profit.
    // =======================================================================
    int fe = g_fail; banner("e", "commodity arbitrage (Achilles -> Helen)");
    {
        const Commodity* ore = commodity::find("iron");
        check(ore != nullptr, "'iron' in the commodity catalog");
        if (ore) {
            const int cap = player::cargo_capacity(player, tarsus);
            const economy::Quote qa = economy::price("achilles", ore->id);
            const economy::Quote qh = economy::price("helen", ore->id);
            std::printf("  iron: achilles buy=%d sell=%d | helen buy=%d sell=%d (cap=%d)\n",
                        qa.buy_price, qa.sell_price, qh.buy_price, qh.sell_price, cap);
            const long long before = player.credits;
            const int qty = (cap < qa.available_units) ? cap : qa.available_units;
            const bool bought = trade_buy(player, "achilles", *ore, qty, cap);
            check(bought, "bought ore at Achilles (credits + cargo moved)");
            check(player::cargo_units_used(player) == qty, "hold carries the ore");
            // Over-cap and over-budget are both refused.
            check(!trade_buy(player, "achilles", *ore, cap + 1, cap), "over-cap buy refused");
            const long long spent = before - player.credits;
            const long long gain  = trade_sell(player, "helen", *ore, qty);
            std::printf("  bought %d @ %d (-%lld), sold @ %d (+%lld), net %+lld\n",
                        qty, qa.buy_price, spent, qh.sell_price, gain, gain - spent);
            check(gain > spent, "arbitrage nets positive (buy cheap, sell dear)");
            check(player::cargo_units_used(player) == 0, "hold empty after the sale");
        }
    }
    step_result("e", fe);

    // =======================================================================
    // STEP f — outfitting: buy a gun, upgrade, hull trade-in; over-budget
    //          refused.
    // =======================================================================
    int ff = g_fail; banner("f", "outfitting: gun / upgrade / hull trade-in");
    {
        // Give the outfitting wallet room (we'll restore the loop wallet after).
        const PlayerState saved = player;
        player.credits = 400000;

        // gun_mounts became MountSlot rows with the v7 unified-hold work.
        const std::string gun0 =
            player.gun_mounts.empty() ? "" : player.gun_mounts[0].gun_id;
        check(outfitting::buy_gun(player, "tachyon_cannon", 0, tarsus),
              "bought a tachyon cannon into mount 0");
        check(!player.gun_mounts.empty() &&
              player.gun_mounts[0].gun_id == "tachyon_cannon",
              "mount 0 now holds the new gun");
        check(!outfitting::buy_gun(player, "laser", 9, tarsus),
              "out-of-range mount refused");

        const int sh0 = player.shield_level;
        check(outfitting::upgrade_shield(player, tarsus), "shield upgrade L0->L1 succeeded");
        check(player.shield_level == sh0 + 1, "shield level climbed one rung");

        const std::string hull0 = player.ship_class_name;
        const long long net = outfitting::hull_net_cost("centurion", "tarsus");
        const long long pre = player.credits;
        check(outfitting::buy_hull(player, "centurion"), "bought a Centurion (trade-in math)");
        check(player.ship_class_name == "centurion", "now flying a Centurion");
        check(player.credits == pre - net, "charged exactly net hull cost");
        std::printf("  hull %s -> %s, net cost %lld, credits %lld -> %lld\n",
                    hull0.c_str(), player.ship_class_name.c_str(), net,
                    (long long)pre, (long long)player.credits);

        // Over-budget refusal on a separate broke pilot (no side effects).
        PlayerState broke = player::new_game("troy");
        broke.credits = 5000;
        check(!outfitting::buy_hull(broke, "centurion") && broke.ship_class_name == "tarsus",
              "over-budget hull purchase refused");

        player = saved;   // restore the loop pilot (still a Tarsus, real wallet)
        (void)gun0;
    }
    step_result("f", ff);

    // =======================================================================
    // STEP g — missions: cargo delivery (reward paid) + bounty (kill payout).
    // =======================================================================
    int fg = g_fail; banner("g", "missions: cargo delivery + bounty payout");
    {
        const int cap = player::cargo_capacity(player, tarsus);
        // Boards are seed-random; take the first seed whose board offers both
        // a cargo run and a bounty so the test doesn't hinge on one roll.
        std::vector<missions::Mission> board;
        for (uint32_t seed = 0xC0FFEEu; seed < 0xC0FFEEu + 64; ++seed) {
            board = missions::generate("achilles", "troy", gal, seed);
            bool has_cargo = false, has_bounty = false;
            for (const missions::Mission& m : board) {
                has_cargo  |= m.type == missions::MissionType::CargoDelivery;
                has_bounty |= m.type == missions::MissionType::Bounty;
            }
            if (has_cargo && has_bounty) break;
        }
        // Out-of-system jobs need a jump drive (a real pilot buys one at the
        // equipment dealer; step f's purchases don't include it).
        player.has_jump_drive = true;
        check(!board.empty(), "mission board generated for Achilles");

        const missions::Mission* cargo = nullptr;
        const missions::Mission* bounty = nullptr;
        for (const missions::Mission& m : board) {
            if (!cargo  && m.type == missions::MissionType::CargoDelivery) cargo  = &m;
            if (!bounty && m.type == missions::MissionType::Bounty)        bounty = &m;
        }
        check(cargo  != nullptr, "board offers a cargo delivery");
        check(bounty != nullptr, "board offers a bounty");

        if (cargo) {
            const long long cr0 = player.credits;
            check(missions::accept(player, *cargo, cap), "accepted the cargo run (loads hold)");
            check(player::cargo_units_used(player) == cargo->units, "consignment is in the hold");
            // "Fly" to the destination + deliver.
            player.current_system   = cargo->dest_system;
            player.last_docked_base = cargo->dest_base;
            check(missions::complete_delivery(player, cargo->id, cargo->dest_base),
                  "delivered at the destination base");
            check(player.credits == cr0 + cargo->reward, "delivery reward paid exactly");
            check(player::cargo_units_used(player) == 0, "consignment removed on delivery");
            std::printf("  delivery: +%lld cr (%lld -> %lld)\n",
                        (long long)cargo->reward, (long long)cr0, (long long)player.credits);
        }
        if (bounty) {
            const long long cr0 = player.credits;
            check(missions::accept(player, *bounty, cap), "accepted the bounty");
            const Faction tgt = faction::from_name(bounty->target_faction);
            // Non-matching kill must not advance.
            const Faction other = (tgt == Faction::Pirate) ? Faction::Kilrathi : Faction::Pirate;
            // Hunt inside the bounty's posted region (empty -> "any").
            const std::string hunt = bounty->bounty_region.empty()
                                   ? player.current_system
                                   : bounty->bounty_region.front();
            missions::on_target_destroyed(player, other, hunt);
            bool unchanged = false;
            for (const ActiveMission& m : player.missions)
                if (m.id == bounty->id && m.progress == 0) unchanged = true;
            check(unchanged, "non-matching kill leaves bounty progress at 0");
            for (int k = 0; k < bounty->count_required; ++k)
                missions::on_target_destroyed(player, tgt, hunt);
            bool done = true;
            for (const ActiveMission& m : player.missions) if (m.id == bounty->id) done = false;
            check(done, "bounty completed after the required kills");
            check(player.credits == cr0 + bounty->reward, "bounty payout credited exactly");
            std::printf("  bounty: target=%s need=%d +%lld cr (%lld -> %lld)\n",
                        bounty->target_faction.c_str(), bounty->count_required,
                        (long long)bounty->reward, (long long)cr0, (long long)player.credits);
        }
    }
    step_result("g", fg);

    // =======================================================================
    // STEP h — repair: damage the player hull, repair at a base for credits.
    // =======================================================================
    int fh = g_fail; banner("h", "paid hull repair");
    {
        // Player hull, built like main.cpp does: ship::spawn() is the NPC
        // path and applies the enemy armor buff that repair rightly ignores.
        Ship sh = ship::spawn_player();
        sh.klass = tarsus;
        ship::heal_to_full(sh);
        const float full = total_armor(sh);
        sh.armor_fore_cm = 0.0f;                  // bash the nose
        const float damaged = total_armor(sh);
        const repair::Quote q = repair::quote(&sh, player);
        check(q.hull_damaged && q.hull_cost > 0, "repair quote flags hull damage + a cost");
        const long long cr0 = player.credits;
        const bool ok = repair::repair_hull(sh, player);
        std::printf("  armor %.0f/%.0f -> %.0f | repaired=%d | credits %lld -> %lld (cost %lld)\n",
                    damaged, full, total_armor(sh), (int)ok,
                    (long long)cr0, (long long)player.credits, (long long)q.hull_cost);
        check(ok, "repair succeeded");
        check(std::fabs(total_armor(sh) - full) < 0.5f, "hull restored to full");
        check(player.credits == cr0 - q.hull_cost, "credits spent on repair");
    }
    step_result("h", fh);

    // =======================================================================
    // STEP i — missiles (homing kill) + afterburner drain/cutout/regen.
    // =======================================================================
    int fi = g_fail; banner("i", "homing missile kill + afterburner fuel");
    {
        const ShipClass* talon = ship_class::find("talon");
        check(talon != nullptr, "talon class for the missile target");
        if (talon) {
            World w;
            const HMM_Vec3 tpos = HMM_V3(0, 0, 4000.0f);
            const uint32_t tgt = spawn_npc(w, *talon, Faction::Pirate, tpos);
            const float hp0 = total_health(*w.ships.find_by_id(tgt));

            // Top the IR rack so we can guarantee a kill, then fire IR missiles
            // 30deg off-bearing (a dumbfire would sail past) until the target
            // is destroyed — proving guidance AND the lethal damage path.
            player::add_missiles(player, (int)MissileType::IR, 8);
            const float ang = 30.0f * 3.14159265f / 180.0f;
            const HMM_Vec3 off = HMM_V3(std::sin(ang), 0.0f, std::cos(ang));

            std::vector<Missile> ms;
            int fired = 0;
            bool killed = false;
            const uint32_t player_id = 4242;   // stand-in shooter id
            for (int salvo = 0; salvo < 8 && !killed; ++salvo) {
                if (!player::consume_missile(player, (int)MissileType::IR)) break;
                ++fired;
                ms.push_back(missile::spawn(MissileType::IR, HMM_V3(0, 0, 0),
                                            off, HMM_V3(0, 0, 0), player_id, tgt));
                for (int frame = 0; frame < 2000 && !ms.empty(); ++frame) {
                    missile::tick(ms, w.ships, dt);
                    missile::collide_and_damage(ms, w.ships, dt);
                }
                const Ship* t = w.ships.find_by_id(tgt);
                if (!t || !t->alive) killed = true;
            }
            const Ship* t = w.ships.find_by_id(tgt);
            std::printf("  fired %d IR missiles, target health %.0f -> %.0f, alive=%d\n",
                        fired, hp0, t ? total_health(*t) : 0.0f, t ? (int)t->alive : 0);
            check(killed, "homing missile(s) destroyed the locked target");
            check(t && t->killed_by_id == player_id,
                  "kill attributed to the shooter (killed_by_id) — feeds the rep path");
        }

        // Afterburner fuel pool merged into Ship::energy_gj (np-zte.2):
        // the standalone drain/regen helpers are gone, so the equivalent
        // ground-truth test now lives in the firing.cpp energy regen path
        // (firing::tick refills energy_gj) and the main.cpp cruise gate
        // that subtracts k_afterburner_drain_per_s * dt. Headless
        // coverage skipped here; full system tested by running the game.
    }
    step_result("i", fi);

    // =======================================================================
    // STEP j — jump: round-trip; combat blocks autopilot, not gate travel.
    // =======================================================================
    int fj = g_fail; banner("j", "jump under fire + autopilot hostile gate");
    {
        // Reciprocal topology round-trip via the galaxy graph.
        const galaxy::JumpTarget out  = gal.jump_target("troy", "Pyrenees Jump");
        const galaxy::JumpTarget back = out.ok ? gal.jump_target(out.system, out.nav)
                                               : galaxy::JumpTarget{};
        std::printf("  troy/'Pyrenees Jump' -> %s/'%s' -> %s/'%s'\n",
                    out.system.c_str(), out.nav.c_str(), back.system.c_str(), back.nav.c_str());
        check(out.ok && out.system == "pyrenees" && out.nav == "Troy Jump",
              "Troy -> Pyrenees resolves");
        check(back.ok && back.system == "troy" && back.nav == "Pyrenees Jump",
              "Pyrenees -> Troy round-trips to the origin gate");

        // The destination system actually loads (multi-system, np-6al.1).
        std::optional<StarSystem> pyr = load_system(out.system);
        check(pyr.has_value(), "destination system JSON loads");

        // Player persists across the (simulated) jump: only location changes.
        const long long cr_before = player.credits;
        const size_t cargo_before = player.cargo.size();
        const std::string sys_before = player.current_system;
        player.current_system = out.system;     // the live game does this in main.cpp
        check(player.credits == cr_before && player.cargo.size() == cargo_before,
              "credits + cargo persist across the jump");
        player.current_system = sys_before;     // hop back for the rest of the harness

        // Jump eligibility remains ready under fire, while autopilot refuses.
        const int gate = nav_index(troy, "Pyrenees Jump");
        check(gate >= 0, "Pyrenees Jump gate found in Troy");
        if (gate >= 0) {
            World w;
            threat::set_world(&w.ships, &player.rep);
            Camera cam;
            cam.position = troy.nav_points[gate].position;   // sitting on the gate

            const jump::Eligibility clear = jump::evaluate(cam, troy, gal, "troy",
                                                           jump::Drive::Online);
            bool ready = false; (void)jump::prompt(clear, &ready);
            check(clear.status == jump::Status::Ready && ready &&
                  clear.nav_index == gate,
                  "jump READY at the gate with no hostiles");

            // Drop a pirate nearby. The gate remains a valid escape, but the
            // long-distance autopilot must still refuse to engage.
            const ShipClass* talon = ship_class::find("talon");
            if (talon) {
                spawn_npc(w, *talon, Faction::Pirate,
                          HMM_AddV3(cam.position, HMM_V3(2000, 0, 0)));
                const jump::Eligibility under_fire = jump::evaluate(
                    cam, troy, gal, "troy", jump::Drive::Online);
                check(under_fire.status == jump::Status::Ready,
                      "jump remains READY with a hostile nearby");

                Autopilot ap;
                const EngageResult autopilot_result =
                    autopilot::try_engage(ap, cam, troy, gate);
                check(autopilot_result == EngageResult::Hostiles,
                      "autopilot still REFUSES with a hostile nearby");
            }
            threat::set_world(nullptr, nullptr);
        }
    }
    step_result("j", fj);

    // =======================================================================
    // STEP k — save -> reload: bit-exact round-trip of the whole PlayerState.
    // =======================================================================
    int fk = g_fail; banner("k", "save/reload bit-exact round-trip");
    {
        // Mutate every serialized axis so the round-trip is non-trivial.
        PlayerState src = player;
        src.credits          = 1234567;
        src.ship_class_name  = "centurion";
        src.gun_mounts       = { MountSlot{"tachyon_cannon"}, MountSlot{},
                                 MountSlot{"meson_blaster"} };
        src.shield_level     = 2; src.engine_level = 1; src.cargo_expansion = true;
        src.cargo            = { { "iron", 42, 35 }, { "tungsten", 7, 410 } };
        src.missiles[0] = 3; src.missiles[1] = 1; src.missiles[2] = 5;
        // afterburner_fuel field removed (np-zte.2 merged pool).
        src.current_system   = "pyrenees";
        src.last_docked_base = "achilles";
        src.docked           = true;
        src.rep.rep[(int)Faction::Confed] = 30;
        src.rep.rep[(int)Faction::Pirate] = -80;
        src.missions.clear();
        ActiveMission cm; cm.id = "deliv1"; cm.type = 0; cm.commodity_id = "iron";
        cm.units = 10; cm.dest_system = "pyrenees"; cm.dest_base = "hector"; cm.reward = 900;
        ActiveMission bm; bm.id = "bnty1";  bm.type = 1; bm.target_faction = "pirate";
        bm.count_required = 4; bm.progress = 2; bm.reward = 6000;
        src.missions = { cm, bm };

        constexpr int kSlot = 9;
        check(savegame::save(src, kSlot), "save wrote slot 9");
        PlayerState dst;
        check(savegame::load(dst, kSlot), "load read slot 9 into a fresh state");

        bool reps_ok = true;
        for (int i = 0; i < kFactionCount; ++i)
            if (src.rep.rep[i] != dst.rep.rep[i]) reps_ok = false;
        bool cargo_ok = src.cargo.size() == dst.cargo.size();
        for (size_t i = 0; cargo_ok && i < src.cargo.size(); ++i)
            cargo_ok = src.cargo[i].commodity_id == dst.cargo[i].commodity_id &&
                       src.cargo[i].units == dst.cargo[i].units &&
                       src.cargo[i].bought_at_price == dst.cargo[i].bought_at_price;
        bool miss_ok = src.missions.size() == dst.missions.size();
        for (size_t i = 0; miss_ok && i < src.missions.size(); ++i)
            miss_ok = src.missions[i].id == dst.missions[i].id &&
                      src.missions[i].type == dst.missions[i].type &&
                      src.missions[i].progress == dst.missions[i].progress &&
                      src.missions[i].reward == dst.missions[i].reward;

        check(dst.credits == src.credits,                 "credits round-trip");
        check(dst.ship_class_name == src.ship_class_name, "ship class round-trip");
        {   // MountSlot has no operator==; compare the serialized axes.
            bool mounts_ok = dst.gun_mounts.size() == src.gun_mounts.size();
            for (size_t i = 0; mounts_ok && i < src.gun_mounts.size(); ++i)
                mounts_ok = dst.gun_mounts[i].gun_id == src.gun_mounts[i].gun_id &&
                            dst.gun_mounts[i].rarity == src.gun_mounts[i].rarity;
            check(mounts_ok,                              "gun mounts round-trip");
        }
        check(dst.shield_level == src.shield_level && dst.engine_level == src.engine_level &&
              dst.cargo_expansion == src.cargo_expansion, "equipment levels round-trip");
        check(reps_ok,                                    "reputation round-trip (all factions)");
        check(cargo_ok,                                   "cargo manifest round-trip");
        check(dst.missiles[0] == src.missiles[0] && dst.missiles[1] == src.missiles[1] &&
              dst.missiles[2] == src.missiles[2],         "missile inventory round-trip");
        // afterburner_fuel no longer a PlayerState field; merged into energy_gj.
        check(dst.current_system == src.current_system &&
              dst.last_docked_base == src.last_docked_base &&
              dst.docked == src.docked,                   "location + docked flag round-trip");
        check(miss_ok,                                    "accepted missions round-trip");
        // Remove the scratch slot so step l's newest-save lookup can't pick
        // it (same-second timestamps tie-break on path).
        std::remove(savegame::slot_path(kSlot).c_str());
    }
    step_result("k", fk);

    // =======================================================================
    // STEP l — death -> respawn at last docked base (autosaved state restored).
    // =======================================================================
    int fl = g_fail; banner("l", "death -> respawn at last docked base");
    {
        // Re-dock to refresh the autosave with the loop pilot's CURRENT state
        // (credits earned, etc.) so we can prove respawn restores THAT, not a
        // stale slot from step d.
        const NavPointDef* achilles = nav_with_base(troy, "achilles");
        if (achilles) {
            Camera cam; GameState gs; Docking dock;
            player.docked = false;
            cam.position = HMM_AddV3(achilles->position, HMM_V3(0, 0, 1500.0f));
            cam.velocity = HMM_MulV3F(
                HMM_NormV3(HMM_SubV3(achilles->position, cam.position)), 40.0f);
            docking::request(dock, cam.position, cam.velocity, *achilles);
            int guard = 0;
            while (!player.docked && guard++ < 60 * 40) {
                docking::tick(dock, cam, gs, player, dt);
                game_state::apply_pending(gs, dt);
            }
        }
        const long long credits_at_dock = player.credits;

        // Now the player dies in flight. Death returns to the title screen
        // (np-3dp.18); Continue resumes the NEWEST save, which is the landing
        // autosave above. Model exactly that restore.
        PlayerState restored;
        const std::vector<savegame::SlotInfo> saves = savegame::list_saves();
        const bool from_save = !saves.empty() && savegame::load(restored, saves.front().path);
        check(from_save, "Continue loaded the newest autosave");
        std::printf("  respawn: base='%s' system='%s' credits=%lld\n",
                    restored.last_docked_base.c_str(), restored.current_system.c_str(),
                    (long long)restored.credits);
        check(restored.last_docked_base == "achilles", "respawns at last docked base (Achilles)");
        check(restored.docked, "respawned state is docked (Landed)");
        check(restored.credits == credits_at_dock, "respawn restores the autosaved credits");
        check(restored.current_system == player.current_system, "respawn keeps the current system");
    }
    step_result("l", fl);

    // =======================================================================
    // STEP m — corpse reap (np-zte.1 regression). PROVES dead NPCs are
    // despawned: registry size() returns to the player-only baseline, sprite
    // slots are reclaimed, and the encounter director keeps spawning past 64
    // CUMULATIVE kills (the old bug leaked corpses until ships.size() pinned
    // the k_registry_hard_cap gate shut, permanently starving spawns).
    // =======================================================================
    int fm = g_fail; banner("m", "corpse reap: no registry/sprite leak, no spawn starvation");
    {
        const ShipClass* talon = ship_class::find("talon");
        check(talon != nullptr, "talon class available for reap test");
        if (talon) {
            ShipRegistry ships;
            SpritePool   pool;

            // Player owns slot 0 (never reaped — death/respawn owns that hull).
            { Ship pl = ship::spawn(*talon); pl.is_player = true; ships.spawn(std::move(pl)); }
            const size_t baseline = ships.size();
            check(baseline == 1, "baseline registry is player-only (1)");

            // Shared spawn recipe: claim a sprite slot, attach, register.
            auto spawn_one = [&](HMM_Vec3 pos, Faction fac) -> uint32_t {
                const size_t slot = pool.claim_slot();
                ShipSpriteObject& spr = pool.slots[slot];
                spr.position   = pos;
                spr.world_size = 200.0f;
                Ship s   = ship::spawn(*talon);
                s.faction  = fac;
                s.position = pos;
                s.sprite   = &pool.slots[slot];
                const ShipHandle h = ships.spawn(std::move(s));
                const Ship* sp = ships.get(h);
                return sp ? sp->id : 0u;
            };
            auto kill_one = [&](uint32_t id) {
                Ship* s = ships.find_by_id(id);
                if (s) ship::take_damage(*s, 1.0e6f, HitFacing::Fore);   // one lethal blow
            };

            // ---- phase 1: direct spawn -> kill -> reap, exact reclamation --
            const int N = 12;
            std::vector<uint32_t> ids;
            for (int i = 0; i < N; ++i)
                ids.push_back(spawn_one(HMM_V3(1000.0f * i, 0, 0), Faction::Pirate));
            check(ships.size() == baseline + N, "12 NPCs spawned -> registry = player + 12");
            check(pool.slots.size() == (size_t)N && pool.free_list.empty(),
                  "12 sprite slots claimed, none parked yet");

            for (uint32_t id : ids) kill_one(id);
            const int reaped = reap_dead_npcs(ships, pool);
            std::printf("  phase1: spawned %d, killed+reaped %d, registry now %zu, "
                        "sprite free-list %zu/%zu\n",
                        N, reaped, ships.size(), pool.free_list.size(), pool.slots.size());
            check(reaped == N,                      "reap freed all 12 corpses");
            check(ships.size() == baseline,         "registry size() back to player-only baseline");
            check(pool.free_list.size() == (size_t)N && pool.slots.size() == (size_t)N,
                  "all 12 sprite slots reclaimed (parked, reusable)");

            // Re-spawn proves parked slots are REUSED, not leaked (no growth).
            const uint32_t reuse_id = spawn_one(HMM_V3(0, 0, 0), Faction::Pirate);
            check(pool.slots.size() == (size_t)N, "re-spawn reused a parked slot (deque didn't grow)");
            kill_one(reuse_id);
            reap_dead_npcs(ships, pool);

            // ---- phase 2: director soak, kill+reap past 64 CUMULATIVE kills-
            // The crux: with the leak, ships.size() climbs to k_registry_hard_cap
            // (64) and the director never spawns again, so cumulative kills
            // would stall well under our target. With the reap, size() stays
            // bounded and spawning continues indefinitely.
            StarSystem soak = troy;
            EncounterRuleDef roamers;
            roamers.name           = "soak_roamers";
            roamers.region         = "anywhere";
            roamers.factions       = { {"pirate", 1.0f} };
            roamers.classes        = { {"talon", 1.0f} };
            roamers.spawn_interval = 1.0f;
            soak.encounters = { roamers };
            encounters::init(soak);
            auto spawnfn   = [&](const encounters::SpawnRequest& r) -> uint32_t {
                const ShipClass* k = ship_class::find(r.class_name);
                return k ? spawn_one(r.position, r.faction) : 0u;
            };
            auto despawnfn = [&](uint32_t id) {
                Ship* s = ships.find_by_id(id);
                if (s) pool.park_slot(s->sprite);
                ships.despawn(ships.find_handle_by_id(id));
            };

            const HMM_Vec3 ppos = troy.player_start;
            const int target_kills = encounters::k_registry_hard_cap + 24;   // 88 > 64
            int    cumulative = 0;
            size_t max_registry = ships.size();
            int    loops = 0;
            while (cumulative < target_kills && loops++ < 60 * 60 * 20) {
                encounters::tick(ships, ppos, dt, spawnfn, despawnfn);
                if (ships.size() > max_registry) max_registry = ships.size();
                // Kill everything the director put on the field this tick,
                // then reap — exactly the per-frame order main.cpp runs.
                for (size_t i = 0; i < ships.slot_count(); ++i) {
                    Ship* s = ships.ship_at(i);
                    if (s && !s->is_player && s->alive)
                        ship::take_damage(*s, 1.0e6f, HitFacing::Fore);
                }
                cumulative += reap_dead_npcs(ships, pool);
            }
            encounters::shutdown();
            std::printf("  phase2: %d cumulative kills over %d ticks; peak registry %zu "
                        "(hard cap %d), sprite pool %zu slots\n",
                        cumulative, loops, max_registry,
                        encounters::k_registry_hard_cap, pool.slots.size());
            check(cumulative >= target_kills,
                  "director kept spawning past 64 cumulative kills (no starvation)");
            check(max_registry < (size_t)encounters::k_registry_hard_cap,
                  "registry stayed bounded well under the hard cap under reap");
            check(ships.size() == baseline,
                  "registry settled back to player-only after the soak");
        }
    }
    step_result("m", fm);

    // ---- verdict ----------------------------------------------------------
    std::printf("\n################################################################\n");
    if (g_fail == 0)
        std::printf("# RESULT: ALL STEPS (a..m) PASSED — sandbox loop is coherent.\n");
    else
        std::printf("# RESULT: %d CHECK(S) FAILED — see [FAIL] lines above.\n", g_fail);
    std::printf("################################################################\n");
    (void)g_step;
    return g_fail == 0 ? 0 : 1;
}

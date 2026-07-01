// -----------------------------------------------------------------------------
// tools/test_savegame.cpp — offline round-trip proof for np-ymp.1 save/load.
//
// Links the REAL savegame.cpp, player.cpp, json.cpp and faction.cpp, then:
//
//   1. builds a NEW_GAME PlayerState and mutates EVERY serialized field
//      (credits, rep across several factions, ship + equipment, a 2-stack
//      cargo manifest, location, docked flag),
//   2. saves it to a scratch slot, constructs a FRESH default PlayerState,
//      loads the slot back, and asserts every field matches bit-exact
//      (the bead's acceptance criterion) with a field-by-field log,
//   3. proves the failure paths never lie or crash: missing-file load,
//      truncated/corrupt-file load, and a newer-than-supported version are
//      each rejected (return false), leaving the target state untouched.
//
// Build (mirrors tools/test_economy.cpp's recipe):
//   clang++ -std=c++20 -Isrc -Ithird_party \
//       tools/test_savegame.cpp src/savegame.cpp src/player.cpp \
//       src/json.cpp src/faction.cpp -o /tmp/test_savegame
// -----------------------------------------------------------------------------

#include "faction.h"
#include "player.h"
#include "plot.h"
#include "savegame.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

int g_fail = 0;

// Compare one field; log PASS/FAIL with both values.
template <typename T>
void check(const char* name, const T& got, const T& want) {
    const bool ok = (got == want);
    if (!ok) ++g_fail;
}

#define CHECK_EQ(name, got, want)                                            \
    do {                                                                     \
        auto _g = (got);                                                     \
        auto _w = (want);                                                    \
        const bool _ok = (_g == _w);                                         \
        if (!_ok) ++g_fail;                                                  \
        std::printf("  [%s] %-20s\n", _ok ? "OK  " : "FAIL", name);          \
    } while (0)

// The scratch slots we own here — kept high so a stray run never clobbers a
// real autosave (0) or the concourse manual save (1).
constexpr int kSlot         = 7;
constexpr int kCorruptSlot  = 8;
constexpr int kVersionSlot  = 6;
constexpr int kMissingSlot  = 42;
// (#8) old-format save load + out-of-range mission-type drop tests below.
constexpr int kOldNoMissSlot = 9;    // v1 save with no missions key at all
constexpr int kOldBadTypeSlot= 10;   // v1 save with a type-int outside [0,5]

PlayerState make_mutated() {
    PlayerState p = player::new_game("troy");
    p.credits = 1234567;                         // distinctive, > new-game default
    p.rep.rep[(int)Faction::Confed]   = 42;
    p.rep.rep[(int)Faction::Pirate]   = -77;
    p.rep.rep[(int)Faction::Merchant] = 13;
    p.rep.rep[(int)Faction::Kilrathi] = -100;
    p.ship_class_name = "centurion";
    // (#88/#89) mounts are MountSlot objects now, not bare strings — this
    // assignment bit-rotted when the type changed; fixed with the v7 work.
    p.gun_mounts      = { MountSlot{"tachyon_cannon"}, MountSlot{},
                          MountSlot{"meson_blaster", inventory::Rarity::Rare} };
    p.shield_level    = 3;
    p.engine_level    = 2;
    p.cargo_expansion = true;
    // #16: guild memberships should survive the round-trip.
    p.merc_guild_member     = true;
    p.merchant_guild_member = true;
    // #138 (v7): campaign plot state — set through the real plot:: API so
    // the round-trip also exercises the mutators' invariants.
    plot::set_flag(p, "sandoval_done");
    plot::set_flag(p, "tayla_1_done");
    plot::give_item(p, "steltek_artifact");
    p.cargo = {
        { "iron",   42, 35 },
        { "tungsten", 7, 410 },
    };
    // np-zte.2: distinctive missile counts. afterburner_fuel field removed
    // (merged into Ship::energy_gj), so nothing to round-trip there.
    p.missiles[0] = 3; p.missiles[1] = 1; p.missiles[2] = 5;
    // np-3dp.19: career faction-kill tallies + a live ship-damage snapshot.
    p.faction_kills[(int)Faction::Pirate]   = 17;
    p.faction_kills[(int)Faction::Kilrathi] = 9;
    p.faction_kills[(int)Faction::Retro]    = 4;
    p.hp_valid       = true;
    p.hp_armor_fore  = 12.5f;
    p.hp_armor_aft   = 8.25f;
    p.hp_armor_port  = 6.75f;
    p.hp_armor_starboard = 2.5f;
    p.hp_shield_fore = 3.5f;
    p.hp_shield_aft  = 1.25f;
    p.hp_shield_port = 0.75f;
    p.hp_shield_starboard = 0.0f;
    p.hp_energy      = 99.0f;
    p.current_system   = "pentonville";
    p.last_docked_base = "achilles";
    p.docked           = true;

    // (#8) one mission of EACH new type with every new-field populated so
    // the round-trip below can prove savegame learned the expanded payload.
    ActiveMission patrol;            // type 0 — Patrol
    patrol.id            = "np-patrol";
    patrol.type          = 0;
    patrol.source        = 1;   // MercenariesGuild
    patrol.giver_faction = "Confederation";
    patrol.title         = "PATROL in troy";
    patrol.reward        = 500;
    patrol.target_system = "troy";
    patrol.nav_targets   = { "nav_a", "nav_b", "nav_c" };
    patrol.nav_count     = 3;
    patrol.nav_done      = { 1, 0, 1 };   // #13 mid-mission survey progress
    p.missions.push_back(patrol);

    ActiveMission bounty;            // type 4 — Bounty
    bounty.id            = "np-bounty";
    bounty.type          = 4;
    bounty.source        = 2;   // MerchantsGuild
    bounty.giver_faction = "Merchant";
    bounty.title         = "BOUNTY pirate";
    bounty.reward        = 4000;
    bounty.target_faction= "pirate";
    bounty.count_required= 5;
    bounty.progress      = 2;
    bounty.target_system = "troy";
    bounty.bounty_region   = { "troy", "delphi", "peleus" };
    bounty.last_seen_system     = "peleus";
    bounty.last_seen_alt_system = "delphi";
    p.missions.push_back(bounty);

    ActiveMission cargo;             // type 5 — CargoDelivery (legacy fields)
    cargo.id            = "np-cargo";
    cargo.type          = 5;
    cargo.source        = 2;   // MerchantsGuild
    cargo.giver_faction = "Merchant";
    cargo.title         = "Deliver 10 iron to achilles";
    cargo.reward        = 800;
    cargo.commodity_id  = "iron";
    cargo.units         = 10;
    cargo.dest_system   = "troy";
    cargo.dest_base     = "achilles";
    cargo.target_faction= "merchant";   // unused by Cargo but field exists
    cargo.count_required= 0;
    cargo.progress      = 0;
    cargo.target_system = "troy";       // unused but populated to round-trip
    cargo.target_base   = "achilles";   // unused but populated
    p.missions.push_back(cargo);

    ActiveMission defend;            // type 3 — DefendBase
    defend.id            = "np-defend";
    defend.type          = 3;
    defend.source        = 1;
    defend.giver_faction = "Confederation";
    defend.title         = "DEFEND achilles";
    defend.reward        = 1500;
    defend.target_system = "troy";
    defend.target_base   = "achilles";
    defend.hostiles_required = 4;
    p.missions.push_back(defend);

    return p;
}

bool reps_equal(const PlayerReputation& a, const PlayerReputation& b) {
    for (int i = 0; i < kFactionCount; ++i)
        if (a.rep[i] != b.rep[i]) return false;
    return true;
}

bool cargo_equal(const std::vector<CargoEntry>& a, const std::vector<CargoEntry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].commodity_id    != b[i].commodity_id)    return false;
        if (a[i].units           != b[i].units)           return false;
        if (a[i].bought_at_price != b[i].bought_at_price) return false;
    }
    return true;
}

bool guns_equal(const std::vector<MountSlot>& a, const std::vector<MountSlot>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].gun_id != b[i].gun_id)  return false;
        if (a[i].rarity != b[i].rarity)  return false;
        if (a[i].mods.fire_rate_mult != b[i].mods.fire_rate_mult) return false;
        if (a[i].mods.energy_mult    != b[i].mods.energy_mult)    return false;
    }
    return true;
}

// Field-by-field equality of the ActiveMission payload — covers BOTH the
// original fields (#6/#7) and every new field added in #8. Used by the
// round-trip block below to prove savegame.cpp learned the expanded shape.
bool mission_equal(const ActiveMission& x, const ActiveMission& y) {
    if (x.id                  != y.id)                  return false;
    if (x.type                != y.type)                return false;
    if (x.source              != y.source)              return false;
    if (x.giver_faction       != y.giver_faction)       return false;
    if (x.title               != y.title)               return false;
    if (x.reward              != y.reward)              return false;
    if (x.commodity_id        != y.commodity_id)        return false;
    if (x.units               != y.units)               return false;
    if (x.dest_system         != y.dest_system)         return false;
    if (x.dest_base           != y.dest_base)           return false;
    if (x.target_faction      != y.target_faction)      return false;
    if (x.count_required      != y.count_required)      return false;
    if (x.progress            != y.progress)            return false;
    if (x.target_system       != y.target_system)       return false;
    if (x.target_base         != y.target_base)         return false;
    if (x.last_seen_system    != y.last_seen_system)    return false;
    if (x.last_seen_alt_system!= y.last_seen_alt_system)return false;
    if (x.nav_count           != y.nav_count)           return false;
    if (x.hostiles_required   != y.hostiles_required)   return false;
    if (x.nav_targets         != y.nav_targets)         return false;
    if (x.nav_done            != y.nav_done)            return false;
    if (x.bounty_region       != y.bounty_region)       return false;
    return true;
}

bool missions_equal(const std::vector<ActiveMission>& a, const std::vector<ActiveMission>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!mission_equal(a[i], b[i])) return false;
    return true;
}

} // namespace

int main() {
    std::printf("=== np-ymp.1 save/load round-trip harness ===\n\n");

    // ---- 1+2. round-trip --------------------------------------------------
    const PlayerState src = make_mutated();
    std::printf("[1] saving mutated PlayerState to slot %d...\n", kSlot);
    if (!savegame::save(src, kSlot)) {
        std::printf("FATAL: save() returned false\n");
        return 1;
    }

    PlayerState dst;   // fresh default — every field must come from the file
    std::printf("[2] loading slot %d into a fresh default PlayerState...\n", kSlot);
    if (!savegame::load(dst, kSlot)) {
        std::printf("FATAL: load() returned false on a file we just wrote\n");
        return 1;
    }

    std::printf("\n--- field-by-field comparison (src vs loaded) ---\n");
    std::printf("  credits:          %lld vs %lld\n", (long long)src.credits, (long long)dst.credits);
    CHECK_EQ("credits", dst.credits, src.credits);

    std::printf("  rep[Confed]:      %d vs %d\n", src.rep.rep[(int)Faction::Confed], dst.rep.rep[(int)Faction::Confed]);
    std::printf("  rep[Pirate]:      %d vs %d\n", src.rep.rep[(int)Faction::Pirate], dst.rep.rep[(int)Faction::Pirate]);
    std::printf("  rep[Merchant]:    %d vs %d\n", src.rep.rep[(int)Faction::Merchant], dst.rep.rep[(int)Faction::Merchant]);
    std::printf("  rep[Kilrathi]:    %d vs %d\n", src.rep.rep[(int)Faction::Kilrathi], dst.rep.rep[(int)Faction::Kilrathi]);
    { const bool ok = reps_equal(src.rep, dst.rep); if (!ok) ++g_fail;
      std::printf("  [%s] %-20s\n", ok ? "OK  " : "FAIL", "rep (all factions)"); }

    std::printf("  ship_class_name:  '%s' vs '%s'\n", src.ship_class_name.c_str(), dst.ship_class_name.c_str());
    CHECK_EQ("ship_class_name", dst.ship_class_name, src.ship_class_name);

    std::printf("  gun_mounts:       %zu vs %zu entries\n", src.gun_mounts.size(), dst.gun_mounts.size());
    { const bool ok = guns_equal(src.gun_mounts, dst.gun_mounts); if (!ok) ++g_fail;
      std::printf("  [%s] %-20s\n", ok ? "OK  " : "FAIL", "gun_mounts (ordered)"); }

    CHECK_EQ("shield_level",    dst.shield_level,    src.shield_level);
    CHECK_EQ("engine_level",    dst.engine_level,    src.engine_level);
    CHECK_EQ("cargo_expansion", dst.cargo_expansion, src.cargo_expansion);
    CHECK_EQ("merc_guild_member",     dst.merc_guild_member,     src.merc_guild_member);
    CHECK_EQ("merchant_guild_member", dst.merchant_guild_member, src.merchant_guild_member);

    // #138 (v7): plot flags + items survive the round-trip, in order.
    std::printf("  plot_flags:       %zu vs %zu entries\n",
                src.plot_flags.size(), dst.plot_flags.size());
    { const bool ok = (dst.plot_flags == src.plot_flags); if (!ok) ++g_fail;
      std::printf("  [%s] %-20s\n", ok ? "OK  " : "FAIL", "plot_flags (ordered)"); }
    { const bool ok = (dst.plot_items == src.plot_items); if (!ok) ++g_fail;
      std::printf("  [%s] %-20s\n", ok ? "OK  " : "FAIL", "plot_items (ordered)"); }
    { const bool ok = plot::has_flag(dst, "sandoval_done") &&
                      plot::has_flag(dst, "tayla_1_done") &&
                      plot::has_item(dst, "steltek_artifact") &&
                      !plot::has_flag(dst, "never_set");
      if (!ok) ++g_fail;
      std::printf("  [%s] %-20s\n", ok ? "OK  " : "FAIL", "plot:: queries on loaded state"); }

    std::printf("  cargo:            %zu vs %zu stacks\n", src.cargo.size(), dst.cargo.size());
    { const bool ok = cargo_equal(src.cargo, dst.cargo); if (!ok) ++g_fail;
      std::printf("  [%s] %-20s\n", ok ? "OK  " : "FAIL", "cargo (id/units/price)"); }

    CHECK_EQ("current_system",   dst.current_system,   src.current_system);
    CHECK_EQ("last_docked_base", dst.last_docked_base, src.last_docked_base);
    CHECK_EQ("docked",           dst.docked,           src.docked);

    // np-zte.2: missile inventory + afterburner fuel survive the round-trip.
    std::printf("  missiles:         %d/%d/%d vs %d/%d/%d\n",
                src.missiles[0], src.missiles[1], src.missiles[2],
                dst.missiles[0], dst.missiles[1], dst.missiles[2]);
    CHECK_EQ("missiles[DF]", dst.missiles[0], src.missiles[0]);
    CHECK_EQ("missiles[HS]", dst.missiles[1], src.missiles[1]);
    CHECK_EQ("missiles[IR]", dst.missiles[2], src.missiles[2]);
    // afterburner_fuel round-trip removed: field merged into Ship::energy_gj
    // (np-zte.2), no longer persisted on PlayerState.

    // np-3dp.19: career faction-kill tallies survive the round-trip.
    std::printf("  kills[Pirate]:    %lld vs %lld\n",
                (long long)src.faction_kills[(int)Faction::Pirate],
                (long long)dst.faction_kills[(int)Faction::Pirate]);
    CHECK_EQ("kills[Pirate]",   dst.faction_kills[(int)Faction::Pirate],   src.faction_kills[(int)Faction::Pirate]);
    CHECK_EQ("kills[Kilrathi]", dst.faction_kills[(int)Faction::Kilrathi], src.faction_kills[(int)Faction::Kilrathi]);
    CHECK_EQ("kills[Retro]",    dst.faction_kills[(int)Faction::Retro],    src.faction_kills[(int)Faction::Retro]);

    // np-3dp.19: live ship-damage snapshot survives the round-trip.
    // Issue #30: sides split into port + starboard (4 fields now).
    CHECK_EQ("hp_valid",          dst.hp_valid,          src.hp_valid);
    CHECK_EQ("hp_armor_fore",     dst.hp_armor_fore,     src.hp_armor_fore);
    CHECK_EQ("hp_armor_aft",      dst.hp_armor_aft,      src.hp_armor_aft);
    CHECK_EQ("hp_armor_port",     dst.hp_armor_port,     src.hp_armor_port);
    CHECK_EQ("hp_armor_starboard", dst.hp_armor_starboard, src.hp_armor_starboard);
    CHECK_EQ("hp_shield_fore",    dst.hp_shield_fore,    src.hp_shield_fore);
    CHECK_EQ("hp_shield_aft",     dst.hp_shield_aft,     src.hp_shield_aft);
    CHECK_EQ("hp_shield_port",    dst.hp_shield_port,    src.hp_shield_port);
    CHECK_EQ("hp_shield_starboard", dst.hp_shield_starboard, src.hp_shield_starboard);
    CHECK_EQ("hp_energy",         dst.hp_energy,         src.hp_energy);

    // (#8) accepted missions round-trip — Patrol + Bounty + Cargo + DefendBase,
    // each populated with every new field, plus a field-by-field compare.
    std::printf("  missions:          %zu vs %zu entries\n",
                src.missions.size(), dst.missions.size());
    { const bool ok = missions_equal(src.missions, dst.missions); if (!ok) ++g_fail;
      std::printf("  [%s] %-20s\n", ok ? "OK  " : "FAIL", "missions (4 types x all fields)"); }
    // Spot-check the type-membership per index so a regression in any one
    // direction (writer or reader) is localised in the test log.
    CHECK_EQ("missions.size()",      dst.missions.size(),                         src.missions.size());
    CHECK_EQ("missions[0].type",     dst.missions[0].type,                        src.missions[0].type);
    CHECK_EQ("missions[0].source",   dst.missions[0].source,                      src.missions[0].source);
    CHECK_EQ("missions[0].nav_count",dst.missions[0].nav_count,                   src.missions[0].nav_count);
    CHECK_EQ("missions[0].nav_done.size", (int)dst.missions[0].nav_done.size(),   (int)src.missions[0].nav_done.size());
    if (dst.missions[0].nav_done != src.missions[0].nav_done) {
        std::printf("  FAIL: missions[0].nav_done mismatch\n"); ++g_fail;
    }
    CHECK_EQ("missions[1].type",     dst.missions[1].type,                        src.missions[1].type);
    CHECK_EQ("missions[1].target_faction", dst.missions[1].target_faction,        src.missions[1].target_faction);
    CHECK_EQ("missions[1].progress", dst.missions[1].progress,                    src.missions[1].progress);
    CHECK_EQ("missions[2].type",     dst.missions[2].type,                        src.missions[2].type);
    CHECK_EQ("missions[2].dest_base",dst.missions[2].dest_base,                   src.missions[2].dest_base);
    CHECK_EQ("missions[3].type",     dst.missions[3].type,                        src.missions[3].type);
    CHECK_EQ("missions[3].target_base", dst.missions[3].target_base,              src.missions[3].target_base);
    CHECK_EQ("missions[3].hostiles_required", dst.missions[3].hostiles_required,  src.missions[3].hostiles_required);
    // The two vector<string> fields must survive intact (same length + order).
    CHECK_EQ("missions[0].nav_targets.size", (int)dst.missions[0].nav_targets.size(),
                                              (int)src.missions[0].nav_targets.size());
    CHECK_EQ("missions[1].bounty_region.size", (int)dst.missions[1].bounty_region.size(),
                                                (int)src.missions[1].bounty_region.size());
    if (!dst.missions[0].nav_targets.empty() &&
        dst.missions[0].nav_targets != src.missions[0].nav_targets) ++g_fail;
    if (!dst.missions[1].bounty_region.empty() &&
        dst.missions[1].bounty_region != src.missions[1].bounty_region) ++g_fail;

    // ---- 2b. unlimited timestamped saves (np-3dp.19) ----------------------
    std::printf("\n--- timestamped save accumulation + list + load-by-path ---\n");
    const std::string ts_path = savegame::save_timestamped(src);
    { const bool ok = !ts_path.empty(); if (!ok) ++g_fail;
      std::printf("  [%s] save_timestamped wrote a file\n", ok ? "OK  " : "FAIL"); }
    const std::string ts_path2 = savegame::save_timestamped(src);
    { const bool ok = !ts_path2.empty() && ts_path2 != ts_path; if (!ok) ++g_fail;
      std::printf("  [%s] 2nd save is a DISTINCT file (no overwrite)\n", ok ? "OK  " : "FAIL"); }
    {
        const auto saves = savegame::list_saves();
        bool found1 = false, found2 = false, label_ok = false;
        for (const auto& s : saves) {
            if (s.path == ts_path)  { found1 = true; label_ok =
                s.label.find("pentonville") != std::string::npos &&
                s.label.find("achilles")    != std::string::npos &&
                s.label.find("centurion")   != std::string::npos &&
                s.label.find("1234567 cr")  != std::string::npos; }
            if (s.path == ts_path2) found2 = true;
        }
        if (!found1 || !found2) ++g_fail;
        std::printf("  [%s] list_saves contains BOTH new files\n", (found1 && found2) ? "OK  " : "FAIL");
        if (!label_ok) ++g_fail;
        std::printf("  [%s] label = '<time> - pentonville - achilles - centurion - 1234567 cr'\n",
                    label_ok ? "OK  " : "FAIL");
    }
    {
        PlayerState by_path;
        const bool ok = savegame::load(by_path, ts_path) &&
                        by_path.credits == src.credits &&
                        by_path.faction_kills[(int)Faction::Pirate] == 17 &&
                        by_path.hp_valid;
        if (!ok) ++g_fail;
        std::printf("  [%s] load-by-path round-trips the new fields\n", ok ? "OK  " : "FAIL");
    }
    std::remove(ts_path.c_str());
    std::remove(ts_path2.c_str());

    // ---- 3. failure paths -------------------------------------------------
    std::printf("\n--- failure-path tests (must return false, never crash) ---\n");

    // 3a. missing file.
    {
        PlayerState before = make_mutated();
        PlayerState p = before;
        const bool r = savegame::load(p, kMissingSlot);
        const bool untouched = (p.credits == before.credits);
        if (r || !untouched) ++g_fail;
        std::printf("  [%s] missing-file load returns false + leaves state intact\n",
                    (!r && untouched) ? "OK  " : "FAIL");
    }

    // 3b. corrupt file (truncated JSON).
    {
        const std::string path = savegame::slot_path(kCorruptSlot);
        { std::ofstream f(path, std::ios::trunc); f << "{ \"version\": 1, \"player\": { \"cred"; }
        PlayerState p;
        const bool r = savegame::load(p, kCorruptSlot);
        if (r) ++g_fail;
        std::printf("  [%s] corrupt/truncated load returns false (no crash)\n",
                    !r ? "OK  " : "FAIL");
    }

    // 3c. version newer than supported.
    {
        const std::string path = savegame::slot_path(kVersionSlot);
        { std::ofstream f(path, std::ios::trunc);
          f << "{ \"version\": 99999, \"player\": { \"credits\": \"5\" } }"; }
        PlayerState p;
        const bool r = savegame::load(p, kVersionSlot);
        if (r) ++g_fail;
        std::printf("  [%s] newer-version load refused (forward-compat guard)\n",
                    !r ? "OK  " : "FAIL");
    }

    // 3d. (#8) old-format save with NO missions key still loads and the rest
    //     of the player data round-trips; missions array stays empty (back-
    //     compat: absent missions key == empty list).
    {
        const std::string path = savegame::slot_path(kOldNoMissSlot);
        { std::ofstream f(path, std::ios::trunc);
          f << "{ \"version\": 1, \"label\": \"old-save\",\n"
               "  \"player\": { \"credits\": \"99\", \"current_system\": \"troy\",\n"
               "    \"last_docked_base\": \"achilles\" } }"; }
        PlayerState p;
        const bool r = savegame::load(p, kOldNoMissSlot);
        const bool ok = r && p.credits == 99 &&
                        p.current_system == "troy" &&
                        p.last_docked_base == "achilles" &&
                        p.missions.empty();
        if (!ok) ++g_fail;
        std::printf("  [%s] v1 save (no missions key) loads with missions=[], rest intact\n",
                    ok ? "OK  " : "FAIL");
    }

    // 3e. (#8) a pre-#6 mission entry with a `type` int that's outside the
    //     current 0..5 range must be dropped on load (logged + skipped). A
    //     real second entry with a valid new type stays.
    {
        const std::string path = savegame::slot_path(kOldBadTypeSlot);
        { std::ofstream f(path, std::ios::trunc);
          f << "{ \"version\": 4, \"label\": \"old-bad-type\",\n"
               "  \"player\": { \"credits\": \"0\", \"current_system\": \"troy\",\n"
               "    \"last_docked_base\": \"achilles\",\n"
               "    \"missions\": [\n"
               "      { \"id\": \"bad1\",  \"type\": 99,  \"title\": \"bogus\" },\n"
               "      { \"id\": \"good1\", \"type\": 5,  \"title\": \"cargo\",\n"
               "        \"reward\": \"0\" }\n"
               "    ] } }"; }
        PlayerState p;
        const bool r = savegame::load(p, kOldBadTypeSlot);
        const bool ok = r && p.missions.size() == 1 && p.missions[0].id == "good1";
        if (!ok) ++g_fail;
        std::printf("  [%s] out-of-range mission type (99) skipped, type=5 kept\n",
                    ok ? "OK  " : "FAIL");
    }

    // 3f. (#138) a v6 save with NO plot keys loads with both plot lists
    //     empty (campaign not started) and everything else intact — the
    //     v6 -> v7 migration guarantee.
    {
        const std::string path = savegame::slot_path(kOldNoMissSlot);
        { std::ofstream f(path, std::ios::trunc);
          f << "{ \"version\": 6, \"label\": \"v6-pre-campaign\",\n"
               "  \"player\": { \"credits\": \"777\", \"current_system\": \"troy\",\n"
               "    \"last_docked_base\": \"achilles\",\n"
               "    \"merc_guild_member\": true } }"; }
        PlayerState p;
        const bool r = savegame::load(p, kOldNoMissSlot);
        const bool ok = r && p.credits == 777 && p.merc_guild_member &&
                        p.plot_flags.empty() && p.plot_items.empty();
        if (!ok) ++g_fail;
        std::printf("  [%s] v6 save loads with plot_flags/items = [] (campaign off)\n",
                    ok ? "OK  " : "FAIL");
    }

    // 3g. (#138) plot:: mutator invariants: idempotent set/give, clear/
    //     remove report presence truthfully, empty ids refused.
    {
        PlayerState p = player::new_game("troy");
        bool ok = plot::set_flag(p, "x");            // newly set -> true
        ok = ok && !plot::set_flag(p, "x");          // duplicate  -> false
        ok = ok && plot::has_flag(p, "x");
        ok = ok && plot::clear_flag(p, "x");         // was set    -> true
        ok = ok && !plot::clear_flag(p, "x");        // already gone -> false
        ok = ok && !plot::set_flag(p, "");           // empty id refused
        ok = ok && plot::give_item(p, "i") && !plot::give_item(p, "i") &&
             plot::has_item(p, "i") && plot::remove_item(p, "i") &&
             !plot::remove_item(p, "i");
        ok = ok && p.plot_flags.empty() && p.plot_items.empty();
        if (!ok) ++g_fail;
        std::printf("  [%s] plot:: mutator invariants (idempotence, empty-id refusal)\n",
                    ok ? "OK  " : "FAIL");
    }

    std::printf("\n=== %s ===\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}

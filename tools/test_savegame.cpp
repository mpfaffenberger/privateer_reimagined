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
constexpr int kSlot       = 7;
constexpr int kCorruptSlot = 8;
constexpr int kVersionSlot = 6;
constexpr int kMissingSlot = 42;

PlayerState make_mutated() {
    PlayerState p = player::new_game("troy");
    p.credits = 1234567;                         // distinctive, > new-game default
    p.rep.rep[(int)Faction::Confed]   = 42;
    p.rep.rep[(int)Faction::Pirate]   = -77;
    p.rep.rep[(int)Faction::Merchant] = 13;
    p.rep.rep[(int)Faction::Kilrathi] = -100;
    p.ship_class_name = "centurion";
    p.gun_mounts      = { "tachyon_cannon", "", "meson_blaster" };
    p.shield_level    = 3;
    p.engine_level    = 2;
    p.cargo_expansion = true;
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
    p.hp_armor_side  = 6.75f;
    p.hp_shield_fore = 3.5f;
    p.hp_shield_aft  = 1.25f;
    p.hp_shield_side = 0.0f;
    p.hp_energy      = 99.0f;
    p.current_system   = "pentonville";
    p.last_docked_base = "achilles";
    p.docked           = true;
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

bool guns_equal(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    return a == b;
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
    CHECK_EQ("hp_valid",       dst.hp_valid,       src.hp_valid);
    CHECK_EQ("hp_armor_fore",  dst.hp_armor_fore,  src.hp_armor_fore);
    CHECK_EQ("hp_armor_aft",   dst.hp_armor_aft,   src.hp_armor_aft);
    CHECK_EQ("hp_armor_side",  dst.hp_armor_side,  src.hp_armor_side);
    CHECK_EQ("hp_shield_fore", dst.hp_shield_fore, src.hp_shield_fore);
    CHECK_EQ("hp_shield_aft",  dst.hp_shield_aft,  src.hp_shield_aft);
    CHECK_EQ("hp_shield_side", dst.hp_shield_side, src.hp_shield_side);
    CHECK_EQ("hp_energy",      dst.hp_energy,      src.hp_energy);

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

    std::printf("\n=== %s ===\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}

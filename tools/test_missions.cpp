// -----------------------------------------------------------------------------
// tools/test_missions.cpp — offline driver for np-zte.1 mission computer:
// generation + accept/deliver + bounty progress + save/load round-trip.
//
// Links the REAL missions.cpp (built -DMISSIONS_HEADLESS so the ImGui board
// is excluded), comm.cpp (-DCOMM_HEADLESS), faction/player/commodity/galaxy/
// system_def/json/savegame. It walks exactly the paths the live Mission
// Computer screen + the np-ma2.1 kill hook drive, since those call the same
// model functions this test does:
//
//   * generate() a deterministic board for Achilles (Troy) and print the
//     cargo + bounty offers,
//   * ACCEPT a cargo mission (cargo loaded into the hold) + prove an
//     over-capacity accept is REFUSED,
//   * "travel" to the destination + DELIVER (reward paid, cargo removed),
//   * ACCEPT a bounty + simulate qualifying kills via on_target_destroyed ->
//     progress increments -> completion auto-pays,
//   * SAVE then LOAD and confirm accepted missions survive the round-trip.
//
// Build:
//   clang++ -std=c++20 -DMISSIONS_HEADLESS -DCOMM_HEADLESS -Isrc -Ithird_party \
//       tools/test_missions.cpp src/missions.cpp src/comm.cpp src/faction.cpp \
//       src/player.cpp src/commodity.cpp src/galaxy.cpp src/system_def.cpp \
//       src/savegame.cpp src/json.cpp -o /tmp/test_missions
// -----------------------------------------------------------------------------

#include "missions.h"
#include "comm.h"
#include "commodity.h"
#include "encounters.h"
#include "faction.h"
#include "galaxy.h"
#include "player.h"
#include "savegame.h"
#include "test_sandbox.h"
#include "ship_class.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "system_def.h"

// ---- test-only forward decls (anon-namespace helpers in src/missions.cpp) ---
// These match the anonymous-namespace declarations at the bottom of the
// tuning block in missions.cpp. The anon wrap gives them internal linkage,
// but the C++ linker still resolves them as `missions::X` symbols, which we
// re-declare here so the test can call them without polluting the public
// missions.h API. (hops_between IS public in missions.h now; no need to
// forward-declare it here.)
namespace missions {
struct NavRef { std::string name; HMM_Vec3 pos; };
struct DestBase {
    std::string system_id;
    std::string system_name;
    std::string base_id;
    std::string base_name;
    int         jumps = 0;
};
std::vector<NavRef> nonbase_navs(const galaxy::Galaxy& g, const std::string& system_id);
std::vector<DestBase> bases_in_system(const galaxy::Galaxy& g,
                                      const std::string& system_id,
                                      const std::string& except_base);
std::vector<std::string> bounty_region(const galaxy::Galaxy& g,
                                       const std::string& system_id,
                                       std::mt19937_64& rng);
int64_t compute_reward(MissionType type, MissionSource src, int hops,
                       int64_t cargo_value, std::mt19937_64& rng);
// #10 slice 2a: pure token-substitution helper + its value bag. Anon in
// missions.cpp; re-declared here so the test can exercise it directly.
struct TokenValues {
    std::string py, en, cl, cg, dn, dn1, dn2, nn, db, ds, ds1, ds2,
                d_o, d1, d2, bn, sb, ss;
};
std::string substitute(std::string_view tmpl, const TokenValues& v);
} // namespace missions


static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Most-recent comm-feed line ("" when empty). The feed is capped at a handful
// of lines, so size() plateaus — to prove a completion/refusal pushed a
// player-visible line we capture this just before the call and assert it
// CHANGED afterward.
static std::string last_comm() {
    const auto& f = comm::feed();
    return f.empty() ? std::string() : f.back().text;
}


int main() {
    test_sandbox::isolate_saves("missions");   // slot-7 round trip (#383)
    faction::init();
    comm::load("assets/data/comm_lines.json");
    commodity::load("assets/data/privateer_db/cargo.toml");

    galaxy::Galaxy gal;
    if (!galaxy::load("assets/galaxy.json", gal)) {
        std::printf("FAIL: could not load galaxy\n");
        return 1;
    }

    // A Tarsus-sized hold (100 units), fresh-start bankroll.
    ShipClass tarsus; tarsus.cargo_units = 100;
    PlayerState p = player::new_game("troy");
    p.last_docked_base = "achilles";
    // The cross-system flows below (cargo to a neighbour, bounty hunt region
    // beyond Troy) require a jump drive now (#12 out-of-system gate). Fit one
    // on the main test player; the dedicated refusal case uses a fresh
    // no-jump-drive player on purpose.
    p.has_jump_drive = true;
    const int cap = player::cargo_capacity(p, &tarsus);

    // ---- 1. generation -------------------------------------------------------
    std::printf("\n=================================================================\n");
    std::printf("1. GENERATE board for Achilles (Troy)\n");
    std::printf("=================================================================\n");
    // The board is a random MIX of the six types now (#10 2b), so a single
    // seed needn't contain BOTH a cargo and a bounty offer. Walk a handful
    // of deterministic seeds until we land a board with one of each (the
    // accept/deliver + bounty flows below need a concrete instance of both),
    // then drive the rest of the harness off that board.
    std::vector<missions::Mission> board;
    uint64_t board_seed = 0;
    const missions::Mission* cargo  = nullptr;
    const missions::Mission* bounty = nullptr;
    for (uint64_t s = 0xABCDEF01u; s < 0xABCDEF01u + 64; ++s) {
        std::vector<missions::Mission> b = missions::generate("achilles", "troy", gal, s);
        const missions::Mission* c = nullptr;
        const missions::Mission* y = nullptr;
        for (const missions::Mission& m : b) {
            if (!c && m.type == missions::MissionType::CargoDelivery) c = &m;
            if (!y && m.type == missions::MissionType::Bounty)        y = &m;
        }
        if (c && y) { board = std::move(b); board_seed = s; break; }
    }
    std::printf("  using seed 0x%llX\n", (unsigned long long)board_seed);
    for (const missions::Mission& m : board) {
        std::printf("  [%-6s] %-44.44s reward %lld\n",
                    missions::type_label(m.type),
                    m.title.c_str(), (long long)m.reward);
        if (m.type == missions::MissionType::CargoDelivery)
            std::printf("            -> %d %s to %s in %s\n", m.units,
                        m.commodity_id.c_str(), m.dest_base.c_str(),
                        m.dest_system.c_str());
    }
    check(!board.empty(), "board generated some missions");

    // Find the first cargo + first bounty offer in the chosen board.
    for (const missions::Mission& m : board) {
        if (!cargo  && m.type == missions::MissionType::CargoDelivery) cargo  = &m;
        if (!bounty && m.type == missions::MissionType::Bounty)        bounty = &m;
    }
    check(cargo  != nullptr, "board contains a cargo-delivery mission");
    check(bounty != nullptr, "board contains a bounty mission");
    if (!cargo || !bounty) return 1;

    // ---- 2. accept -> simulate progress -> complete, ALL SIX types ----------
    std::printf("\n=================================================================\n");
    std::printf("2. DRIVE ALL SIX TYPES: accept -> progress -> complete\n");
    std::printf("=================================================================\n");
    // The board is a random mix, so sweep deterministic Computer boards until
    // we've captured one concrete Mission of every type.
    std::vector<missions::Mission> sample(6);
    bool have[6] = { false, false, false, false, false, false };
    int  have_n  = 0;
    for (uint64_t s = 0; s < 400 && have_n < 6; ++s)
        for (const missions::Mission& m : missions::generate("achilles", "troy", gal, s)) {
            const int ti = (int)m.type;
            if (ti >= 0 && ti < 6 && !have[ti]) { sample[ti] = m; have[ti] = true; ++have_n; }
        }
    check(have_n == 6, "found a concrete instance of every mission type");

    // Fresh, jump-capable, Achilles-docked player per run so credits / cargo /
    // comm state from one type can't bleed into the next.
    auto mkp = []() {
        PlayerState pp     = player::new_game("troy");
        pp.has_jump_drive  = true;
        pp.last_docked_base = "achilles";
        return pp;
    };

    // CargoDelivery: accept loads cargo -> travel -> deliver pays + drops.
    {
        PlayerState pp = mkp();
        const missions::Mission& m = sample[(int)missions::MissionType::CargoDelivery];
        const int64_t before = pp.credits; std::string fb = last_comm();
        check(missions::accept(pp, m, cap), "CARGO: accept loads consignment");
        check(player::cargo_units_used(pp) == m.units && pp.missions.size() == 1,
              "CARGO: consignment in hold, mission tracked");
        pp.current_system = m.dest_system; pp.last_docked_base = m.dest_base;
        check(missions::complete_delivery(pp, m.id, m.dest_base), "CARGO: delivery completes");
        check(pp.missions.empty(), "CARGO: mission removed");
        check(pp.credits == before + m.reward, "CARGO: credits += reward");
        check(last_comm() != fb, "CARGO: comm line pushed");
    }

    // Campaign cargo is surfaced through ActiveMission for HUD/nav routing,
    // but campaign.cpp exclusively owns its failure and completion rules.
    {
        PlayerState pp = mkp();
        ActiveMission story;
        story.id = "m01";
        story.type = (int)missions::MissionType::CargoDelivery;
        story.source = (int)missions::MissionSource::Fixer;
        story.giver_faction = "Fixer";
        story.commodity_id = "iron";
        story.units = 40;
        story.dest_base = "liverpool";
        player::add_cargo(pp, "iron", 40, 0, cap);
        pp.missions.push_back(story);
        missions::fail_incomplete_on_dock(pp, "achilles");
        check(pp.missions.size() == 1 && player::cargo_units_used(pp) == 40,
              "FIXER CARGO: generic dock failure leaves campaign mission alone");
    }

    // #162 fail-on-dock, ALL types: docking forfeits any contract whose
    // objective isn't met yet; met objectives survive for the tracker.
    {
        // Generic cargo at the WRONG base: jettisoned + dropped.
        PlayerState pp = mkp();
        const missions::Mission& m = sample[(int)missions::MissionType::CargoDelivery];
        check(missions::accept(pp, m, cap), "DOCKFAIL: cargo accept");
        missions::fail_incomplete_on_dock(pp, "some_other_base");
        check(pp.missions.empty() && player::cargo_units_used(pp) == 0,
              "DOCKFAIL: off-target cargo jettisoned + dropped");
    }
    {
        // Incomplete patrol: forfeited on dock, comm line pushed.
        PlayerState pp = mkp();
        const missions::Mission& m = sample[(int)missions::MissionType::Patrol];
        std::string fb = last_comm();
        check(missions::accept(pp, m, cap), "DOCKFAIL: patrol accept");
        missions::fail_incomplete_on_dock(pp, "achilles");
        check(pp.missions.empty(), "DOCKFAIL: incomplete patrol forfeited");
        check(last_comm() != fb, "DOCKFAIL: patrol failure comm pushed");
    }
    {
        // Attack whose objective is ALREADY met: dock must NOT forfeit it —
        // the tracker settles the payout on its own schedule.
        PlayerState pp = mkp();
        const missions::Mission& m = sample[(int)missions::MissionType::Attack];
        check(missions::accept(pp, m, cap), "DOCKFAIL: attack accept");
        ActiveMission& am = pp.missions.back();
        if (am.hostiles_required <= 0) am.hostiles_required = 1;   // defensive
        am.progress = am.hostiles_required;        // hostiles cleared pre-dock
        missions::fail_incomplete_on_dock(pp, "achilles");
        check(pp.missions.size() == 1,
              "DOCKFAIL: met-objective attack survives dock");
    }
    {
        // Incomplete bounty: forfeited on dock.
        PlayerState pp = mkp();
        const missions::Mission& m = sample[(int)missions::MissionType::Bounty];
        check(missions::accept(pp, m, cap), "DOCKFAIL: bounty accept");
        missions::fail_incomplete_on_dock(pp, "achilles");
        check(pp.missions.empty(), "DOCKFAIL: incomplete bounty forfeited");
    }

    // Scout: complete when its single nav is marked reached.
    {
        PlayerState pp = mkp();
        const missions::Mission& m = sample[(int)missions::MissionType::Scout];
        const int64_t before = pp.credits; std::string fb = last_comm();
        check(missions::accept(pp, m, cap), "SCOUT: accept");
        check(!missions::complete_if_objectives_met(pp, m.id), "SCOUT: incomplete before nav reached");
        check(missions::mark_nav_reached(pp, m.id, 0), "SCOUT: mark single nav reached");
        check(missions::complete_if_objectives_met(pp, m.id), "SCOUT: completes once nav done");
        check(pp.missions.empty(), "SCOUT: mission removed");
        check(pp.credits == before + m.reward, "SCOUT: credits += reward");
        check(last_comm() != fb, "SCOUT: comm line pushed");
    }

    // Patrol: complete only when ALL navs reached.
    {
        PlayerState pp = mkp();
        const missions::Mission& m = sample[(int)missions::MissionType::Patrol];
        const int64_t before = pp.credits; std::string fb = last_comm();
        check(missions::accept(pp, m, cap), "PATROL: accept");
        const size_t n = m.nav_targets.size();
        check(n >= 1, "PATROL: has at least one nav");
        for (size_t i = 0; i + 1 < n; ++i) {
            check(missions::mark_nav_reached(pp, m.id, i), "PATROL: mark a nav reached");
            check(!missions::complete_if_objectives_met(pp, m.id),
                  "PATROL: incomplete while navs remain");
        }
        check(missions::mark_nav_reached(pp, m.id, n - 1), "PATROL: mark final nav");
        check(missions::complete_if_objectives_met(pp, m.id), "PATROL: completes when all navs done");
        check(pp.missions.empty(), "PATROL: mission removed");
        check(pp.credits == before + m.reward, "PATROL: credits += reward");
        check(last_comm() != fb, "PATROL: comm line pushed");
    }

    // Attack: progress advances PER-KILL via on_target_destroyed (the mission force
    // spawns outside the tracker's 6km nav bubble, so killing the spawned
    // hostiles — not update_clear's at-nav gate — drives completion). In-system
    // kills of the target faction increment; out-of-system or wrong faction do
    // NOT. The model does NOT settle inline: on reaching hostiles_required the
    // next complete_if_objectives_met() (what tick() calls every frame) pays +
    // drops — single completion path.
    {
        PlayerState pp = mkp();
        const missions::Mission& m = sample[(int)missions::MissionType::Attack];
        const int64_t before = pp.credits; std::string fb = last_comm();
        check(missions::accept(pp, m, cap), "ATTACK: accept");
        check(!missions::complete_if_objectives_met(pp, m.id), "ATTACK: incomplete at 0 kills");

        const Faction tgt = faction::from_name(m.target_faction);
        check(tgt != Faction::Count, "ATTACK: target faction resolves");
        const Faction wrong =
            (tgt == Faction::Pirate) ? Faction::Kilrathi : Faction::Pirate;
        // Attack is always same-system (target_system == offering system), and
        // delta_prime is 11 hops from troy — guaranteed != the target system.
        const std::string far = "delta_prime";
        check(far != m.target_system, "ATTACK: far system differs from target");
        const int need = pp.missions[0].hostiles_required;
        check(need > 0, "ATTACK: hostiles_required posted");

        // Wrong faction, in-system: no advance.
        check(missions::on_target_destroyed(pp, wrong, m.target_system) == 0 &&
              !pp.missions.empty() && pp.missions[0].progress == 0,
              "ATTACK: wrong-faction kill never advances");
        // Right faction, wrong system: no advance.
        check(missions::on_target_destroyed(pp, tgt, far) == 0 &&
              !pp.missions.empty() && pp.missions[0].progress == 0,
              "ATTACK: out-of-system kill does NOT advance");

        // In-system kills of the target faction advance progress; NOT settled
        // inline (mission stays in the active list until tick settles it).
        for (int k = 1; k <= need; ++k) {
            check(missions::on_target_destroyed(pp, tgt, m.target_system) == 1 &&
                  !pp.missions.empty() && pp.missions[0].progress == k,
                  "ATTACK: in-system kill advances progress (no inline settle)");
        }
        // A stray kill at the cap is a no-op (the < hostiles_required guard).
        check(missions::on_target_destroyed(pp, tgt, m.target_system) == 0 &&
              !pp.missions.empty() && pp.missions[0].progress == need,
              "ATTACK: kill at cap does not over-count");

        // The next tick's complete_if_objectives_met settles payout + drop.
        check(missions::complete_if_objectives_met(pp, m.id), "ATTACK: completes when hostiles cleared");
        check(pp.missions.empty(), "ATTACK: mission removed");
        check(pp.credits == before + m.reward, "ATTACK: credits += reward");
        check(last_comm() != fb, "ATTACK: comm line pushed");
    }

    // DefendBase: same per-kill path as Attack. The base under attack may be
    // cross-system (target_system can be a neighbour), so kills count only in
    // that system; wrong system / wrong faction never advance. Settled by the
    // next complete_if_objectives_met() — not inline.
    {
        PlayerState pp = mkp();
        const missions::Mission& m = sample[(int)missions::MissionType::DefendBase];
        const int64_t before = pp.credits; std::string fb = last_comm();
        check(missions::accept(pp, m, cap), "DEFEND: accept");
        check(!missions::complete_if_objectives_met(pp, m.id), "DEFEND: incomplete while base under attack");

        const Faction tgt = faction::from_name(m.target_faction);
        check(tgt != Faction::Count, "DEFEND: target faction resolves");
        const Faction wrong =
            (tgt == Faction::Pirate) ? Faction::Kilrathi : Faction::Pirate;
        const std::string far = "delta_prime";
        check(far != m.target_system, "DEFEND: far system differs from target");
        const int need = pp.missions[0].hostiles_required;
        check(need > 0, "DEFEND: hostiles_required posted");

        // Wrong faction / wrong system never advance.
        check(missions::on_target_destroyed(pp, wrong, m.target_system) == 0 &&
              !pp.missions.empty() && pp.missions[0].progress == 0,
              "DEFEND: wrong-faction kill never advances");
        check(missions::on_target_destroyed(pp, tgt, far) == 0 &&
              !pp.missions.empty() && pp.missions[0].progress == 0,
              "DEFEND: out-of-system kill does NOT advance");

        // In-system kills advance; not settled inline.
        for (int k = 1; k <= need; ++k) {
            check(missions::on_target_destroyed(pp, tgt, m.target_system) == 1 &&
                  !pp.missions.empty() && pp.missions[0].progress == k,
                  "DEFEND: in-system kill advances progress (no inline settle)");
        }
        check(missions::on_target_destroyed(pp, tgt, m.target_system) == 0 &&
              !pp.missions.empty() && pp.missions[0].progress == need,
              "DEFEND: kill at cap does not over-count");

        check(missions::complete_if_objectives_met(pp, m.id), "DEFEND: completes when base cleared");
        check(pp.missions.empty(), "DEFEND: mission removed");
        check(pp.credits == before + m.reward, "DEFEND: credits += reward");
        check(last_comm() != fb, "DEFEND: comm line pushed");
    }

    // Bounty (#15): kills only count inside the posted hunt region. Post a
    // concrete {"troy", <neighbor>} region; in-region kills advance, while
    // out-of-region + wrong-faction kills never do. Final qualifying kill
    // auto-pays + drops via the shared pay_and_drop path.
    {
        PlayerState pp = mkp();
        missions::Mission m = sample[(int)missions::MissionType::Bounty];
        // Posted region: troy + its real neighbors ([0] == "troy", size 1..3).
        std::mt19937_64 rrng(7);
        m.bounty_region = missions::bounty_region(gal, "troy", rrng);
        check(!m.bounty_region.empty() && m.bounty_region[0] == "troy",
              "BOUNTY: region posted as troy(+neighbor)");
        // delta_prime is 11 hops from troy (see 7e) — guaranteed out of region.
        const std::string far = "delta_prime";
        bool far_in = false;
        for (const std::string& s : m.bounty_region) if (s == far) far_in = true;
        check(!far_in, "BOUNTY: 'delta_prime' is outside the hunt region");

        const int64_t before = pp.credits; std::string fb = last_comm();
        check(missions::accept(pp, m, cap), "BOUNTY: accept");
        const Faction tgt = faction::from_name(m.target_faction);
        check(tgt != Faction::Count, "BOUNTY: target faction resolves");
        const Faction wrong =
            (tgt == Faction::Pirate) ? Faction::Kilrathi : Faction::Pirate;
        const int need = m.count_required;

        // Wrong faction never advances, even standing in-region.
        check(missions::on_target_destroyed(pp, wrong, "troy") == 0 &&
              !pp.missions.empty() && pp.missions[0].progress == 0,
              "BOUNTY: wrong-faction kill never advances");
        // Right faction, wrong system: no progress.
        check(missions::on_target_destroyed(pp, tgt, far) == 0 &&
              !pp.missions.empty() && pp.missions[0].progress == 0,
              "BOUNTY: out-of-region kill does NOT advance");

        // In-region ("troy") kills advance; the final one auto-pays + drops.
        for (int k = 1; k <= need; ++k) {
            const int adv = missions::on_target_destroyed(pp, tgt, "troy");
            if (k < need)
                check(adv == 1 && !pp.missions.empty() && pp.missions[0].progress == k,
                      "BOUNTY: in-region kill advances progress");
            else
                check(adv == 1 && pp.missions.empty(),
                      "BOUNTY: final in-region kill completes + drops");
        }
        check(pp.credits == before + m.reward, "BOUNTY: credits += reward");
        check(last_comm() != fb, "BOUNTY: comm line pushed");
    }

    // ---- 3. accept refusals: over-capacity cargo + out-of-system no jump ----
    std::printf("\n=================================================================\n");
    std::printf("3. ACCEPT REFUSALS: over-capacity cargo + out-of-system (no jump)\n");
    std::printf("=================================================================\n");
    {
        // A hold one unit too small can't take the consignment.
        const missions::Mission& m = sample[(int)missions::MissionType::CargoDelivery];
        ShipClass tiny; tiny.cargo_units = m.units - 1;
        PlayerState pp = mkp();
        const int tiny_cap = player::cargo_capacity(pp, &tiny);
        check(missions::accept_block(pp, m, tiny_cap) ==
                  missions::AcceptBlock::CargoSpace,
              "REFUSE: model reports CargoSpace blocker");
        check(!missions::can_accept(pp, m, tiny_cap), "REFUSE: can_accept=false when hold too small");
        check(!missions::accept(pp, m, tiny_cap), "REFUSE: accept refused over-capacity");
        check(pp.missions.empty(), "REFUSE: refused accept tracks nothing");
    }
    {
        // Out-of-system without a jump drive. MerchantsGuild deliveries are
        // almost always cross-system, so grab one bound out of Troy.
        missions::Mission cross; bool found = false;
        for (uint64_t s = 0; s < 200 && !found; ++s)
            for (const missions::Mission& mm :
                 missions::generate("achilles", "troy", gal, s,
                                    missions::MissionSource::MerchantsGuild))
                if (mm.type == missions::MissionType::CargoDelivery &&
                    mm.dest_system != "troy") { cross = mm; found = true; break; }
        check(found, "REFUSE: found a cross-system cargo job");
        PlayerState nojump = player::new_game("troy");   // has_jump_drive = false
        const std::string fb = last_comm();
        check(missions::accept_block(nojump, cross, cap) ==
                  missions::AcceptBlock::JumpDrive,
              "REFUSE: model reports JumpDrive blocker");
        check(!missions::can_accept(nojump, cross, cap),
              "REFUSE: can_accept=false out-of-system without jump drive");
        check(!missions::accept(nojump, cross, cap),
              "REFUSE: out-of-system accept refused without jump drive");
        check(nojump.missions.empty(), "REFUSE: out-of-system tracks nothing");
        check(last_comm() != fb, "REFUSE: out-of-system pushes a comm line");
        // The very same job IS acceptable once a jump drive is fitted.
        nojump.has_jump_drive = true;
        check(missions::can_accept(nojump, cross, cap),
              "REFUSE: same job accepted once jump-capable");
    }

    // ---- 5. save / load round-trip of accepted missions ----------------------
    std::printf("\n=================================================================\n");
    std::printf("5. SAVE / LOAD round-trip of accepted missions\n");
    std::printf("=================================================================\n");
    // Re-accept both kinds so there's a non-trivial mission list to persist.
    p.missions.clear();
    missions::accept(p, *cargo, cap);     // a cargo delivery (in progress)
    missions::accept(p, *bounty, cap);    // a bounty
    p.missions[1].progress = 1;           // partial bounty progress to round-trip
    const size_t before_n   = p.missions.size();
    const std::string c_id   = p.missions[0].id;
    const std::string b_id   = p.missions[1].id;
    const int          b_prog= p.missions[1].progress;
    const int64_t      b_rew = p.missions[1].reward;

    constexpr int kSlot = 7;
    check(savegame::save(p, kSlot), "save wrote the slot");

    PlayerState loaded;
    check(savegame::load(loaded, kSlot), "load read the slot");
    check(loaded.missions.size() == before_n, "mission count survives round-trip");
    bool match = loaded.missions.size() == before_n &&
                 loaded.missions[0].id == c_id &&
                 loaded.missions[1].id == b_id &&
                 loaded.missions[1].progress == b_prog &&
                 loaded.missions[1].reward == b_rew;
    check(match, "mission ids / progress / reward all match after load");
    for (const ActiveMission& m : loaded.missions)
        std::printf("    loaded: %s type=%d progress=%d reward=%lld\n",
                    m.id.c_str(), m.type, m.progress, (long long)m.reward);

    // ---- 6. #11 reward model — band + inequality assertions ---------------
    std::printf("\n=================================================================\n");
    std::printf("6. REWARD MODEL (#11): band + inequality assertions\n");
    std::printf("=================================================================\n");

    // Sweep seeds and return (min, max) over the jitter window. n=400 covers
    // the band comfortably — jitter is the only rng consumer in compute_reward.
    constexpr int k_n_seeds = 400;
    auto sweep_minmax = [](missions::MissionType t, missions::MissionSource s,
                           int hops, int64_t cargo_value = 0) {
        int64_t lo = std::numeric_limits<int64_t>::max();
        int64_t hi = std::numeric_limits<int64_t>::min();
        for (uint64_t seed = 0; seed < (uint64_t)k_n_seeds; ++seed) {
            std::mt19937_64 rng(seed + 1);   // +1 keeps seed=0 deterministic
            int64_t r = missions::compute_reward(t, s, hops, cargo_value, rng);
            if (r < lo) lo = r;
            if (r > hi) hi = r;
        }
        return std::pair<int64_t, int64_t>(lo, hi);
    };

    // 6a. Patrol, Computer, h=0 → reward ∈ [4500, 10500]. The Mission
    // Computer source now shaves a random 1750–2500 cr off every offer, so
    // the band drops from [7000, 12000] to [4500, 10500].
    {
        auto [lo, hi] = sweep_minmax(missions::MissionType::Patrol,
                                      missions::MissionSource::Computer, 0);
        std::printf("  Patrol/Computer/h=0  min=%lld  max=%lld\n",
                    (long long)lo, (long long)hi);
        check(lo >= 4500 && hi <= 10500,
              "Patrol/Computer/h=0 ∈ [4500, 10500]");
    }

    // 6b. Patrol, h=4 — per-source band. h=4 with our 2750 cr/hop model is
    // the band the spec was calibrated against; Computer now shaves a random
    // 1750–2500 cr off the top, so it sits a bit below the others.
    {
        auto [lo, hi] = sweep_minmax(missions::MissionType::Patrol,
                                     missions::MissionSource::Computer, 4);
        std::printf("  Patrol/h=4 Computer    min=%lld  max=%lld\n",
                    (long long)lo, (long long)hi);
        check(lo >= 15000 && hi <= 20000,
              "Patrol/h=4 Computer ∈ [15000, 20000]");
    }
    for (missions::MissionSource s : { missions::MissionSource::MercenariesGuild,
                                       missions::MissionSource::MerchantsGuild }) {
        auto [lo, hi] = sweep_minmax(missions::MissionType::Patrol, s, 4);
        std::printf("  Patrol/h=4 src=%d       min=%lld  max=%lld\n",
                    (int)s, (long long)lo, (long long)hi);
        check(lo >= 18000 && hi <= 23000,
              "Patrol/h=4 (Merc/Merchant) ∈ [18000, 23000]");
    }

    // 6c. Bounty(h=0) >= Patrol(h=0) typical same source. "Typical" is the
    // median roll — sweep seeds, sort, and pick the middle. By construction
    // the Bounty band is +10% over Patrol at every source, so this holds.
    {
        auto median = [](missions::MissionType t, missions::MissionSource s, int hops) {
            std::vector<int64_t> v;
            v.reserve(k_n_seeds);
            for (uint64_t seed = 0; seed < (uint64_t)k_n_seeds; ++seed) {
                std::mt19937_64 rng(seed + 1);
                v.push_back(missions::compute_reward(t, s, hops, 0, rng));
            }
            std::sort(v.begin(), v.end());
            return v[k_n_seeds / 2];
        };
        for (missions::MissionSource s : { missions::MissionSource::Computer,
                                           missions::MissionSource::MercenariesGuild,
                                           missions::MissionSource::MerchantsGuild }) {
            int64_t p_med = median(missions::MissionType::Patrol, s, 0);
            int64_t b_med = median(missions::MissionType::Bounty, s, 0);
            std::printf("  src=%d  Patrol h=0 med=%lld  Bounty h=0 med=%lld\n",
                        (int)s, (long long)p_med, (long long)b_med);
            check(b_med >= p_med,
                  "Bounty(h=0) >= Patrol(h=0) typical (median) same source");
        }
    }

    // 6d. MercenariesGuild >= Computer for equal inputs. Merc (1.10x) always
    // beats Computer (0.85x) on the same jittered base, so the WORST Merc
    // roll still beats the BEST Computer roll — test that strictly.
    {
        auto [comp_min, comp_max] = sweep_minmax(missions::MissionType::Patrol,
                                                 missions::MissionSource::Computer, 0);
        auto [merc_min, merc_max] = sweep_minmax(missions::MissionType::Patrol,
                                                 missions::MissionSource::MercenariesGuild, 0);
        std::printf("  Patrol h=0  Comp[%lld..%lld]  Merc[%lld..%lld]\n",
                    (long long)comp_min, (long long)comp_max,
                    (long long)merc_min, (long long)merc_max);
        check(merc_min >= comp_max,
              "Merc >= Computer for equal inputs (worst Merc >= best Computer)");
    }

    // ---- 7. #9 nav helpers -------------------------------------------------
    std::printf("\n=================================================================\n");
    std::printf("7. NAV HELPERS (#9): graph + non-base nav assertions\n");
    std::printf("=================================================================\n");

    // 7a. nonbase_navs("troy") non-empty. The real galaxy has plenty of navs
    // in Troy (Pyrenees Jump, Pender's Jump, Regallis Jump, War Jump, plus
    // non-jump navs); at minimum the function should return SOMETHING.
    {
        auto nbs = missions::nonbase_navs(gal, "troy");
        std::printf("  nonbase_navs(troy):  %zu entries\n", nbs.size());
        check(!nbs.empty(), "nonbase_navs(troy) non-empty");
    }

    // 7b. bases_in_system(troy, "achilles") excludes achilles itself. Just a
    // sanity check that the except_base filter works.
    {
        auto bs = missions::bases_in_system(gal, "troy", "achilles");
        bool hit_ach = false;
        for (const auto& b : bs) if (b.base_id == "achilles") hit_ach = true;
        std::printf("  bases_in_system(troy, 'achilles'): %zu entries, contains_achilles=%d\n",
                    bs.size(), hit_ach ? 1 : 0);
        check(!hit_ach, "bases_in_system excludes except_base");
    }

    // 7c. hops_between(troy, troy) == 0 (trivially).
    check(missions::hops_between(gal, "troy", "troy") == 0,
          "hops_between(troy, troy) == 0");

    // 7d. hops_between(troy, neighbor) == 1 for each direct neighbor of troy.
    // Per assets/galaxy.json, Troy's outbound links are pyrenees, penders_star,
    // regallis, war (see docs/re/mission_economy.md).
    for (const char* nb : { "pyrenees", "penders_star", "regallis", "war" }) {
        int h = missions::hops_between(gal, "troy", nb);
        std::printf("  hops_between(troy, %s) = %d\n", nb, h);
        check(h == 1, "hops_between(troy, neighbor) == 1");
    }

    // 7e. hops_between(troy, delta_prime) == 11. delta_prime is the system's
    // farthest system at 11 hops from troy (the diameter of the graph).
    {
        int h = missions::hops_between(gal, "troy", "delta_prime");
        std::printf("  hops_between(troy, delta_prime) = %d (expect 11)\n", h);
        check(h == 11, "hops_between(troy, delta_prime) == 11");
    }

    // 7f. bounty_region(troy) is 1..3 systems, first == "troy" (the input).
    {
        std::mt19937_64 rng(42);
        auto r = missions::bounty_region(gal, "troy", rng);
        std::printf("  bounty_region(troy):  %zu entries\n", r.size());
        check(!r.empty() && r.size() <= 3, "bounty_region size 1..3");
        check(!r.empty() && r[0] == "troy",
              "bounty_region[0] is the input system ('troy')");
    }

    // ---- 8. #10 slice 2a: token substitution -------------------------------
    std::printf("\n=================================================================\n");
    std::printf("8. TOKEN SUBSTITUTION (#10 2a): substitute() longest-match\n");
    std::printf("=================================================================\n");
    {
        missions::TokenValues v;
        v.cl = "Tara Coraline";  v.en = "Kilrathi";  v.py = "12500";
        v.ds = "Troy";          v.db = "Achilles";
        v.dn = "Alpha";  v.dn1 = "Beta";  v.dn2 = "Gamma";  v.nn = "3";

        const std::string_view tmpl =
            "$CL needs a patrol of $NN navs ($DN1, $DN2, then $DN) in the $DS "
            "system, base $DB, to repel $EN raiders for $PY credits.";
        const std::string got = missions::substitute(tmpl, v);
        std::printf("  => %s\n", got.c_str());

        const std::string want =
            "Tara Coraline needs a patrol of 3 navs (Beta, Gamma, then Alpha) "
            "in the Troy system, base Achilles, to repel Kilrathi raiders for "
            "12500 credits.";
        check(got == want, "substitute renders every token (longest-match first)");

        // $DN1/$DN2 must NOT be eaten by $DN (would leave a stray '1'/'2').
        check(got.find("Beta") != std::string::npos &&
              got.find("Gamma") != std::string::npos,
              "$DN1/$DN2 win over $DN (longest-match)");

        // No leftover $[A-Z] token sequences survive.
        check(!std::regex_search(got, std::regex("\\$[A-Z]")),
              "no leftover $[A-Z] token in output");

        // Unknown $X is left untouched (caller asserts none remain).
        const std::string unk = missions::substitute("keep $ZZ here", v);
        check(unk == "keep $ZZ here", "unknown $X left untouched");
    }

    // ---- 9. #10 slice 2b: full per-type generate() -------------------------
    std::printf("\n=================================================================\n");
    std::printf("9. GENERATE 2b: six types, source-gated, fully substituted\n");
    std::printf("=================================================================\n");
    {
        const std::regex leftover("\\$[A-Z]");

        // 9a. Determinism: same (seed, source) -> byte-identical board.
        {
            auto a = missions::generate("achilles", "troy", gal, 0x1234u);
            auto b = missions::generate("achilles", "troy", gal, 0x1234u);
            bool same = a.size() == b.size();
            for (size_t i = 0; same && i < a.size(); ++i)
                same = a[i].id == b[i].id && a[i].type == b[i].type &&
                       a[i].title == b[i].title &&
                       a[i].description == b[i].description &&
                       a[i].reward == b[i].reward;
            check(same, "same seed -> identical board");
        }

        // 9b. Sweep seeds for the Computer board: every one of the six types
        // appears at least once; every title+description is fully rendered
        // (no leftover $[A-Z]); every reward is strictly positive.
        bool seen[6] = { false, false, false, false, false, false };
        bool all_clean = true, all_pos = true;
        bool bounty_single = true;
        int  total = 0;
        for (uint64_t s = 0; s < 80; ++s) {
            auto board9 = missions::generate("achilles", "troy", gal, s);
            for (const missions::Mission& m : board9) {
                ++total;
                const int ti = (int)m.type;
                if (ti >= 0 && ti < 6) seen[ti] = true;
                if (std::regex_search(m.title, leftover) ||
                    std::regex_search(m.description, leftover)) {
                    all_clean = false;
                    std::printf("    LEFTOVER token in: %s | %s\n",
                                m.title.c_str(), m.description.c_str());
                }
                if (m.reward <= 0) all_pos = false;
                // Bounty is a single named target — never a multi-kill contract.
                if (m.type == missions::MissionType::Bounty && m.count_required != 1)
                    bounty_single = false;
            }
        }
        std::printf("    swept %d Computer missions; types seen:", total);
        for (int t = 0; t < 6; ++t)
            std::printf(" %s=%d", missions::type_label((missions::MissionType)t), seen[t]);
        std::printf("\n");
        check(seen[0] && seen[1] && seen[2] && seen[3] && seen[4] && seen[5],
              "all six types appear across seeds (Computer)");
        check(all_clean, "no leftover $[A-Z] in any title/description");
        check(all_pos,   "every reward is > 0");
        check(bounty_single, "bounty count_required is always 1 (single target)");

        // Cargo generation must use the whole reachable graph rather than
        // stopping at direct neighbours. Verify through the public generator
        // so this covers destination collection and mission construction.
        bool cargo_multi_jump = false;
        bool cargo_shortest_routes = true;
        for (uint64_t s = 0; s < 80; ++s) {
            for (const missions::Mission& m :
                 missions::generate("achilles", "troy", gal, s,
                                    missions::MissionSource::MerchantsGuild)) {
                if (m.type != missions::MissionType::CargoDelivery) continue;
                const int hops = missions::hops_between(gal, "troy", m.dest_system);
                cargo_multi_jump |= hops > 1;
                cargo_shortest_routes &= hops >= 0;
            }
        }
        check(cargo_multi_jump,
              "cargo generation includes destinations more than one jump away");
        check(cargo_shortest_routes,
              "generated cargo destinations have reachable shortest routes");

        // 9c. Source gating: MercenariesGuild never emits CargoDelivery;
        // MerchantsGuild only ever emits CargoDelivery or Bounty.
        bool merc_ok = true, merch_ok = true;
        for (uint64_t s = 0; s < 40; ++s) {
            for (const missions::Mission& m :
                 missions::generate("achilles", "troy", gal, s,
                                    missions::MissionSource::MercenariesGuild))
                if (m.type == missions::MissionType::CargoDelivery) merc_ok = false;
            for (const missions::Mission& m :
                 missions::generate("achilles", "troy", gal, s,
                                    missions::MissionSource::MerchantsGuild))
                if (m.type != missions::MissionType::CargoDelivery &&
                    m.type != missions::MissionType::Bounty) merch_ok = false;
        }
        check(merc_ok,  "MercenariesGuild never emits CargoDelivery");
        check(merch_ok, "MerchantsGuild only emits Cargo or Bounty");
    }

    // ---- 10. mission_status helper (np-19.3 / navmap mission-status panel) ----
    std::printf("\n=================================================================\n");
    std::printf("10. MISSION STATUS: per-type text + cross-system target\n");
    std::printf("=================================================================\n");
    // Needs the real StarSystem to resolve base labels. Load Troy once.
    std::optional<StarSystem> troy_opt = load_system("troy");
    check(troy_opt.has_value(), "Troy loaded for mission_status assertions");
    if (troy_opt) {
        const StarSystem& troy = *troy_opt;
        ActiveMission am_patrol{};
        am_patrol.type = (int)missions::MissionType::Patrol;
        am_patrol.target_system = "troy";
        am_patrol.nav_targets = { "Pyrenees Jump", "Pender's Jump",
                                  "Regallis Jump", "War Jump" };
        am_patrol.nav_done   = { 1, 1, 0, 0 };
        am_patrol.nav_count  = (int)am_patrol.nav_targets.size();
        {
            const missions::MissionStatus s =
                missions::mission_status(am_patrol, troy, "troy");
            check(s.text == "PATROL 2/4",       "PATROL: text = 'PATROL 2/4'");
            check(s.in_current_system,         "PATROL: in_current_system");
            check(s.target_system.empty(),     "PATROL: target_system empty when here");
        }

        ActiveMission am_scout{};
        am_scout.type = (int)missions::MissionType::Scout;
        am_scout.target_system = "troy";
        am_scout.nav_targets = { "Pyrenees Jump" };
        am_scout.nav_done = { 0 };
        {
            const missions::MissionStatus s =
                missions::mission_status(am_scout, troy, "troy");
            check(s.text == "SCOUT: reach Pyrenees Jump",
                  "SCOUT: text matches expected");
            check(s.in_current_system, "SCOUT: in_current_system");
        }

        ActiveMission am_attack{};
        am_attack.type = (int)missions::MissionType::Attack;
        am_attack.target_system = "troy";
        am_attack.nav_targets = { "Pyrenees Jump" };
        am_attack.progress = 3;
        am_attack.hostiles_required = 5;
        am_attack.count_required = 5;
        {
            const missions::MissionStatus s =
                missions::mission_status(am_attack, troy, "troy");
            check(s.text == "ATTACK 3/5",     "ATTACK: text = 'ATTACK 3/5'");
            check(s.in_current_system,        "ATTACK: in_current_system");
        }

        ActiveMission am_defend{};
        am_defend.type = (int)missions::MissionType::DefendBase;
        am_defend.target_system = "troy";
        am_defend.target_base = "achilles";   // a real base in Troy
        am_defend.hostiles_required = 4;
        am_defend.count_required = 4;
        am_defend.progress = 0;              // still under attack
        {
            const missions::MissionStatus s =
                missions::mission_status(am_defend, troy, "troy");
            // Should mention 'Achilles' (nav name) and 'hostiles' (status).
            check(s.text.find("DEFEND") == 0,        "DEFEND: starts with DEFEND");
            check(s.text.find("hostiles") != std::string::npos,
                  "DEFEND: text mentions 'hostiles' (not secure)");
            check(s.in_current_system,              "DEFEND: in_current_system");
        }

        ActiveMission am_bounty{};
        am_bounty.type = (int)missions::MissionType::Bounty;
        am_bounty.target_system = "troy";
        am_bounty.target_faction = "pirates";
        am_bounty.count_required = 3;
        am_bounty.progress = 2;
        am_bounty.bounty_region = { "troy" };
        {
            const missions::MissionStatus s =
                missions::mission_status(am_bounty, troy, "troy");
            check(s.text == "BOUNTY pirates 2/3",   "BOUNTY: text = 'BOUNTY pirates 2/3'");
            check(s.in_current_system,             "BOUNTY: in_current_system (here)");
        }

        ActiveMission am_cargo_in{};
        am_cargo_in.type = (int)missions::MissionType::CargoDelivery;
        am_cargo_in.dest_system = "troy";
        am_cargo_in.dest_base   = "achilles";
        am_cargo_in.commodity_id = "grain";
        am_cargo_in.units = 10;
        {
            const missions::MissionStatus s =
                missions::mission_status(am_cargo_in, troy, "troy");
            // In-system cargo mirrors #18 verbatim — "READY TO DELIVER".
            check(s.text == "READY TO DELIVER",    "CARGO-in: text mirrors #18");
            check(s.in_current_system,             "CARGO-in: in_current_system");
        }

        // Cross-system case: dest_system != current. Must surface 'target_system'
        // (the raw id, the panel does display-name lookup itself) and the
        // text should NOT claim it's in the current system.
        ActiveMission am_cargo_cross{};
        am_cargo_cross.type = (int)missions::MissionType::CargoDelivery;
        am_cargo_cross.dest_system = "pyrenees";   // not current
        am_cargo_cross.dest_base   = "some_other";
        am_cargo_cross.commodity_id = "grain";
        am_cargo_cross.units = 10;
        {
            const missions::MissionStatus s =
                missions::mission_status(am_cargo_cross, troy, "troy");
            check(!s.in_current_system,            "CARGO-cross: NOT in_current_system");
            check(s.target_system == "pyrenees",   "CARGO-cross: target_system=pyrenees");
            check(s.text.find("DELIVER") == 0,     "CARGO-cross: text starts with DELIVER");
            check(s.text.find("pyrenees") == std::string::npos,
                  "CARGO-cross: raw 'pyrenees' text NOT embedded");
        }
    }

    // ---- 11. mission-force per-objective triggered set (#14 unified model) ----
    std::printf("\n=================================================================\n");
    std::printf("11. MISSION FORCE: per-objective triggered-set logic\n");
    std::printf("=================================================================\n");
    // The triggered-set is unit-testable without a real system: an empty
    // StarSystem makes build_faction_fighter_table fall back to the
    // per-faction default fighter, so the SpawnFn always gets a non-empty
    // class_name and the spawn "succeeds" (returns a fake id).
    {
        StarSystem sys;                       // empty — fallback fighter path
        HMM_Vec3   player{ 0, 0, 0 };
        int        spawn_count = 0;
        encounters::SpawnFn spawn_fn =
            [&](const encounters::SpawnRequest&) -> uint32_t {
                ++spawn_count;
                return 1000 + spawn_count;     // fake non-zero id
            };

        encounters::sync_active_missions({});  // clean slate

        // 11a. Same objective_key: spawns once, second call returns 0.
        {
            encounters::MissionForce mf;
            mf.mission_id   = "test_trig_1";
            mf.objective_key = "nav_A";
            mf.anchor        = { 5000, 0, 0 };
            mf.faction       = Faction::Pirate;
            mf.count         = 2;
            const int n1 = encounters::ensure_mission_force(sys, mf, player, spawn_fn);
            check(n1 > 0, "TRIGGER: first ensure spawns");
            check(encounters::mission_objective_triggered("test_trig_1", "nav_A"),
                  "TRIGGER: objective marked triggered after spawn");
            const int n2 = encounters::ensure_mission_force(sys, mf, player, spawn_fn);
            check(n2 == 0, "TRIGGER: second ensure with same key spawns nothing");
        }

        // 11b. Different objective_key on the SAME mission: spawns independently.
        {
            encounters::MissionForce mf;
            mf.mission_id   = "test_trig_1";   // same mission, different nav
            mf.objective_key = "nav_B";
            mf.anchor        = { -5000, 0, 0 };
            mf.faction       = Faction::Pirate;
            mf.count         = 1;
            const int nb = encounters::ensure_mission_force(sys, mf, player, spawn_fn);
            check(nb > 0, "TRIGGER: different objective_key spawns independently");
        }

        // 11c. mark_mission_objective_triggered (a scout/patrol MISS) prevents
        //      a later ensure from spawning — the objective is one-time.
        {
            encounters::mark_mission_objective_triggered("test_trig_2", "nav_C");
            check(encounters::mission_objective_triggered("test_trig_2", "nav_C"),
                  "TRIGGER: mark sets triggered without spawning");
            encounters::MissionForce mf;
            mf.mission_id   = "test_trig_2";
            mf.objective_key = "nav_C";
            mf.anchor        = { 0, 5000, 0 };
            mf.faction       = Faction::Pirate;
            mf.count         = 3;
            const int nc = encounters::ensure_mission_force(sys, mf, player, spawn_fn);
            check(nc == 0, "TRIGGER: ensure after mark spawns nothing");
        }

        encounters::sync_active_missions({});  // clean up global state
    }

    std::printf("\n=================================================================\n");
    std::printf("RESULT: %s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
    std::printf("=================================================================\n");
    return g_fail == 0 ? 0 : 1;
}

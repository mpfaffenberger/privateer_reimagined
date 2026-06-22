// -----------------------------------------------------------------------------
// missions.cpp — Mission Computer: generation, accept/complete, screen body.
//
// See missions.h for the design (data-driven generation, the headless split,
// board persistence policy, and the np-ma2.1 kill-path wiring). This file is
// two halves, mirroring economy.cpp:
//
//   1. The MODEL — generate() / accept() / complete_delivery() / abandon() /
//      on_player_kill(). Pure logic: it reads the commodity catalog, the
//      galaxy graph + neighbouring system JSON (for real destination bases),
//      moves credits/cargo ONLY through player:: helpers, and pushes comm
//      lines. No ImGui, no audio — links into tools/test_missions.cpp.
//
//   2. The VIEW — the Mission Computer ImGui screen body, registered with
//      base_screens via the np-9cu.4 hook seam. Compiled out under
//      MISSIONS_HEADLESS so the offline harness needn't drag in the UI stack.
//
// ---- TUNING CONSTANTS (Privateer-plausible, all in one place) ---------------
// Cargo rewards = a flat handling fee + a fraction of the hauled goods' coarse
// nominal value + a per-jump distance bonus. Bounties = a base posting + a
// flat per-kill bounty. The "nominal value" table is a deliberately COARSE
// heuristic (NOT the live market in economy.h) so generation stays pure and
// offline-testable; it only needs to make richer cargo pay a bit more.
// -----------------------------------------------------------------------------

#include "missions.h"

#include "comm.h"
#include "commodity.h"
#include "faction.h"
#include "galaxy.h"
#include "player.h"
#include "system_def.h"

#ifndef MISSIONS_HEADLESS
#include "base_screens.h"
#include "ship_class.h"
#include "sfx.h"
#include "imgui.h"
#include "sokol_app.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// Pull in the JSON reader for the giver-faction lookup on the origin base.
#include "json.h"

namespace missions {

namespace {

// ---- tuning knobs -----------------------------------------------------------
constexpr int64_t k_cargo_base_fee      = 250;   // flat handling fee, credits
constexpr double  k_cargo_value_margin  = 0.60;  // share of cargo value paid
constexpr int64_t k_cargo_per_jump_bonus= 900;   // credits per jump of distance

constexpr int64_t k_bounty_base         = 200;   // flat posting, credits
constexpr int64_t k_bounty_per_kill     = 600;   // credits per required kill

constexpr int     k_cargo_units_min     = 8;     // plausible vs a ~100-unit hold
constexpr int     k_cargo_units_max     = 28;
constexpr int     k_bounty_count_min    = 2;
constexpr int     k_bounty_count_max    = 5;

constexpr int     k_max_cargo_missions  = 3;
constexpr int     k_max_bounty_missions = 2;

// Board refresh window: the visible board is reseeded once per this many wall-
// clock seconds (plus the base id), so it feels alive between visits but is
// stable within a sitting and never per-frame noise. 30 min.
constexpr int64_t k_board_refresh_secs  = 1800;

// Coarse per-unit "mission value" heuristic (credits/unit) — see header. NOT
// the market price; just enough that luxury/weapons jobs pay more than ore.
int nominal_unit_value(const std::string& category) {
    if (category == "MAGIC")   return 100;
    if (category == "WEAPONS") return 80;
    if (category == "LUXURY")  return 60;
    if (category == "MEDICAL") return 55;
    if (category == "FOOD")    return 25;
    if (category == "RAWMAT")  return 20;
    return 35;   // sensible default for the other categories
}

// FNV-1a over a string — small, stable, no deps. Seeds the per-base board.
uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// One reachable destination base, with its galaxy distance in jumps.
struct DestBase {
    std::string system_id;
    std::string system_name;
    std::string base_id;
    std::string base_name;   // the nav-point display name ("Helen Planet")
    int         jumps = 0;
};

// Read the origin base's display faction from its base.json (flavour for the
// mission giver). "Independent" when absent — never fatal.
std::string giver_faction_of(const std::string& base_id) {
    const json::Value root =
        json::parse_file("assets/bases/" + base_id + "/base.json");
    if (root.is_object() && root.contains("faction")) {
        const std::string f = root["faction"].string_or("");
        if (!f.empty()) return f;
    }
    return "Independent";
}

// Append every dockable base in `system_id` (loaded fresh from its JSON) to
// `out`, tagging each with `jumps`. Skips the origin base itself.
void add_bases_from_system(const galaxy::Galaxy& g, const std::string& system_id,
                           const std::string& origin_base, int jumps,
                           std::vector<DestBase>& out) {
    const std::string path = g.json_path_for(system_id);
    if (path.empty()) return;
    std::optional<StarSystem> sys = load_system(path);
    if (!sys) return;

    std::string sys_name = sys->name;
    if (const galaxy::SystemEntry* e = g.find(system_id); e && !e->display_name.empty())
        sys_name = e->display_name;

    for (const NavPointDef& nav : sys->nav_points) {
        if (!nav.dockable || nav.base_id.empty()) continue;
        if (jumps == 0 && nav.base_id == origin_base) continue;  // not to self
        out.push_back(DestBase{ system_id, sys_name, nav.base_id, nav.name, jumps });
    }
}

// Reachable destination bases: this system (0 jumps) + every neighbour (1).
std::vector<DestBase> collect_dest_bases(const galaxy::Galaxy& g,
                                         const std::string& system_id,
                                         const std::string& origin_base) {
    std::vector<DestBase> out;
    add_bases_from_system(g, system_id, origin_base, 0, out);
    for (const std::string& nb : g.neighbors(system_id))
        add_bases_from_system(g, nb, origin_base, 1, out);
    return out;
}

// Outlaw factions = those the lawful universe already distrusts (negative
// baseline-to-player). Derived, never hardcoded — same source of truth the
// reputation system reads (faction.h g_faction_baseline_to_player).
std::vector<Faction> outlaw_factions() {
    std::vector<Faction> out;
    for (int i = 0; i < kFactionCount; ++i)
        if (g_faction_baseline_to_player[i] < 0) out.push_back((Faction)i);
    return out;
}

// Convert a generated offer to its persistent accepted form (player.h).
ActiveMission to_active(const Mission& m) {
    ActiveMission a;
    a.id             = m.id;
    a.type           = (int)m.type;
    a.giver_faction  = m.giver_faction;
    a.title          = m.title;
    a.reward         = m.reward;
    a.commodity_id   = m.commodity_id;
    a.units          = m.units;
    a.dest_system    = m.dest_system;
    a.dest_base      = m.dest_base;
    a.target_faction = m.target_faction;
    a.count_required = m.count_required;
    a.progress       = 0;
    return a;
}

// ---- the cached board (transient module state; NOT saved) -------------------
std::vector<Mission> g_board;
std::string          g_board_base;

} // namespace

// ---- generation -------------------------------------------------------------

std::vector<Mission> generate(const std::string& base_id,
                              const std::string& system_id,
                              const galaxy::Galaxy& g,
                              uint64_t seed) {
    std::vector<Mission> out;
    std::mt19937_64 rng(seed);

    const std::string giver = giver_faction_of(base_id);

    // --- cargo deliveries ---
    const std::vector<DestBase>  dests = collect_dest_bases(g, system_id, base_id);
    const std::vector<Commodity>& cat  = commodity::all();
    if (!dests.empty() && !cat.empty()) {
        std::uniform_int_distribution<size_t> pick_dest(0, dests.size() - 1);
        std::uniform_int_distribution<size_t> pick_comm(0, cat.size() - 1);
        std::uniform_int_distribution<int>    pick_units(k_cargo_units_min,
                                                         k_cargo_units_max);
        for (int i = 0; i < k_max_cargo_missions; ++i) {
            const DestBase&  d = dests[pick_dest(rng)];
            const Commodity& c = cat[pick_comm(rng)];
            const int        units = pick_units(rng);

            const int64_t cargo_value = (int64_t)nominal_unit_value(c.category) * units;
            const int64_t reward =
                k_cargo_base_fee +
                (int64_t)std::llround(cargo_value * k_cargo_value_margin) +
                k_cargo_per_jump_bonus * d.jumps;

            Mission m;
            m.id           = base_id + "-c" + std::to_string(i);
            m.type         = MissionType::CargoDelivery;
            m.giver_faction= giver;
            m.reward       = reward;
            m.commodity_id = c.id;
            m.units        = units;
            m.origin_base  = base_id;
            m.dest_system  = d.system_id;
            m.dest_base    = d.base_id;
            m.dest_base_name   = d.base_name;
            m.dest_system_name = d.system_name;

            char title[160];
            std::snprintf(title, sizeof(title), "Deliver %d %s to %s",
                          units, c.label.c_str(), d.base_name.c_str());
            m.title = title;
            char desc[320];
            std::snprintf(desc, sizeof(desc),
                "%s contracts the haul of %d units of %s to %s in the %s system. "
                "Payment of %lld cr on delivery.",
                giver.c_str(), units, c.label.c_str(), d.base_name.c_str(),
                d.system_name.c_str(), (long long)reward);
            m.description = desc;
            out.push_back(std::move(m));
        }
    }

    // --- bounties ---
    const std::vector<Faction> outlaws = outlaw_factions();
    if (!outlaws.empty()) {
        std::uniform_int_distribution<size_t> pick_fac(0, outlaws.size() - 1);
        std::uniform_int_distribution<int>    pick_count(k_bounty_count_min,
                                                         k_bounty_count_max);
        for (int i = 0; i < k_max_bounty_missions; ++i) {
            const Faction f     = outlaws[pick_fac(rng)];
            const int     count = pick_count(rng);
            const char*   fname = faction::to_name(f);
            const int64_t reward = k_bounty_base + k_bounty_per_kill * count;

            Mission m;
            m.id            = base_id + "-b" + std::to_string(i);
            m.type          = MissionType::Bounty;
            m.giver_faction = giver;
            m.reward        = reward;
            m.target_faction= fname;
            m.count_required= count;

            char title[160];
            std::snprintf(title, sizeof(title), "Bounty: destroy %d %s ships",
                          count, fname);
            m.title = title;
            char desc[320];
            std::snprintf(desc, sizeof(desc),
                "%s posts a bounty on %s raiders: destroy %d of them for %lld cr. "
                "Collected automatically on the final kill.",
                giver.c_str(), fname, count, (long long)reward);
            m.description = desc;
            out.push_back(std::move(m));
        }
    }

    return out;
}

void generate_board(const std::string& base_id, const std::string& system_id,
                    const galaxy::Galaxy& g) {
    // Seed = base id hashed, XOR a slow wall-clock window — see header.
    const int64_t window = (int64_t)std::time(nullptr) / k_board_refresh_secs;
    const uint64_t seed  = fnv1a(base_id) ^
                           ((uint64_t)window * 0x9E3779B97F4A7C15ull);

    g_board      = generate(base_id, system_id, g, seed);
    g_board_base = base_id;

    std::printf("[missions] generated %zu missions for '%s' (system '%s', seed %llu)\n",
                g_board.size(), base_id.c_str(), system_id.c_str(),
                (unsigned long long)seed);
    for (const Mission& m : g_board) {
        std::printf("[missions]   %s | %s | %lld cr\n",
                    m.id.c_str(), m.title.c_str(), (long long)m.reward);
    }
}

const std::vector<Mission>& board() { return g_board; }

// ---- accept / complete ------------------------------------------------------

bool can_accept(const PlayerState& p, const Mission& m, int capacity) {
    if (m.type == MissionType::CargoDelivery)
        return player::cargo_units_used(p) + m.units <= capacity;
    return true;
}

bool accept(PlayerState& p, const Mission& m, int capacity) {
    if (!can_accept(p, m, capacity)) {
        std::printf("[missions] ACCEPT refused: %s (hold %d/%d, needs %d)\n",
                    m.title.c_str(), player::cargo_units_used(p), capacity, m.units);
        return false;
    }
    if (m.type == MissionType::CargoDelivery) {
        // Mission cargo loads at bought_at_price 0 — it isn't yours to sell at
        // a profit, it's a consignment (v1 simplification: it still occupies
        // hold space like any cargo).
        if (!player::add_cargo(p, m.commodity_id, m.units, 0, capacity)) {
            std::printf("[missions] ACCEPT failed: could not load %d %s\n",
                        m.units, m.commodity_id.c_str());
            return false;
        }
    }
    p.missions.push_back(to_active(m));

    std::printf("[missions] ACCEPT %s | %s | reward %lld | hold %d/%d\n",
                m.id.c_str(), m.title.c_str(), (long long)m.reward,
                player::cargo_units_used(p), capacity);
    comm::push("Mission accepted: " + m.title, /*taunt=*/false);
    return true;
}

bool complete_delivery(PlayerState& p, const std::string& mission_id,
                       const std::string& at_base) {
    for (size_t i = 0; i < p.missions.size(); ++i) {
        ActiveMission& am = p.missions[i];
        if (am.id != mission_id || am.type != (int)MissionType::CargoDelivery)
            continue;
        if (am.dest_base != at_base) {
            std::printf("[missions] DELIVER refused: %s due at '%s', you're at '%s'\n",
                        am.title.c_str(), am.dest_base.c_str(), at_base.c_str());
            return false;
        }
        // The consignment must still be aboard (can't deliver what you sold).
        if (!player::remove_cargo(p, am.commodity_id, am.units)) {
            std::printf("[missions] DELIVER refused: consignment %d %s missing from hold\n",
                        am.units, am.commodity_id.c_str());
            return false;
        }
        const int64_t reward = am.reward;
        const std::string title = am.title;
        player::add_credits(p, reward);
        p.missions.erase(p.missions.begin() + (long)i);

        std::printf("[missions] DELIVER complete: %s | +%lld cr | credits %lld\n",
                    title.c_str(), (long long)reward, (long long)p.credits);
        comm::push("Delivery confirmed. Credits transferred.", /*taunt=*/false);
        return true;
    }
    std::printf("[missions] DELIVER: no active delivery '%s' for base '%s'\n",
                mission_id.c_str(), at_base.c_str());
    return false;
}

bool abandon(PlayerState& p, const std::string& mission_id) {
    for (size_t i = 0; i < p.missions.size(); ++i) {
        ActiveMission& am = p.missions[i];
        if (am.id != mission_id) continue;
        if (am.type == (int)MissionType::CargoDelivery)
            player::remove_cargo(p, am.commodity_id, am.units);   // jettison
        std::printf("[missions] ABANDON %s | %s\n", am.id.c_str(), am.title.c_str());
        comm::push("Mission abandoned: " + am.title, /*taunt=*/false);
        p.missions.erase(p.missions.begin() + (long)i);
        return true;
    }
    return false;
}

// ---- bounty progress (np-ma2.1 player-kill path) ----------------------------

int on_player_kill(PlayerState& p, Faction victim) {
    const char* victim_name = faction::to_name(victim);
    int advanced = 0;

    for (size_t i = 0; i < p.missions.size(); /* manual */) {
        ActiveMission& am = p.missions[i];
        if (am.type != (int)MissionType::Bounty ||
            am.target_faction != victim_name ||
            am.progress >= am.count_required) {
            ++i;
            continue;
        }
        ++am.progress;
        ++advanced;
        std::printf("[missions] BOUNTY progress: %s %d/%d\n",
                    am.title.c_str(), am.progress, am.count_required);
        char line[128];
        std::snprintf(line, sizeof(line), "Bounty: %s %d/%d",
                      victim_name, am.progress, am.count_required);
        comm::push(line, /*taunt=*/false);

        if (am.progress >= am.count_required) {
            const int64_t reward = am.reward;
            const std::string title = am.title;
            player::add_credits(p, reward);
            std::printf("[missions] BOUNTY complete: %s | +%lld cr | credits %lld\n",
                        title.c_str(), (long long)reward, (long long)p.credits);
            char done[128];
            std::snprintf(done, sizeof(done),
                          "Bounty complete. %lld cr transferred.", (long long)reward);
            comm::push(done, /*taunt=*/false);
            p.missions.erase(p.missions.begin() + (long)i);
            continue;   // don't advance i — we removed this entry
        }
        ++i;
    }
    return advanced;
}

// ---- Mission Computer screen body -------------------------------------------
#ifndef MISSIONS_HEADLESS

namespace {

// HUD palette echoing base_screens.cpp / economy.cpp.
constexpr ImU32 kAmber = IM_COL32(255, 217,  77, 255);
constexpr ImU32 kGreen = IM_COL32(120, 230, 120, 255);
constexpr ImU32 kGrey  = IM_COL32(150, 158, 168, 255);

const char* type_label(MissionType t) {
    return (t == MissionType::CargoDelivery) ? "CARGO" : "BOUNTY";
}

void draw_missions(BaseContext& ctx) {
    PlayerState& p = *ctx.player;
    const ShipClass* klass = ship_class::find(p.ship_class_name);
    const int capacity = player::cargo_capacity(p, klass);
    const int used     = player::cargo_units_used(p);

    const float dpi = sapp_dpi_scale();
    const float sw  = (float)sapp_width()  / dpi;
    const float sh  = (float)sapp_height() / dpi;

    // Totals strip.
    ImGui::SetCursorScreenPos(ImVec2(28, 62));
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::Text("CREDITS  %lld", (long long)p.credits);
    ImGui::SameLine(260);
    ImGui::Text("CARGO  %d / %d units", used, capacity);
    ImGui::SameLine(520);
    ImGui::Text("ACTIVE  %zu", p.missions.size());
    ImGui::PopStyleColor();

    // Split the screen height between the two panes (Available + Active).
    // The vertical accounting must clear:
    //   * 92  top reservation   — totals strip (62..92) and the "AVAILABLE MISSIONS" label (at y=92).
    //   * 24  inter-pane gap    — gap between the bottom of pane #1 and the "ACTIVE MISSIONS" label at y=92+child_sz.y+24.
    //   * 18  label line        — ~18 px for "ACTIVE MISSIONS" itself.
    //   * 70  bottom reservation — BACK button at ss.h - 56 spanning 32 px, so clear with slack.
    // The old formula (sh - 92 - 70) forgot the 24+18 gap-and-label, so the
    // active child overlapped the BACK button and only its bottom sliver
    // was clickable (issue #1).
    const ImVec2 child_sz(sw - 56, (sh - 92 - 24 - 18 - 70) * 0.5f);


    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    // ---- available board ----
    ImGui::SetCursorScreenPos(ImVec2(28, 92));
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextUnformatted("AVAILABLE MISSIONS");
    ImGui::PopStyleColor();

    if (ImGui::BeginChild("##avail", child_sz, false) &&
        ImGui::BeginTable("avail_tbl", 5, tflags, child_sz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Mission", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Destination / Target", ImGuiTableColumnFlags_WidthFixed, 280.0f);
        ImGui::TableSetupColumn("Reward");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableHeadersRow();

        for (const Mission& m : board()) {
            ImGui::TableNextRow();
            ImGui::PushID(m.id.c_str());

            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.title.c_str());
            if (ImGui::IsItemHovered() && !m.description.empty())
                ImGui::SetTooltip("%s", m.description.c_str());

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
            ImGui::TextUnformatted(type_label(m.type));
            ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            if (m.type == MissionType::CargoDelivery)
                ImGui::Text("%s (%s)", m.dest_base_name.c_str(), m.dest_system_name.c_str());
            else
                ImGui::Text("%d x %s", m.count_required, m.target_faction.c_str());

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            ImGui::Text("%lld", (long long)m.reward);
            ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            const bool ok = can_accept(p, m, capacity);
            ImGui::BeginDisabled(!ok);
            if (ImGui::SmallButton("Accept")) {
                if (accept(p, m, capacity)) sfx::ui_click();
            }
            ImGui::EndDisabled();
            if (!ok && ImGui::IsItemHovered())
                ImGui::SetTooltip("Not enough cargo space");

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // ---- active missions ----
    ImGui::SetCursorScreenPos(ImVec2(28, 92 + child_sz.y + 24));
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextUnformatted("ACTIVE MISSIONS");
    ImGui::PopStyleColor();

    if (ImGui::BeginChild("##active", child_sz, false) &&
        ImGui::BeginTable("active_tbl", 4, tflags, child_sz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Mission", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Reward");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableHeadersRow();

        // Copy ids to act on after the loop — accept/deliver/abandon mutate
        // p.missions, so we never erase mid-iteration of the draw.
        std::string deliver_id, abandon_id;

        for (const ActiveMission& am : p.missions) {
            ImGui::TableNextRow();
            ImGui::PushID(am.id.c_str());

            ImGui::TableNextColumn(); ImGui::TextUnformatted(am.title.c_str());

            ImGui::TableNextColumn();
            if (am.type == (int)MissionType::Bounty)
                ImGui::Text("%d / %d kills", am.progress, am.count_required);
            else if (am.dest_base == ctx.base_id)
                ImGui::TextColored(ImVec4(0.47f, 0.90f, 0.47f, 1.0f), "ready to deliver");
            else
                ImGui::Text("-> %s", am.dest_base.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%lld", (long long)am.reward);

            ImGui::TableNextColumn();
            if (am.type == (int)MissionType::CargoDelivery && am.dest_base == ctx.base_id) {
                // `###mission_id` suffix tells Dear ImGui to use the suffix
                // as the widget ID while still showing "Deliver" to the
                // player — keeps the ID unique per row even when several
                // rows share the same label, without extra PushID bookkeeping.
                char label[80];
                std::snprintf(label, sizeof(label), "Deliver###%s", am.id.c_str());
                if (ImGui::SmallButton(label)) deliver_id = am.id;
                ImGui::SameLine();
            }
            {
                char label[80];
                std::snprintf(label, sizeof(label), "Abandon###%s", am.id.c_str());
                if (ImGui::SmallButton(label)) abandon_id = am.id;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();

        if (!deliver_id.empty()) {
            if (complete_delivery(p, deliver_id, ctx.base_id)) sfx::ui_click();
        }
        if (!abandon_id.empty()) {
            if (abandon(p, abandon_id)) sfx::ui_click();
        }
    }
    ImGui::EndChild();
}

} // namespace

void register_screen() {
    base_screens::register_screen(BaseScreen::MissionComputer, draw_missions);
}

#else  // MISSIONS_HEADLESS
void register_screen() {}   // no-op in the offline harness
#endif

} // namespace missions

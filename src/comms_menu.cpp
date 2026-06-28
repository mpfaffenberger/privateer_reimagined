// comms_menu.cpp — implementation. See header for the elevator pitch.
//
// Architectural notes:
//   * Module owns a tiny slice of transient UI state (the current step, the
//     cached destination list, the rolling comm log). Everything else flows
//     through draw()'s parameters. No upstream game state is mutated except
//     the placeholder voice playback on a line pick.
//   * The destination list is rebuilt every DestSelect frame inside draw()
//     AND cached, so select() — which runs from the key handler, a different
//     callsite than draw() — can map a 1-based pick to the same entry the
//     player is looking at without recomputing system geometry.
#include "comms_menu.h"

#include "faction.h"
#include "json.h"
#include "ship.h"
#include "ship_class.h"
#include "system_def.h"
#include "voice.h"

#include "imgui.h"

#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace comms_menu {

namespace {

// Palette — kept in lockstep with cockpit_hud's amber/white HUD theme so the
// Comms screen sings the same tune as the rest of the STATUS panel.
const ImU32 kAmber    = IM_COL32(255, 217,  77, 240);
const ImU32 kHudWhite = IM_COL32(220, 230, 235, 220);
const ImU32 kDim      = IM_COL32(150, 160, 170, 200);
const ImU32 kRed      = IM_COL32(255, 110,  90, 240);

// ---- line pools (data-driven) -------------------------------------------
// Loaded from player_comms.json. If the file is missing we fall back to a
// minimal built-in set so the screen is never empty.
std::vector<std::string> g_friendly = {
    "Greetings, this is a peaceful trader.",
    "Good hunting out there.",
    "Safe travels, friend.",
};
std::vector<std::string> g_hostile = {
    "You picked the wrong target, pal.",
    "Back off or I'll scatter you across the void!",
    "Please, I don't want any trouble!",
};

// ---- interaction state ---------------------------------------------------
enum class Step { DestSelect, LineSelect };
Step g_step = Step::DestSelect;

// One selectable destination row, cached during the DestSelect draw so
// select() (a separate callsite) can resolve a 1-based pick.
struct Dest {
    std::string label;     // shown in the menu
    bool        hostile;   // resolved stance → which line pool to offer
};
std::vector<Dest> g_dests;

// The destination the player chose in DestSelect — drives which pool
// LineSelect shows + logs.
bool g_chosen_hostile = false;

// Rolling comm log — last few lines the player has sent. Shown at the
// bottom of the panel as "You: <line>".
constexpr size_t kLogMax = 4;
std::deque<std::string> g_log;

// True iff a base/station/planet nav point is a hailable comms destination.
bool is_base_kind(const std::string& kind) {
    return kind == "station" || kind == "planet" || kind == "base";
}

void push_log(const std::string& line) {
    g_log.push_back("You: " + line);
    while (g_log.size() > kLogMax) g_log.pop_front();
}

// Rebuild the cached destination list from the live system + target. Called
// every DestSelect frame so the menu tracks the world (targets come and go,
// the player flies between systems). Bases resolve their stance from the
// Civilian baseline (nav points carry no faction of their own); the target
// ship resolves its own faction's stance toward the player.
void rebuild_dests(const StarSystem& sys, const Ship* target,
                   const PlayerReputation& rep) {
    g_dests.clear();
    for (const NavPointDef& nav : sys.nav_points) {
        if (!is_base_kind(nav.kind)) continue;
        const Stance st = faction::stance_npc_vs_player(Faction::Civilian, rep);
        g_dests.push_back({ nav.name, st == Stance::Hostile });
    }
    if (target) {
        const char* cls = target->klass ? target->klass->display_name.c_str()
                                         : "Unknown";
        const char* fac = faction::to_name(target->faction);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Target: %s / %s", cls, fac);
        const Stance st = faction::stance_npc_vs_player(target->faction, rep);
        g_dests.push_back({ buf, st == Stance::Hostile });
    }
}

} // namespace

void load(const std::string& path) {
    json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::printf("[comms_menu] could not load %s — using built-in lines\n",
                    path.c_str());
        return;
    }
    auto read_pool = [&](const char* key, std::vector<std::string>& out) {
        const json::Value* arr = root.find(key);
        if (!arr || !arr->is_array() || arr->as_array().empty()) return;
        std::vector<std::string> tmp;
        for (const json::Value& v : arr->as_array())
            if (v.is_string()) tmp.push_back(v.as_string());
        if (!tmp.empty()) out.swap(tmp);
    };
    read_pool("friendly", g_friendly);
    read_pool("hostile",  g_hostile);
    std::printf("[comms_menu] loaded %s — %zu friendly, %zu hostile lines\n",
                path.c_str(), g_friendly.size(), g_hostile.size());
}

void open() {
    g_step = Step::DestSelect;
}

void close() {
    g_step = Step::DestSelect;
}

void select(int n) {
    if (n < 1) return;
    const size_t idx = (size_t)(n - 1);

    if (g_step == Step::DestSelect) {
        if (idx >= g_dests.size()) return;
        g_chosen_hostile = g_dests[idx].hostile;
        g_step = Step::LineSelect;
        return;
    }

    // LineSelect — resolve the pick against the chosen pool.
    const std::vector<std::string>& pool =
        g_chosen_hostile ? g_hostile : g_friendly;
    if (idx >= pool.size()) return;

    push_log(pool[idx]);
    // Placeholder pilot voice — the player's bar-patron voice id stands in
    // until a dedicated comms VO bank lands. 2D radio playback (to_player).
    voice::say("PrivBarPc01", voice::Category::Greeting,
               HMM_Vec3{ 0, 0, 0 }, true);
    g_step = Step::DestSelect;
}

void draw(const StarSystem& sys, const Ship* target,
          const PlayerReputation& rep) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextUnformatted("COMMS");
    ImGui::PopStyleColor();
    ImGui::Separator();

    if (g_step == Step::DestSelect) {
        rebuild_dests(sys, target, rep);
        if (g_dests.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted("no one in range to hail");
            ImGui::PopStyleColor();
        } else {
            for (size_t i = 0; i < g_dests.size(); ++i) {
                ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
                ImGui::Text("%zu. %s", i + 1, g_dests[i].label.c_str());
                ImGui::PopStyleColor();
            }
        }
    } else {
        const std::vector<std::string>& pool =
            g_chosen_hostile ? g_hostile : g_friendly;
        ImGui::PushStyleColor(ImGuiCol_Text, g_chosen_hostile ? kRed : kAmber);
        ImGui::TextUnformatted(g_chosen_hostile ? "HOSTILE HAIL" : "FRIENDLY HAIL");
        ImGui::PopStyleColor();
        for (size_t i = 0; i < pool.size(); ++i) {
            ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
            ImGui::Text("%zu. %s", i + 1, pool[i].c_str());
            ImGui::PopStyleColor();
        }
    }

    // Footer hint + rolling comm log.
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("press 1-9");
    ImGui::PopStyleColor();
    if (!g_log.empty()) {
        for (const std::string& line : g_log) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
    }
}

} // namespace comms_menu

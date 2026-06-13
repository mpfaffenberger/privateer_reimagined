// -----------------------------------------------------------------------------
// comm.cpp — faction comm chatter + reputation HUD feed. See header.
// -----------------------------------------------------------------------------

#include "comm.h"

#include "json.h"
#include "player.h"

// draw() is the only GPU/UI-coupled entry point. Gate it (and the ImGui +
// sokol headers it needs) behind COMM_HEADLESS so the offline rep test
// driver (tools/test_reputation.cpp) can link the pure logic without
// dragging in the whole render backend — same pattern as ECONOMY_HEADLESS.
#ifndef COMM_HEADLESS
#include "imgui.h"
#include "sokol_app.h"
#endif

#include <cstdio>
#include <random>

namespace comm {

namespace {

// ---- taunt table ---------------------------------------------------------
// Lines indexed [faction][event] -> list of candidate strings. Loaded from
// assets/data/comm_lines.json (faction lowercase name -> event key ->
// array). Empty vectors are fine — pick_line returns "" for those.
struct TauntTable {
    std::vector<std::string> by_event[kFactionCount][2];  // [event 0|1]
};
TauntTable g_table;

const char* event_key(Event e) {
    return (e == Event::KillTheirEnemy) ? "kill_their_enemy"
                                        : "killed_by_player_crime";
}

// One shared RNG. Comm flavour is cosmetic, so a default seed is fine —
// reproducible across runs, no need for entropy.
std::mt19937& rng() {
    static std::mt19937 r{0xC0FFEEu};
    return r;
}

// ---- feed state ----------------------------------------------------------
// Tuning knobs for the on-screen feed. Lines live k_line_lifetime_s then
// fade out over the last k_fade_s; at most k_max_lines show at once.
constexpr float  k_line_lifetime_s = 7.0f;
constexpr float  k_fade_s          = 1.5f;
constexpr size_t k_max_lines       = 6;

std::vector<FeedLine> g_feed;

} // namespace

bool load(const std::string& path) {
    // Reset first so a reload doesn't accumulate duplicates.
    for (auto& per_faction : g_table.by_event)
        for (auto& v : per_faction) v.clear();

    json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[comm] could not parse '%s' — taunts disabled\n",
                     path.c_str());
        return false;
    }
    const json::Value* factions = root.find("factions");
    if (!factions || !factions->is_object()) {
        std::fprintf(stderr, "[comm] '%s': missing 'factions' object\n",
                     path.c_str());
        return false;
    }

    int n_lines = 0;
    for (int fi = 0; fi < kFactionCount; ++fi) {
        const char* fname = faction::to_name((Faction)fi);
        const json::Value* fnode = factions->find(fname);
        if (!fnode || !fnode->is_object()) continue;

        for (int ev = 0; ev < 2; ++ev) {
            const Event e = (ev == 0) ? Event::KillTheirEnemy
                                      : Event::KilledByPlayerCrime;
            const json::Value* arr = fnode->find(event_key(e));
            if (!arr || !arr->is_array()) continue;
            for (const json::Value& line : arr->as_array()) {
                if (!line.is_string()) continue;
                g_table.by_event[fi][ev].push_back(line.as_string());
                ++n_lines;
            }
        }
    }

    std::printf("[comm] loaded %d taunt lines from %s\n", n_lines, path.c_str());
    return true;
}

std::string pick_line(Faction f, Event e) {
    const int fi = (int)f;
    if (fi < 0 || fi >= kFactionCount) return "";
    const int ev = (e == Event::KillTheirEnemy) ? 0 : 1;
    const std::vector<std::string>& lines = g_table.by_event[fi][ev];
    if (lines.empty()) return "";
    std::uniform_int_distribution<size_t> pick(0, lines.size() - 1);
    return lines[pick(rng())];
}

void push(const std::string& text, bool taunt) {
    g_feed.push_back(FeedLine{ text, 0.0f, taunt });
    // Cap the backlog — drop the oldest. erase-front on a tiny vector is
    // cheaper than the bookkeeping a ring buffer would need.
    while (g_feed.size() > k_max_lines) g_feed.erase(g_feed.begin());
}

void tick(float dt) {
    for (FeedLine& l : g_feed) l.age_s += dt;
    // Drop fully-expired lines. Stable order preserved (oldest first).
    size_t write = 0;
    for (size_t read = 0; read < g_feed.size(); ++read) {
        if (g_feed[read].age_s < k_line_lifetime_s) {
            if (write != read) g_feed[write] = std::move(g_feed[read]);
            ++write;
        }
    }
    g_feed.resize(write);
}

const std::vector<FeedLine>& feed() { return g_feed; }

void report_player_kill(PlayerState& player, Faction victim) {
    const std::vector<RepKillEffect> effects =
        faction::apply_player_kill(player.rep, victim);

    std::printf("[rep] player destroyed a %s ship\n", faction::to_name(victim));
    if (effects.empty()) {
        std::printf("[rep]   (no factions cared)\n");
        return;
    }

    auto stance_name = [](Stance s) {
        return (s == Stance::Hostile) ? "HOSTILE"
             : (s == Stance::Allied)  ? "ALLIED"
             :                          "Neutral";
    };

    for (const RepKillEffect& e : effects) {
        const int   delta = (int)e.after - (int)e.before;
        const char* fname = faction::to_name(e.faction);

        // stdout audit line — exact numbers for validation/log scraping.
        std::printf("[rep]   %-9s %+d  (%d -> %d)\n",
                    fname, delta, (int)e.before, (int)e.after);

        // HUD status line, e.g. "Pirates: reputation -5".
        char status[96];
        std::snprintf(status, sizeof(status), "%s: reputation %+d", fname, delta);
        push(status, /*taunt=*/false);

        // Stance threshold flip — the consequence that actually changes
        // how the living world treats the player (perception + encounters
        // read this every frame).
        if (e.stance_before != e.stance_after) {
            std::printf("[rep]   >> %s are now %s to the player\n",
                        fname, stance_name(e.stance_after));
            char flip[96];
            std::snprintf(flip, sizeof(flip), "%s are now %s",
                          fname, stance_name(e.stance_after));
            push(flip, /*taunt=*/false);
        }

        // Flavour taunt, if the faction reacted vocally and we have a line.
        if (e.reaction != KillReaction::None) {
            const Event ev = (e.reaction == KillReaction::Praise)
                               ? Event::KillTheirEnemy
                               : Event::KilledByPlayerCrime;
            std::string line = pick_line(e.faction, ev);
            if (!line.empty()) {
                std::printf("[comm]   %s\n", line.c_str());
                push(line, /*taunt=*/true);
            }
        }
    }
}

#ifndef COMM_HEADLESS
void draw() {
    if (g_feed.empty()) return;

    // Logical-pixel screen dims (Retina-aware), same convention as
    // cockpit_hud — ImGui drawlist coords are logical pixels.
    const float dpi = sapp_dpi_scale();
    const float sw  = (float)sapp_width()  / dpi;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Top-centre-ish column, below where the upper HUD furniture sits.
    const float x      = sw * 0.5f - 230.0f;
    float       y      = 90.0f;
    const float line_h = 20.0f;

    for (const FeedLine& l : g_feed) {
        // Fade alpha over the final k_fade_s of the lifetime.
        const float remain = k_line_lifetime_s - l.age_s;
        float a = 1.0f;
        if (remain < k_fade_s) a = remain / k_fade_s;
        if (a < 0.0f) a = 0.0f;
        const int alpha = (int)(a * 255.0f);

        // Amber for comm chatter, cool white for rep status — mirrors the
        // cockpit_hud palette so the feed reads as part of the same HUD.
        const ImU32 col = l.taunt ? IM_COL32(255, 217, 77, alpha)
                                  : IM_COL32(210, 225, 235, alpha);
        // Cheap drop shadow for legibility over bright scenes.
        dl->AddText(ImVec2(x + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, alpha), l.text.c_str());
        dl->AddText(ImVec2(x, y), col, l.text.c_str());
        y += line_h;
    }
}
#else  // COMM_HEADLESS
void draw() {}   // no-op in the offline test driver
#endif

} // namespace comm

// -----------------------------------------------------------------------------
// comm.cpp — faction comm chatter + reputation HUD feed. See header.
// -----------------------------------------------------------------------------

#include "comm.h"

#ifndef COMM_HEADLESS
#include "cinematic.h"   // active() — suppress ambient barks during cutscenes
#endif
#include "json.h"
#include "player.h"
#include "voice.h"

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

// Map a comm Event to the voice Category it should speak in. Single
// source of truth for the comm -> voice wiring so callers don't have
// to remember which Category goes with which Event (or duplicate the
// event_key switch into a parallel category switch).
voice::Category event_to_category(Event e) {
    return (e == Event::KillTheirEnemy)
               ? voice::Category::Greeting   // praise -> friendly line
               : voice::Category::Hostile;    // aggression/threat -> bark
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

// ---- speaker indicator ---------------------------------------------------
// "Who's talking to me" HUD state. g_speaker_id is the active speaker's
// monotonic id (0 when none/expired); g_speaker_age_s ticks down in the
// shared tick(dt) so we don't need a separate aging callback. k_speaker_
// show_s is the dwell time — long enough that a final line still has a
// visible marker, short enough that a stale speaker doesn't linger.
uint32_t g_speaker_id        = 0;
Faction  g_speaker_faction   = Faction::Civilian;
float    g_speaker_age_s     = 0.0f;
constexpr float k_speaker_show_s = 4.0f;

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

// The dev_remote observation tap (see comm.h). Empty by default — push()
// pays one branch when nothing is listening.
namespace { std::function<void(const std::string&, bool)> g_feed_tap; }

void set_feed_tap(std::function<void(const std::string& text, bool taunt)> tap) {
    g_feed_tap = std::move(tap);
}

void push(const std::string& text, bool taunt) {
    g_feed.push_back(FeedLine{ text, 0.0f, taunt });
    // Cap the backlog — drop the oldest. erase-front on a tiny vector is
    // cheaper than the bookkeeping a ring buffer would need.
    while (g_feed.size() > k_max_lines) g_feed.erase(g_feed.begin());
    if (g_feed_tap) g_feed_tap(text, taunt);
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

    // Age out the speaker indicator. When the timer expires we retire
    // the id so the HUD gates out cleanly; the faction stays as-is but
    // no one queries it (speaker_id() == 0 short-circuits the draw).
    if (g_speaker_id != 0) {
        g_speaker_age_s -= dt;
        if (g_speaker_age_s <= 0.0f) g_speaker_id = 0;
    }
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

    // Voice candidate tracking (#43): at most ONE spoken line per kill
    // report, regardless of how many factions react. Anger wins over
    // Praise, and within either bucket we keep the FIRST faction to
    // appear in the effects list (iteration order == most-natural
    // "this is the biggest deal" ordering already).
    Faction       voice_speaker = Faction::Civilian;
    voice::Category voice_cat   = voice::Category::Greeting;
    bool          voice_chosen  = false;

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
            // Pick the most-salient voice candidate for this kill report.
            // Anger beats Praise (we'd rather hear the angrier faction),
            // and we cap at ONE spoken line per call no matter how many
            // factions react — so multiple anger/praise reactions in the
            // same call don't pile on top of each other in audio. A
            // voiceless faction (Kilrathi, #640) keeps its text line above
            // but never claims the one spoken slot.
            if (!voice_chosen && voice::speaks(e.faction)) {
                if (e.reaction == KillReaction::Anger) {
                    voice_speaker = e.faction;
                    voice_cat     = voice::Category::Hostile;
                    voice_chosen  = true;
                } else if (e.reaction == KillReaction::Praise) {
                    voice_speaker = e.faction;
                    voice_cat     = voice::Category::Greeting;
                    voice_chosen  = true;
                }
            }
        }
    }

    // One-and-only-one spoken line for this report_player_kill call.
    // Suppress during cinematics — same rationale as npc_engage_bark.
#ifndef COMM_HEADLESS
    if (voice_chosen && !cinematic::active()) {
        voice::say(voice_speaker, voice_cat, HMM_Vec3{0,0,0},
                   /*to_player=*/true);
    }
#else
    (void)voice_chosen;   // headless: feed text only, no audio stack
#endif
}

void npc_engage_bark(Faction speaker, bool target_is_player, uint32_t speaker_id) {
    // Only the player's HUD gets the line; NPC-on-NPC chatter is
    // cosmetic and would just spam the feed with fights we're not in.
    if (!target_is_player) return;
    // Suppress ambient combat barks while a cinematic is playing — the
    // cutscene's own dialogue should be the only voice on the radio. Headless
    // harnesses have no cinematic runtime, so they deliberately exercise the
    // feed behavior without linking the renderer/audio dependency tree.
#ifndef COMM_HEADLESS
    if (cinematic::active()) return;
#endif
    std::string line = pick_line(speaker, Event::KilledByPlayerCrime);
    if (line.empty()) return;
    push(line, /*taunt=*/true);
    // Voice the bark in this NPC's OWN stable voice (radio path, 2D).
    // say_ship resolves speaker_id -> a consistent voice so each ship
    // sounds like itself; rate-limiting is the caller's job
    // (ShipAIState::last_bark_at) so this stays unconditional.
#ifndef COMM_HEADLESS
    voice::say_ship(speaker, speaker_id,
                    event_to_category(Event::KilledByPlayerCrime),
                    HMM_Vec3{0,0,0}, /*to_player=*/true);
#endif
    // Mark the speaker for the on-ship HUD indicator. 0 means "no ship
    // context" (dev panel); skip the marker in that case.
    if (speaker_id != 0) set_speaker(speaker_id, speaker);
}

void set_speaker(uint32_t ship_id, Faction faction) {
    g_speaker_id      = ship_id;
    g_speaker_faction = faction;
    g_speaker_age_s   = k_speaker_show_s;
}

uint32_t speaker_id() {
    return g_speaker_id;
}

Faction speaker_faction() {
    return g_speaker_faction;
}

#ifndef COMM_HEADLESS
void draw() {
    if (g_feed.empty()) return;

    // Logical-pixel screen dims (Retina-aware), same convention as
    // cockpit_hud — ImGui drawlist coords are logical pixels.
    const float dpi = sapp_dpi_scale();
    const float sw  = (float)sapp_width()  / dpi;

    // Boxed COMMS panel anchored top-centre, just below the FLIGHT panel.
    // Same amber-border / dark-bg style as cockpit_hud's STATUS / TARGET
    // / FLIGHT MFDs so the feed reads as part of the same HUD vocabulary
    // instead of free-floating text. Height grows with the line count
    // (title + separator + ~18 px/line). Window flags mirror cockpit_hud's
    // kHudWindowFlags exactly so the panel can't steal input.
    constexpr float w_box = 600.0f;
    const float h_box = 36.0f + (float)g_feed.size() * 18.0f;
    constexpr float top_pad = 16.0f + 140.0f;   // sit under the FLIGHT panel
    ImGui::SetNextWindowPos(ImVec2(sw * 0.5f - w_box * 0.5f, top_pad),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w_box, h_box), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32( 10,  14,  20, 220));
    ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(255, 217,  77, 240));
    ImGui::PushStyleVar  (ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar  (ImGuiStyleVar_WindowPadding,    ImVec2(8.0f, 6.0f));

    constexpr ImGuiWindowFlags hud_flags =
        ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize        |
        ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoCollapse         | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav           |
        ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##comm_feed", nullptr, hud_flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 217, 77, 240));
        ImGui::TextUnformatted("COMMS");
        ImGui::PopStyleColor();
        ImGui::Separator();
        for (const FeedLine& l : g_feed) {
            // Fade alpha over the final k_fade_s of the lifetime.
            const float remain = k_line_lifetime_s - l.age_s;
            float a = 1.0f;
            if (remain < k_fade_s) a = remain / k_fade_s;
            if (a < 0.0f) a = 0.0f;
            const int alpha = (int)(a * 255.0f);
            // Amber for comm chatter, cool white for rep status.
            const ImU32 col = l.taunt ? IM_COL32(255, 217,  77, alpha)
                                      : IM_COL32(210, 225, 235, alpha);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar  (2);
    ImGui::PopStyleColor(2);
}
#else  // COMM_HEADLESS
void draw() {}   // no-op in the offline test driver
#endif

} // namespace comm

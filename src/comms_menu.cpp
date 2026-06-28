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

#include "audio.h"
#include "faction.h"
#include "json.h"
#include "ship.h"
#include "ship_class.h"
#include "system_def.h"
#include "voice.h"

#include "imgui.h"

#include <cstdio>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
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

// ---- per-line voice manifest (data-driven) -------------------------------
// Maps a comm line's EXACT text -> its mp3 path (loaded from
// player_comms_voice.json). Empty/missing manifest => every line falls back
// to the placeholder pilot voice. Populated in load(), read in select().
std::unordered_map<std::string, std::string> g_line_voice;

// Lazy path -> SampleId cache so each clip is decoded at most once (same
// trick as scripted_encounters.cpp / loot.cpp). Caches 0 too, so a
// missing/failed-to-load path doesn't get retried on every pick.
std::unordered_map<std::string, SampleId> g_clip_cache;

// Resolve a clip path to a SampleId via the lazy cache. First call hits
// audio::load(); subsequent calls reuse the cached id (0 included).
SampleId resolve_clip(const std::string& path) {
    auto it = g_clip_cache.find(path);
    if (it != g_clip_cache.end()) return it->second;
    const SampleId sid = audio::load(path);
    g_clip_cache.emplace(path, sid);
    return sid;
}

// ---- hailed-party response bank (data-driven) ----------------------------
// Loaded from comm_responses.json: faction -> category -> list of canned
// reply lines, each with its own pre-generated voice clip. When the player
// hails someone, the recipient barks back one of these a beat later (see the
// pending-reply machinery + tick()). Missing file is non-fatal: the bank
// stays empty and nobody talks back.
struct Response {
    std::string text;
    std::string clip;
};
using CategoryBank = std::unordered_map<std::string, std::vector<Response>>;
std::unordered_map<std::string, CategoryBank> g_responses;

// RNG for picking a random reply line. Module-static, seeded once.
std::mt19937 g_rng{ std::random_device{}() };

// Faction -> response-bank key. Same gotcha as voice.cpp: the engine's
// Faction::Hunter maps to the bank's "bounty_hunter". Civilian has no bank
// entry (bases stay silent for now). Returns nullptr for factions with no
// bank presence.
const char* faction_to_bank_name(Faction f) {
    switch (f) {
        case Faction::Hunter:   return "bounty_hunter";
        case Faction::Merchant: return "merchant";
        case Faction::Confed:   return "confed";
        case Faction::Militia:  return "militia";
        case Faction::Pirate:   return "pirate";
        case Faction::Retro:    return "retro";
        case Faction::Kilrathi: return "kilrathi";
        case Faction::Civilian: return nullptr;   // bases stay silent
        default:                return nullptr;
    }
}

// Capitalize a lowercase bank key for the comm-log display name, e.g.
// "pirate" -> "Pirate", "bounty_hunter" -> "Bounty_hunter". Cheap ASCII.
std::string display_name(const std::string& bank) {
    std::string s = bank;
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
    return s;
}

// A scheduled reply from a hailed party. Armed in select() on SEND, fired by
// tick() once `play_at` is reached so the reply doesn't talk over the
// player's own outgoing line.
struct PendingReply {
    bool        active   = false;
    std::string bank_faction;   // key into g_responses (e.g. "pirate")
    std::string category;       // "hostile" or "greeting"
    float       play_at  = 0.f; // process-uptime seconds
};
PendingReply g_pending;

// Latest process-uptime, refreshed every tick(). select() reads it to
// schedule g_pending.play_at since it has no clock of its own.
float g_now = 0.f;

// ---- interaction state ---------------------------------------------------
enum class Step { DestSelect, LineSelect };
Step g_step = Step::DestSelect;

// One selectable destination row, cached during the DestSelect draw so
// select() (a separate callsite) can resolve a 1-based pick.
struct Dest {
    std::string label;       // shown in the menu
    bool        hostile;     // resolved stance → which line pool to offer
    Faction     faction;     // recipient's engine faction (bases: Civilian)
    Stance      stance;      // recipient's stance toward the player
};
std::vector<Dest> g_dests;

// The destination the player chose in DestSelect — drives which pool
// LineSelect shows + logs, and (via faction/stance) which reply bank fires.
bool    g_chosen_hostile = false;
Faction g_chosen_faction = Faction::Civilian;
Stance  g_chosen_stance  = Stance::Neutral;

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

// Push an already-formatted line (e.g. a hailed party's reply) verbatim,
// without the "You: " prefix push_log() adds.
void push_log_raw(const std::string& line) {
    g_log.push_back(line);
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
        // Bases carry no faction of their own — store Civilian so no reply
        // bank entry exists and they stay silent for now.
        const Stance st = faction::stance_npc_vs_player(Faction::Civilian, rep);
        g_dests.push_back({ nav.name, st == Stance::Hostile,
                            Faction::Civilian, st });
    }
    if (target) {
        const char* cls = target->klass ? target->klass->display_name.c_str()
                                         : "Unknown";
        const char* fac = faction::to_name(target->faction);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Target: %s / %s", cls, fac);
        const Stance st = faction::stance_npc_vs_player(target->faction, rep);
        g_dests.push_back({ buf, st == Stance::Hostile,
                            target->faction, st });
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

    // Also pull in the per-line voice manifest (line text -> mp3 path). This
    // is a sibling file, NOT a key inside `path`, so it's loaded separately
    // and is just as non-fatal: a missing/unparseable manifest leaves
    // g_line_voice empty and every pick falls back to the placeholder voice.
    constexpr const char* kVoicePath = "assets/data/player_comms_voice.json";
    g_line_voice.clear();
    json::Value voice_root = json::parse_file(kVoicePath);
    if (voice_root.is_object()) {
        for (const auto& [text, val] : voice_root.as_object())
            if (val.is_string()) g_line_voice.emplace(text, val.as_string());
        std::printf("[comms_menu] loaded %s — %zu line-voice mappings\n",
                    kVoicePath, g_line_voice.size());
    } else {
        std::printf("[comms_menu] no %s — comm lines use placeholder voice\n",
                    kVoicePath);
    }

    // Hailed-party response bank (faction -> category -> [{text,clip}]). Like
    // the voice manifest it's a sibling file and just as non-fatal: a
    // missing/unparseable bank leaves g_responses empty and nobody replies.
    constexpr const char* kRespPath = "assets/data/comm_responses.json";
    g_responses.clear();
    json::Value resp_root = json::parse_file(kRespPath);
    if (resp_root.is_object()) {
        size_t lines = 0;
        for (const auto& [fac, cats] : resp_root.as_object()) {
            if (!cats.is_object()) continue;
            CategoryBank bank;
            for (const auto& [cat, arr] : cats.as_object()) {
                if (!arr.is_array()) continue;
                std::vector<Response> vec;
                for (const json::Value& v : arr.as_array()) {
                    if (!v.is_object()) continue;
                    const json::Value* text = v.find("text");
                    const json::Value* clip = v.find("clip");
                    if (!text || !text->is_string()) continue;
                    Response r;
                    r.text = text->as_string();
                    if (clip && clip->is_string()) r.clip = clip->as_string();
                    vec.push_back(std::move(r));
                }
                if (!vec.empty()) { lines += vec.size(); bank.emplace(cat, std::move(vec)); }
            }
            if (!bank.empty()) g_responses.emplace(fac, std::move(bank));
        }
        std::printf("[comms_menu] loaded %s — %zu factions, %zu response lines\n",
                    kRespPath, g_responses.size(), lines);
    } else {
        std::printf("[comms_menu] no %s — hailed parties stay silent\n",
                    kRespPath);
    }
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
        g_chosen_faction = g_dests[idx].faction;
        g_chosen_stance  = g_dests[idx].stance;
        g_step = Step::LineSelect;
        return;
    }

    // LineSelect — resolve the pick against the chosen pool.
    const std::vector<std::string>& pool =
        g_chosen_hostile ? g_hostile : g_friendly;
    if (idx >= pool.size()) return;

    const std::string& line = pool[idx];
    push_log(line);

    // Speak the EXACT chosen line if the manifest has a clip for it: lazy-load
    // (cached) and play it 2D, same player-directed gain as the rest of the
    // comm chatter. No mapping or a failed load() falls back to the
    // placeholder pilot voice so the menu always says *something*.
    bool spoke = false;
    if (auto it = g_line_voice.find(line); it != g_line_voice.end()) {
        if (const SampleId sid = resolve_clip(it->second); sid != 0) {
            audio::play(sid, 1.0f);
            spoke = true;
        }
    }
    if (!spoke) {
        // Placeholder pilot voice — the player's bar-patron voice id stands in
        // until a dedicated comms VO bank lands. 2D radio playback (to_player).
        voice::say("PrivBarPc01", voice::Category::Greeting,
                   HMM_Vec3{ 0, 0, 0 }, true);
    }

    // Arm the hailed party's reply. Only factions with a bank entry talk
    // back — Civilian (bases) and anyone the bank doesn't know stay silent.
    // The reply is delayed ~3.5s so it doesn't step on the player's own line
    // (tick() fires it once g_now reaches play_at).
    g_pending.active = false;
    if (const char* bank = faction_to_bank_name(g_chosen_faction)) {
        if (g_responses.count(bank)) {
            g_pending.active       = true;
            g_pending.bank_faction = bank;
            g_pending.category     = (g_chosen_stance == Stance::Hostile)
                                         ? "hostile" : "greeting";
            g_pending.play_at      = g_now + 3.5f;
        }
    }

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

void tick(float now_s) {
    // Refresh the shared clock FIRST so select() (which schedules off g_now)
    // and the due-check below agree on "now".
    g_now = now_s;
    if (!g_pending.active || now_s < g_pending.play_at) return;

    // Due: resolve the recipient's response bank/category and fire one random
    // line. Anything missing (no bank, no category, empty pool) silently
    // drops the pending reply — the hail just goes unanswered.
    PendingReply p = g_pending;
    g_pending.active = false;

    auto fac_it = g_responses.find(p.bank_faction);
    if (fac_it == g_responses.end()) return;
    auto cat_it = fac_it->second.find(p.category);
    if (cat_it == fac_it->second.end() || cat_it->second.empty()) return;

    const std::vector<Response>& pool = cat_it->second;
    std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
    const Response& r = pool[pick(g_rng)];

    push_log_raw(display_name(p.bank_faction) + ": " + r.text);

    // Voice the reply via the shared lazy clip cache (0 = missing/failed,
    // which we just skip — the log line still lands).
    if (!r.clip.empty()) {
        if (const SampleId sid = resolve_clip(r.clip); sid != 0)
            audio::play(sid, 1.0f);
    }
}

} // namespace comms_menu

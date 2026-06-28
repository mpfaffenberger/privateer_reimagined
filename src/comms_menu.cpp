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

// ---- player comm intents -------------------------------------------------
// The player can say one of four KINDS of thing to ANY destination now
// (greet, taunt, plead, or ask) — the recipient's stance no longer gates
// which pool is offered (np-comms enhancement #1). Each shown line remembers
// its intent so send-time can pick the right reply category (#2), so a taunt
// can provoke (#4), and so an ask can roll for a rumor.
enum class Intent { Greet, Taunt, Plea, Ask };

const char* intent_tag(Intent i) {
    switch (i) {
        case Intent::Greet: return "Greet";
        case Intent::Taunt: return "Taunt";
        case Intent::Plea:  return "Plea";
        case Intent::Ask:   return "Ask";
    }
    return "?";
}

// ---- line pools (data-driven) -------------------------------------------
// Loaded from player_comms.json's { greeting, taunt, plea } buckets. If the
// file is missing we fall back to a minimal built-in set per bucket so the
// screen is never empty.
std::vector<std::string> g_greeting = {
    "Greetings, this is a peaceful trader.",
    "Good hunting out there.",
    "Safe travels, friend.",
};
std::vector<std::string> g_taunt = {
    "You picked the wrong target, pal.",
    "Back off or I'll scatter you across the void!",
    "I'm gonna enjoy turning you into scrap.",
};
std::vector<std::string> g_plea = {
    "Please, I don't want any trouble!",
    "I'm unarmed, just let me pass!",
    "Have mercy, I've got a family waiting!",
};
std::vector<std::string> g_ask = {
    "Heard anything interesting out here?",
    "Any word on good salvage lately?",
    "Picked up any rumors worth chasing?",
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
// Loaded from comm_responses.json, now shaped as TWO top-level objects:
//   by_faction: faction-key -> category -> [{text,clip}]  (faction fallback)
//   by_voice:   voice_id    -> category -> [{text,clip}]  (recipient's OWN voice)
// When the player hails someone, the recipient barks back one of these a beat
// later (see the pending-reply machinery + tick()), preferring a line in its
// OWN stable voice and only falling back to the faction pool. Missing file is
// non-fatal: the banks stay empty and nobody talks back.
struct Response {
    std::string text;
    std::string clip;
};
using CategoryBank = std::unordered_map<std::string, std::vector<Response>>;
std::unordered_map<std::string, CategoryBank> g_resp_by_faction;
std::unordered_map<std::string, CategoryBank> g_resp_by_voice;

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

// Stable 32-bit hash of a base_id string (FNV-1a). Bases have no numeric
// entity id of their own, so we synthesize one here to feed voice::voice_for
// — same base_id always hashes to the same id, so a base ALWAYS speaks with
// the same voice across its lifetime.
uint32_t hash_base_id(const std::string& s) {
    uint32_t h = 2166136261u;
    for (const unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

// A scheduled reply from a hailed party. Armed in select() on SEND, fired by
// tick() once `play_at` is reached so the reply doesn't talk over the
// player's own outgoing line.
struct PendingReply {
    bool        active   = false;
    std::string bank_faction;   // key into g_resp_by_faction (e.g. "pirate") — fallback
    std::string reply_voice;    // recipient's stable voice_id (key into g_resp_by_voice)
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
    Faction     faction;     // recipient's engine faction (bases: resolved)
    Stance      stance;      // recipient's stance toward the player
    bool        is_ship = false;  // true = target ship (provoke candidate)
    uint32_t    ship_id = 0;      // ships: their Ship::id (0 = n/a)
    std::string base_id;          // bases: the nav's base_id ("" = none)
};
std::vector<Dest> g_dests;

// One row in the FLAT line list shown in LineSelect — intent + the text.
// Built when a destination is chosen (across all three buckets, cap 9), read
// by both draw() and select().
struct ShownLine {
    Intent      intent;
    std::string text;
};
constexpr size_t kShownMax = 9;
std::vector<ShownLine> g_shown;

// The destination the player chose in DestSelect — drives logging and (via
// faction/stance/intent) which reply bank+category fires, plus provoke.
Faction     g_chosen_faction = Faction::Civilian;
Stance      g_chosen_stance  = Stance::Neutral;
bool        g_chosen_is_ship = false;
uint32_t    g_chosen_ship_id = 0;
std::string g_chosen_base_id;        // bases: the nav's base_id ("" = ship/none)

// Provoke latch (#4). Set in select() when a taunt rolls success against a
// non-hostile target ship; drained ONCE by take_provoke_target().
uint32_t g_provoke_id = 0;

// Rumor latch (np-comms "ask for rumors"). Set in select() when an Ask at a
// non-hostile recipient rolls success; drained ONCE by take_rumor_pending(),
// which dispatches a fresh nav lead in main.cpp. Hostiles never share tips.
constexpr float k_rumor_chance = 0.04f;
bool g_rumor_pending = false;

// base_id -> resolved Faction cache (#3). Resolved lazily from
// assets/bases/<base_id>/base.json. Civilian == "couldn't resolve" (stays
// silent: no reply bank). Cached so each base.json is read at most once.
std::unordered_map<std::string, Faction> g_base_faction;

// Map a base.json "faction" display string to the engine enum. The match is
// SUBSTRING-based (on a lowercased copy) so real-world display variants land
// on the right enum: "Confederation Navy" -> confed, "Merchant Guild" ->
// merchant, "Pirates" -> pirate, etc. Falls back to faction::from_name for a
// canonical key. Returns Faction::Count when nothing matches (caller treats
// that as "silent").
Faction map_base_faction(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    auto has = [&](const char* kw) { return s.find(kw) != std::string::npos; };
    if (has("confederation")) return Faction::Confed;
    if (has("kilrathi"))      return Faction::Kilrathi;
    if (has("militia"))       return Faction::Militia;
    if (has("merchant"))      return Faction::Merchant;
    if (has("pirate"))        return Faction::Pirate;
    if (has("retro"))         return Faction::Retro;
    return faction::from_name(s);   // canonical keys ("confed", ...); Count if unknown
}

// Resolve (and cache) a base's faction from assets/bases/<base_id>/base.json.
// Any failure (missing file, no "faction", unmapped name) caches + returns
// Civilian so the base stays silent and we don't re-read on every frame.
Faction resolve_base_faction(const std::string& base_id) {
    if (auto it = g_base_faction.find(base_id); it != g_base_faction.end())
        return it->second;
    Faction resolved = Faction::Civilian;
    const std::string path = "assets/bases/" + base_id + "/base.json";
    json::Value root = json::parse_file(path);
    if (root.is_object()) {
        if (const json::Value* f = root.find("faction"); f && f->is_string()) {
            const Faction mapped = map_base_faction(f->as_string());
            if (mapped != Faction::Count) resolved = mapped;
        }
    }
    g_base_faction.emplace(base_id, resolved);
    return resolved;
}

// Build the FLAT shown-line list (cap 9) across all four intent buckets,
// round-robin so greet/taunt/plea/ask are all represented regardless of who
// the player is hailing. Called when a destination is chosen.
void build_shown_lines() {
    g_shown.clear();
    struct Bucket { Intent intent; const std::vector<std::string>* lines; };
    const Bucket buckets[4] = {
        { Intent::Greet, &g_greeting },
        { Intent::Taunt, &g_taunt },
        { Intent::Plea,  &g_plea },
        { Intent::Ask,   &g_ask },
    };
    size_t row = 0;
    bool added = true;
    while (added && g_shown.size() < kShownMax) {
        added = false;
        for (const Bucket& b : buckets) {
            if (row < b.lines->size()) {
                g_shown.push_back({ b.intent, (*b.lines)[row] });
                added = true;
                if (g_shown.size() >= kShownMax) break;
            }
        }
        ++row;
    }
}

// True iff a base/station/planet nav point is a hailable comms destination.
bool is_base_kind(const std::string& kind) {
    return kind == "station" || kind == "planet" || kind == "base";
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
        // Resolve the base's real faction from its base.json so it can talk
        // back (#3). Civilian == unresolved/no base_id == stays silent.
        const Faction fac = nav.base_id.empty() ? Faction::Civilian
                                                : resolve_base_faction(nav.base_id);
        const Stance st = faction::stance_npc_vs_player(fac, rep);
        Dest d;
        d.label   = nav.name;
        d.faction = fac;
        d.stance  = st;
        d.is_ship = false;
        d.base_id = nav.base_id;
        g_dests.push_back(std::move(d));
    }
    if (target) {
        const char* cls = target->klass ? target->klass->display_name.c_str()
                                         : "Unknown";
        const char* fac = faction::to_name(target->faction);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Target: %s / %s", cls, fac);
        const Stance st = faction::stance_npc_vs_player(target->faction, rep);
        Dest d;
        d.label   = buf;
        d.faction = target->faction;
        d.stance  = st;
        d.is_ship = true;
        d.ship_id = target->id;
        g_dests.push_back(std::move(d));
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
    read_pool("greeting", g_greeting);
    read_pool("taunt",    g_taunt);
    read_pool("plea",     g_plea);
    read_pool("ask",      g_ask);
    std::printf("[comms_menu] loaded %s — %zu greeting, %zu taunt, %zu plea, %zu ask lines\n",
                path.c_str(), g_greeting.size(), g_taunt.size(), g_plea.size(),
                g_ask.size());

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

    // Hailed-party response bank. Now shaped with TWO top-level objects:
    //   by_faction: faction-key -> category -> [{text,clip}]
    //   by_voice:   voice_id    -> category -> [{text,clip}]
    // Each parses into its own CategoryBank map. Like the voice manifest it's
    // a sibling file and just as non-fatal: a missing/unparseable bank leaves
    // both maps empty and nobody replies.
    constexpr const char* kRespPath = "assets/data/comm_responses.json";
    g_resp_by_faction.clear();
    g_resp_by_voice.clear();
    json::Value resp_root = json::parse_file(kRespPath);
    if (resp_root.is_object()) {
        // Parse one top-level object ({ key -> { cat -> [{text,clip}] } }) into
        // `out`, accumulating the total line count. Shared by by_faction and
        // by_voice since they're structurally identical.
        auto parse_section =
            [](const json::Value* section,
               std::unordered_map<std::string, CategoryBank>& out) -> size_t {
            size_t lines = 0;
            if (!section || !section->is_object()) return 0;
            for (const auto& [key, cats] : section->as_object()) {
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
                if (!bank.empty()) out.emplace(key, std::move(bank));
            }
            return lines;
        };
        const size_t fac_lines =
            parse_section(resp_root.find("by_faction"), g_resp_by_faction);
        const size_t voice_lines =
            parse_section(resp_root.find("by_voice"), g_resp_by_voice);
        std::printf("[comms_menu] loaded %s — by_faction: %zu factions/%zu lines, "
                    "by_voice: %zu voices/%zu lines\n",
                    kRespPath, g_resp_by_faction.size(), fac_lines,
                    g_resp_by_voice.size(), voice_lines);
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
        g_chosen_faction = g_dests[idx].faction;
        g_chosen_stance  = g_dests[idx].stance;
        g_chosen_is_ship = g_dests[idx].is_ship;
        g_chosen_ship_id = g_dests[idx].ship_id;
        g_chosen_base_id = g_dests[idx].base_id;
        build_shown_lines();
        g_step = Step::LineSelect;
        return;
    }

    // LineSelect — resolve the pick against the flat shown list.
    if (idx >= g_shown.size()) return;

    const Intent       intent = g_shown[idx].intent;
    const std::string& line   = g_shown[idx].text;

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

    // Reply category depends on WHAT was said + the recipient's stance (#2):
    //   * a Taunt always draws a "hostile" reply.
    //   * a Greet/Plea draws "hostile" only if the recipient already hates
    //     the player, else "greeting".
    const char* category = (intent == Intent::Taunt ||
                            g_chosen_stance == Stance::Hostile)
                               ? "hostile" : "greeting";

    // Provoke-via-comms (#4): a Taunt at a target SHIP that isn't already
    // hostile has a ~60% chance to flip it aggressive. main.cpp drains the
    // latch and sets the AI override; the "hostile" reply above already
    // delivers the snarl ("...you're dead!") with its voice.
    if (intent == Intent::Taunt && g_chosen_is_ship &&
        g_chosen_stance != Stance::Hostile && g_chosen_ship_id != 0) {
        std::uniform_real_distribution<float> roll(0.0f, 1.0f);
        if (roll(g_rng) < 0.60f) g_provoke_id = g_chosen_ship_id;
    }

    // Rumor roll (np-comms "ask for rumors"): an Ask at a non-hostile
    // recipient has a small chance (k_rumor_chance) to surface a fresh nav
    // lead. Hostiles don't share tips. main.cpp drains the latch and drops
    // the actual lead marker + HUD line.
    if (intent == Intent::Ask && g_chosen_stance != Stance::Hostile) {
        std::uniform_real_distribution<float> roll(0.0f, 1.0f);
        if (roll(g_rng) < k_rumor_chance) g_rumor_pending = true;
    }

    // Arm the hailed party's reply. Resolve the recipient's STABLE voice so it
    // replies in its OWN voice (with matching text) where possible, falling
    // back to the faction pool. The recipient's entity id is the target ship's
    // id for ships, or a stable hash of the base_id for bases. Only recipients
    // with either a known voice pool OR a faction bank entry talk back —
    // Civilian (unresolved bases) and anyone neither map knows stay silent.
    // The reply is delayed ~3.5s so it doesn't step on the player's own line
    // (tick() fires it once g_now reaches play_at).
    g_pending.active = false;
    {
        const uint32_t entity_id = g_chosen_is_ship
                                       ? g_chosen_ship_id
                                       : hash_base_id(g_chosen_base_id);
        const std::string reply_voice =
            voice::voice_for(g_chosen_faction, entity_id);
        const char* bank = faction_to_bank_name(g_chosen_faction);
        const bool has_voice = !reply_voice.empty() &&
                               g_resp_by_voice.count(reply_voice);
        const bool has_faction = bank && g_resp_by_faction.count(bank);
        if (has_voice || has_faction) {
            g_pending.active       = true;
            g_pending.bank_faction = bank ? bank : "";
            g_pending.reply_voice  = reply_voice;
            g_pending.category     = category;
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
        // FLAT line list — every intent offered to anyone (#1). Each row is
        // tagged with its intent so the player knows a taunt is a taunt
        // (and may provoke). Taunts paint red, the rest amber.
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("HAIL");
        ImGui::PopStyleColor();
        for (size_t i = 0; i < g_shown.size(); ++i) {
            const ShownLine& sl = g_shown[i];
            const ImU32 col = (sl.intent == Intent::Taunt) ? kRed : kHudWhite;
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Text("%zu. [%s] %s", i + 1, intent_tag(sl.intent),
                        sl.text.c_str());
            ImGui::PopStyleColor();
        }
    }

    // Footer hint. (No text log: the panel is too narrow to render comm
    // lines without truncating mid-word — the exchange is conveyed by voice.)
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("press 1-9");
    ImGui::PopStyleColor();
}

void tick(float now_s) {
    // Refresh the shared clock FIRST so select() (which schedules off g_now)
    // and the due-check below agree on "now".
    g_now = now_s;
    if (!g_pending.active || now_s < g_pending.play_at) return;

    // Due: resolve the recipient's response pool and fire one random line.
    // Pool precedence: the recipient's OWN voice (so it speaks consistently
    // in-character) if that voice has a non-empty pool for this category,
    // else the faction-level bank. Anything missing (no pool, empty pool)
    // silently drops the pending reply — the hail just goes unanswered.
    PendingReply p = g_pending;
    g_pending.active = false;

    // Look up `category` in a CategoryBank-map under `key`; returns nullptr
    // when the key/category is absent or the pool is empty.
    auto find_pool =
        [&](const std::unordered_map<std::string, CategoryBank>& m,
            const std::string& key) -> const std::vector<Response>* {
        if (key.empty()) return nullptr;
        auto kit = m.find(key);
        if (kit == m.end()) return nullptr;
        auto cit = kit->second.find(p.category);
        if (cit == kit->second.end() || cit->second.empty()) return nullptr;
        return &cit->second;
    };

    const std::vector<Response>* pool_ptr =
        find_pool(g_resp_by_voice, p.reply_voice);
    if (!pool_ptr) pool_ptr = find_pool(g_resp_by_faction, p.bank_faction);
    if (!pool_ptr) return;

    const std::vector<Response>& pool = *pool_ptr;
    std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
    const Response& r = pool[pick(g_rng)];

    // Voice the reply via the shared lazy clip cache (0 = missing/failed,
    // which we just skip — the hail simply goes unanswered).
    if (!r.clip.empty()) {
        if (const SampleId sid = resolve_clip(r.clip); sid != 0)
            audio::play(sid, 1.0f);
    }
}

uint32_t take_provoke_target() {
    const uint32_t id = g_provoke_id;
    g_provoke_id = 0;
    return id;
}

bool take_rumor_pending() {
    const bool pending = g_rumor_pending;
    g_rumor_pending = false;
    return pending;
}

} // namespace comms_menu

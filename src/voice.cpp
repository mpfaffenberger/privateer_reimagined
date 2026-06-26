// -----------------------------------------------------------------------------
// voice.cpp — voice-bank playback layer (np-ma3 / #37/#38/#39). See header
// for the design and the player-vs-ambient playback contract.
//
// Storage layout:
//   g_by_faction_cat  map<bank_faction, map<category, vector<SampleId>>>
//   g_by_voice        map<voice_id,    vector<SampleId>>
//   g_aliases         map<alias,       vector<SampleId>>
// All three are preloaded: every mp3 path in the bank is run through
// audio::load() once at load() and only the resolved SampleIds are kept.
// The hot path is just a find + a play() / play_world() — no I/O, no
// json, no allocation on the gameplay thread.
//
// GOTCHA 1 (faction -> bank key): faction::to_name() returns "hunter"
// for the hunter faction, but the bank uses "bounty_hunter". The translation
// below handles that one outlier; everything else passes through.
// GOTCHA 2 (category -> bank key): rumor/search/clear resolve to empty
// pools in the bank for Phase 0 — say() then no-ops for those, which is
// the desired behavior until those categories have actual clips.
// -----------------------------------------------------------------------------

#include "voice.h"

#include "audio.h"
#include "faction.h"
#include "json.h"

#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace voice {

namespace {

// ---- faction -> bank-key translation (GOTCHA 1) -------------------------
// The single divergence from faction::to_name() is "hunter" -> "bounty_hunter";
// everything else passes through unchanged.
const char* faction_to_bank_name(Faction f) {
    switch (f) {
        case Faction::Hunter:    return "bounty_hunter";
        case Faction::Civilian:  return "civilian";   // not in the bank -> no-op
        case Faction::Merchant:  return "merchant";
        case Faction::Confed:    return "confed";
        case Faction::Militia:   return "militia";
        case Faction::Pirate:    return "pirate";
        case Faction::Retro:     return "retro";
        case Faction::Kilrathi:  return "kilrathi";
        default:                 return nullptr;
    }
}

// ---- category -> bank-key translation (GOTCHA 2) -----------------------
// rumor / search / clear resolve to empty pools in the bank — silent no-op.
const char* category_to_bank_name(Category c) {
    switch (c) {
        case Category::Greeting: return "greeting";
        case Category::Hostile:  return "hostile";
        case Category::LowHp:    return "low_hp";
        case Category::Kill:     return "kill";
        case Category::Demand:   return "demand";
        case Category::Rumor:    return "rumor";      // not in bank yet
        case Category::Search:   return "search";     // not in bank yet
        case Category::Clear:    return "clear";      // not in bank yet
        default:                 return nullptr;
    }
}

// ---- bank tables -------------------------------------------------------
// by_faction_category: bank_key -> category -> [SampleId]
std::map<std::string,
        std::map<std::string, std::vector<SampleId>>> g_by_faction_cat;

// by_voice: voice_id -> [SampleId]
std::unordered_map<std::string, std::vector<SampleId>> g_by_voice;

// aliases: alias -> [SampleId] (kept separate so aliases win at lookup
// time even if a future build also adds the alias name as a real voice_id).
std::unordered_map<std::string, std::vector<SampleId>> g_aliases;

// True iff load() populated at least one usable pool. say() no-ops when
// false so the game runs silent if the bank is missing/corrupt.
bool g_loaded = false;

// ---- one-voice-at-a-time (player-directed only) -----------------------
// Last-wins: stop this voice before starting a new player-directed line.
// 0 == "no voice currently" — audio::stop() tolerates the zero id.
VoiceId g_player_voice = 0;

// ---- RNG ---------------------------------------------------------------
// Shared default-seeded generator — same pattern as comm.cpp. Voice
// flavour is cosmetic so reproducibility beats entropy here.
std::mt19937& rng() {
    static std::mt19937 r{0xBABEFACEu};
    return r;
}

// ---- helpers -----------------------------------------------------------
// Preload each mp3 path in `arr`; append the non-zero SampleIds to `out`.
void collect_paths(const json::Value& arr,
                   std::vector<SampleId>& out) {
    if (!arr.is_array()) return;
    for (const json::Value& v : arr.as_array()) {
        if (!v.is_string()) continue;
        if (SampleId s = audio::load(v.as_string()); s != 0) {
            out.push_back(s);
        }
    }
}

// Pick a random SampleId from a non-empty pool. Returns 0 if empty.
SampleId pick_random(const std::vector<SampleId>* pool) {
    if (!pool || pool->empty()) return 0;
    std::uniform_int_distribution<size_t> pick(0, pool->size() - 1);
    return (*pool)[pick(rng())];
}

// Play `s` honoring `to_player`:
//   * true  -> 2D radio via audio::play(); enforce last-wins by stopping
//              the previous player-directed voice first.
//   * false -> 3D world via audio::play_world(); no gating.
void play_sample(SampleId s, HMM_Vec3 pos, bool to_player) {
    if (to_player) {
        audio::stop(g_player_voice);                 // retire the previous line
        g_player_voice = audio::play(s, /*gain=*/1.0f);
    } else {
        audio::play_world(s, pos,
                          /*ref_dist=*/3000.0f,
                          /*max_dist=*/25000.0f,
                          /*loop=*/false);
    }
}

} // namespace

bool load(const std::string& path) {
    // Reset before reload so we don't accumulate stale entries.
    g_by_faction_cat.clear();
    g_by_voice.clear();
    g_aliases.clear();
    g_player_voice = 0;
    g_loaded = false;

    json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr,
                     "[voice] could not parse '%s' — voice disabled\n",
                     path.c_str());
        return false;
    }

    int n_faction_pools = 0;
    int n_voice_pools   = 0;
    int n_alias_pools   = 0;

    // by_faction_category: bank_faction -> category -> [paths]
    if (const json::Value* by_fc = root.find("by_faction_category");
        by_fc && by_fc->is_object()) {
        for (const auto& [fk, fv] : by_fc->as_object()) {
            if (!fv.is_object()) continue;
            for (const auto& [ck, cv] : fv.as_object()) {
                if (!cv.is_array()) continue;
                std::vector<SampleId> ids;
                collect_paths(cv, ids);
                if (!ids.empty()) {
                    g_by_faction_cat[fk][ck] = std::move(ids);
                    ++n_faction_pools;
                }
            }
        }
    }

    // by_voice: voice_id -> [paths]
    if (const json::Value* by_v = root.find("by_voice");
        by_v && by_v->is_object()) {
        for (const auto& [vk, vv] : by_v->as_object()) {
            if (!vv.is_array()) continue;
            std::vector<SampleId> ids;
            collect_paths(vv, ids);
            if (!ids.empty()) {
                g_by_voice[vk] = std::move(ids);
                ++n_voice_pools;
            }
        }
    }

    // aliases: alias -> [paths]. Separate map so say() can resolve aliases
    // before falling back to by_voice.
    if (const json::Value* al = root.find("aliases");
        al && al->is_object()) {
        for (const auto& [ak, av] : al->as_object()) {
            if (!av.is_array()) continue;
            std::vector<SampleId> ids;
            collect_paths(av, ids);
            if (!ids.empty()) {
                g_aliases[ak] = std::move(ids);
                ++n_alias_pools;
            }
        }
    }

    if (n_faction_pools == 0 && n_voice_pools == 0 && n_alias_pools == 0) {
        std::fprintf(stderr,
                     "[voice] '%s': no usable pools — voice disabled\n",
                     path.c_str());
        return false;
    }

    g_loaded = true;
    std::printf("[voice] loaded %d faction-category + %d voice + %d alias "
                "pools from %s\n",
                n_faction_pools, n_voice_pools, n_alias_pools, path.c_str());
    return true;
}

void say(Faction speaker, Category cat,
         HMM_Vec3 world_pos, bool to_player) {
    if (!g_loaded || !audio::ready()) return;

    const char* bankname = faction_to_bank_name(speaker);
    if (!bankname) return;
    const char* catstr = category_to_bank_name(cat);
    if (!catstr) return;        // includes the rumor/search/clear silent no-op

    auto f_it = g_by_faction_cat.find(bankname);
    if (f_it == g_by_faction_cat.end()) return;
    auto c_it = f_it->second.find(catstr);
    if (c_it == f_it->second.end()) return;

    SampleId s = pick_random(&c_it->second);
    if (s == 0) return;

    play_sample(s, world_pos, to_player);
}

void say(const std::string& voice_id, Category /*cat*/,
         HMM_Vec3 world_pos, bool to_player) {
    if (!g_loaded || !audio::ready()) return;

    // Aliases first ("confed_f" -> placeholder pool), then by_voice.
    // Category is best-effort for Phase 0 and ignored in this overload.
    const std::vector<SampleId>* pool = nullptr;
    if (auto it = g_aliases.find(voice_id); it != g_aliases.end()) {
        pool = &it->second;
    } else if (auto it = g_by_voice.find(voice_id); it != g_by_voice.end()) {
        pool = &it->second;
    }
    if (!pool) return;

    SampleId s = pick_random(pool);
    if (s == 0) return;

    play_sample(s, world_pos, to_player);
}

} // namespace voice

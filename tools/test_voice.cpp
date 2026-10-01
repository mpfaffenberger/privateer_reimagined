// -----------------------------------------------------------------------------
// tools/test_voice.cpp — voice-line selection proof (#640: no Kilrathi voices
// in space). Links the REAL voice.cpp + faction.cpp + json.cpp against the
// SHIPPED assets/data/voice_bank.json, with the audio mixer stubbed out below
// so every play()/play_world() is counted instead of heard.
//
// Proves that a Kilrathi speaker resolves to NO voice line on every in-flight
// entry point (faction-level say, per-ship say_ship, voice_for) even though
// the bank still carries Kilrathi clips — i.e. the selection drops them, the
// assets stay. A control faction (Pirate) must still speak, so a bank that
// silently failed to load can't pass the test by accident.
//
// Build + run (from the repo root):
//   cmake --build build --target test_voice && ./build/test_voice
// -----------------------------------------------------------------------------

#include "audio.h"
#include "faction.h"
#include "json.h"
#include "voice.h"

#include <cstdio>
#include <cstring>

// ---- audio stub ------------------------------------------------------------
// voice.cpp only needs load / play / play_world / stop / ready. load() hands
// out fake non-zero ids so the bank "preloads"; the play calls just count.
namespace {
int      g_plays   = 0;
uint32_t g_next_id = 0;
} // namespace

namespace audio {
SampleId load(const std::string&) { return ++g_next_id; }
VoiceId  play(SampleId, float) { ++g_plays; return ++g_next_id; }
VoiceId  play_world(SampleId, HMM_Vec3, float, float, bool) {
    ++g_plays;
    return ++g_next_id;
}
void stop(VoiceId) {}
bool ready() { return true; }
} // namespace audio

// ---- harness ---------------------------------------------------------------
static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

static constexpr voice::Category k_categories[] = {
    voice::Category::Greeting, voice::Category::Hostile, voice::Category::LowHp,
    voice::Category::Kill,     voice::Category::Demand,  voice::Category::Rumor,
    voice::Category::Search,   voice::Category::Clear,
};

// Plays produced by every (category x to_player) faction-level say() plus a
// spread of per-ship say_ship() calls for `f`.
static int plays_for(Faction f) {
    const int before = g_plays;
    for (voice::Category cat : k_categories) {
        for (bool to_player : {true, false}) {
            voice::say(f, cat, HMM_Vec3{0, 0, 0}, to_player);
            for (uint32_t id = 1; id <= 16; ++id)
                voice::say_ship(f, id, cat, HMM_Vec3{0, 0, 0}, to_player);
        }
    }
    return g_plays - before;
}

int main() {
    constexpr const char* k_bank = "assets/data/voice_bank.json";

    std::printf("== bank key mapping ==\n");
    check(!voice::speaks(Faction::Kilrathi), "Kilrathi do not speak in flight");
    check(voice::bank_faction(Faction::Kilrathi) == nullptr,
          "Kilrathi resolve to no voice bank");
    check(voice::speaks(Faction::Pirate) &&
              std::strcmp(voice::bank_faction(Faction::Pirate), "pirate") == 0,
          "Pirate still maps to the 'pirate' bank");
    check(std::strcmp(voice::bank_faction(Faction::Hunter), "bounty_hunter") == 0,
          "Hunter keeps the 'bounty_hunter' outlier key");
    check(voice::bank_faction(Faction::Civilian) == nullptr,
          "Civilian has no bank (bases stay silent)");

    std::printf("== shipped bank still carries Kilrathi clips ==\n");
    const json::Value root = json::parse_file(k_bank);
    const json::Value* fv  = root.is_object() ? root.find("faction_voices") : nullptr;
    const json::Value* kv  = fv ? fv->find("kilrathi") : nullptr;
    check(kv && kv->is_array() && !kv->as_array().empty(),
          "voice_bank.json still lists Kilrathi voices (assets untouched)");

    std::printf("== selection against the shipped bank ==\n");
    check(voice::load(k_bank), "voice bank loads");

    bool any_kilrathi_voice = false;
    for (uint32_t id = 0; id < 64; ++id)
        any_kilrathi_voice |= !voice::voice_for(Faction::Kilrathi, id).empty();
    check(!any_kilrathi_voice, "voice_for(Kilrathi, *) resolves to no voice");
    check(plays_for(Faction::Kilrathi) == 0,
          "say / say_ship for a Kilrathi speaker play nothing");

    // Control: the same calls for a voiced faction DO play, so a zero above
    // means "Kilrathi filtered", not "bank broken".
    check(!voice::voice_for(Faction::Pirate, 1).empty(),
          "voice_for(Pirate, 1) resolves to a voice");
    check(plays_for(Faction::Pirate) > 0, "say / say_ship for a Pirate play lines");

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

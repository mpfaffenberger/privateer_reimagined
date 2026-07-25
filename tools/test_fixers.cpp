// -----------------------------------------------------------------------------
// tools/test_fixers.cpp — headless proof for the fixer framework (#137).
//
// Links the REAL fixers.cpp (FIXERS_HEADLESS: no ImGui body) + plot.cpp and
// proves the acceptance criteria from the campaign goal:
//
//   1. placement by EXACT BASE id,
//   2. placement by ARCHETYPE PREDICATE (+ exclude_bases — the M22 shape),
//   3. plot-flag gating (requires_flags + forbids_flags) — including the
//      multi-stage-conversation-as-two-entries pattern,
//   4. the action grammar: native plot verbs mutate PlayerState; unknown
//      tokens route to the campaign handler seam,
//   5. missing/garbage table file is non-fatal (empty registry).
//
// Build:
//   cmake --build build --target test_fixers && ./build/test_fixers
// -----------------------------------------------------------------------------

#include "fixers.h"
#include "player.h"
#include "plot.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

// Does present_at() return exactly the ids in `want` (order-insensitive)?
bool present_ids(const std::string& base, const std::string& archetype,
                 const PlayerState& p, std::vector<std::string> want) {
    auto got = fixers::present_at(base, archetype, p);
    if (got.size() != want.size()) return false;
    for (const fixers::FixerDef* f : got) {
        bool found = false;
        for (auto it = want.begin(); it != want.end(); ++it)
            if (*it == f->id) { want.erase(it); found = true; break; }
        if (!found) return false;
    }
    return want.empty();
}

const char* kTable = R"json({
  "fixers": [
    { "id": "sandoval_offer", "name": "Ernesto Sandoval",
      "base": "new_detroit_industrial",
      "forbids_flags": ["sandoval_done"],
      "dialogue": ["I have a job.", "What's the pay?", "Iron to Liverpool."],
      "speaker": ["", "pc", ""],
      "portrait": "portraits/sandoval/_ref.png",
      "portrait_pc": "portraits/grayson/_ref.png",
      "voice": ["audio/fixers/sandoval_offer_00.mp3"],
      "offer": "15,000 credits on your return. Deal?",
      "accept_actions": ["set_flag:sandoval_accepted",
                         "offer_mission:m01"],
      "refuse_actions": [] },
    { "id": "tayla_intro", "name": "Tayla",
      "base": "new_detroit_industrial",
      "requires_flags": ["sandoval_done"],
      "dialogue": ["Sandoval is dead. Keep the artifact."],
      "done_actions": ["give_item:steltek_artifact",
                       "set_flag:tayla_available"] },
    { "id": "goodin", "name": "Sandra Goodin",
      "archetypes": ["mining"],
      "exclude_bases": ["rygannon"],
      "requires_flags": ["cross_done"],
      "dialogue": ["Admiral Terrell wants to see you."],
      "done_actions": ["set_flag:goodin_done"] }
  ]
})json";

} // namespace

int main() {
    std::printf("=== fixer framework harness (#137) ===\n\n");

    // ---- load a real table from a scratch file ---------------------------
    const std::string path = "/tmp/test_fixers.json";
    { std::ofstream f(path, std::ios::trunc); f << kTable; }
    check(fixers::load(path) == 3, "load() parsed 3 entries");

    PlayerState p = player::new_game("troy");

    // ---- 1. exact-base placement + forbids gate ---------------------------
    check(present_ids("new_detroit_industrial", "refinery", p,
                      { "sandoval_offer" }),
          "base placement: sandoval at ND (tayla gated off, goodin elsewhere)");
    check(present_ids("liverpool", "refinery", p, {}),
          "base placement: nobody at liverpool");

    // ---- 2. archetype predicate + exclusion (the M22 shape) --------------
    plot::set_flag(p, "cross_done");
    check(present_ids("wickerton", "mining", p, { "goodin" }),
          "archetype placement: goodin at ANY mining base once cross_done");
    check(present_ids("rygannon", "mining", p, {}),
          "archetype placement: goodin EXCLUDED at rygannon");
    check(present_ids("wickerton", "refinery", p, {}),
          "archetype placement: goodin absent at non-mining archetype");
    plot::clear_flag(p, "cross_done");
    check(present_ids("wickerton", "mining", p, {}),
          "gate: goodin absent again without cross_done");

    // ---- 3. multi-stage as two gated entries ------------------------------
    plot::set_flag(p, "sandoval_done");
    check(present_ids("new_detroit_industrial", "refinery", p,
                      { "tayla_intro" }),
          "stage flip: sandoval_done hides sandoval, reveals tayla");

    // ---- 4. action grammar -------------------------------------------------
    std::string handled;
    plot::set_action_handler(
        [&](const std::string& action, PlayerState&) -> bool {
            if (action.rfind("offer_mission:", 0) == 0) {
                handled = action.substr(14);
                return true;
            }
            return false;
        });
    const fixers::FixerDef* sand = fixers::find("sandoval_offer");
    const fixers::FixerDef* tay  = fixers::find("tayla_intro");
    check(sand && tay, "find() resolves both entries");
    if (sand) {
        fixers::accept(*sand, p);
        check(plot::has_flag(p, "sandoval_accepted"),
              "accept: native set_flag action ran");
        check(handled == "m01",
              "accept: campaign token routed to the handler seam");
    }
    if (tay) {
        fixers::dialogue_done(*tay, p);
        check(plot::has_item(p, "steltek_artifact"),
              "dialogue_done: native give_item action ran");
        check(plot::has_flag(p, "tayla_available"),
              "dialogue_done: second action ran in order");
    }

    // ---- 4a2. optional presentation fields ---------------------------------
    {
        const fixers::FixerDef* s = fixers::find("sandoval_offer");
        const fixers::FixerDef* t = fixers::find("tayla_intro");
        check(s && s->portrait == "portraits/sandoval/_ref.png",
              "portrait: parsed when present");
        check(s && s->voice.size() == 1 &&
                  s->voice[0] == "audio/fixers/sandoval_offer_00.mp3",
              "voice: parsed as a string array");
        // A SHORT voice array is legal -- paragraph 1 here simply has no clip.
        check(s && s->voice.size() < s->dialogue.size(),
              "voice: may be shorter than dialogue (unvoiced paragraphs)");
        check(t && t->portrait.empty() && t->voice.empty(),
              "portrait/voice: absent fields default to empty (text-only)");

        // Two-hander: speaker[] marks which lines the player-character says.
        check(s && s->speaker.size() == 3 && s->speaker[1] == "pc" &&
                  s->speaker[0].empty() && s->speaker[2].empty(),
              "speaker: 'pc' marks player lines, '' marks the fixer");
        check(s && s->speaker.size() == s->dialogue.size(),
              "speaker: stays index-aligned with dialogue");
        check(s && s->portrait_pc == "portraits/grayson/_ref.png",
              "portrait_pc: parsed for the player side of a two-hander");
        check(t && t->speaker.empty(),
              "speaker: absent array = every line is the fixer's");
    }

    // ---- 4b. $NM / $CS token expansion -------------------------------------
    {
        check(fixers::expand_tokens("Ah, Captain $NM.") == "Ah, Captain Burrows.",
              "expand_tokens: $NM -> surname");
        check(fixers::expand_tokens("Feel lucky, $CS?") == "Feel lucky, Grayson?",
              "expand_tokens: $CS -> callsign");
        check(fixers::expand_tokens("$CS, $NM, $CS") ==
                  "Grayson, Burrows, Grayson",
              "expand_tokens: repeated tokens all expand");
        check(fixers::expand_tokens("no tokens here") == "no tokens here",
              "expand_tokens: passthrough when no '$'");
        check(fixers::expand_tokens("cost $500 and $XX") == "cost $500 and $XX",
              "expand_tokens: unknown '$' sequences untouched");
        check(fixers::expand_tokens("trailing $") == "trailing $",
              "expand_tokens: trailing '$' does not overrun");
        check(fixers::expand_tokens("$N") == "$N",
              "expand_tokens: truncated token does not overrun");
    }

    // ---- 5. bad table is non-fatal ----------------------------------------
    check(fixers::load("/tmp/does_not_exist_fixers.json") == 0,
          "missing file -> empty registry, no crash");
    { std::ofstream f(path, std::ios::trunc); f << "{ not json"; }
    check(fixers::load(path) == 0,
          "garbage file -> empty registry, no crash");

    std::remove(path.c_str());
    std::printf("\n=== %s ===\n",
                g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}

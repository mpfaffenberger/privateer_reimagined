// -----------------------------------------------------------------------------
// test_cinematic.cpp — headless hardening proof for the cutscene PARSER
// (np-cinematic Phase 6.2). The parser was split into cinematic_parse.{h,cpp}
// precisely so it can be exercised with zero engine deps (no imgui / sokol /
// GL / audio). We feed it a battery of deliberately-malformed inline JSON and
// assert the contract: parse_document() NEVER throws — it either returns false
// with an error string, or returns a best-effort Cinematic with valid == true.
//
//   cmake --build build --target test_cinematic && ./build/test_cinematic
// -----------------------------------------------------------------------------

#include "cinematic_parse.h"
#include "cinematic_studio_io.h"
#include "cinematic_triggers.h"
#include "json.h"
#include "player.h"
#include "plot.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) { ++g_pass; }                                                \
        else { ++g_fail; std::printf("  FAIL: %s\n", msg); }                   \
    } while (0)

// Parse a JSON string into a Cinematic, asserting the whole thing NEVER throws
// (that's the point of the exercise). Returns the ok flag from parse_document.
static bool try_parse(const char* label, const std::string& text,
                      cinematic::Cinematic& out, std::string& err) {
    std::printf("[case] %s\n", label);
    bool ok = false;
    try {
        const json::Value root = json::parse(text);
        ok = cinematic::parse_document(root, "test_default", out, err);
    } catch (...) {
        ++g_fail;
        std::printf("  FAIL: parse_document THREW (must never throw)\n");
        return false;
    }
    return ok;
}

int main() {
    std::printf("=== test_cinematic (parser hardening, Phase 6.2) ===\n");

    // ---- #1 the exact confrontation_demo crash: a `line` with NO voice_file.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("line missing OPTIONAL voice_file",
            R"({"id":"x","timeline":[
                 {"cmd":"line","speaker":"Pirate","text":"Then you'll die.",
                  "side":"right","dur":3.0,"portrait":"p/pirate/02.png","t":9.2}
               ]})", c, err);
        CHECK(ok && c.valid, "must parse (no throw) with voice_file absent");
        CHECK(c.cues.size() == 1, "one cue survives");
        CHECK(!c.cues.empty() && c.cues[0].voice_file.empty(),
              "absent voice_file defaults to empty string");
    }

    // ---- a fuller confrontation-style doc mixing lines w/ and w/o voice_file.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("mixed lines (some without voice_file)",
            R"({"id":"confront","letterbox":true,"timeline":[
                 {"cmd":"music","file":"audio/bed.wav","t":0.0},
                 {"cmd":"line","speaker":"A","text":"hi","voice_file":"a.wav","t":2.0},
                 {"cmd":"line","speaker":"B","text":"bye","t":5.0},
                 {"cmd":"end","actions":["set_flag:seen"],"t":8.0}
               ]})", c, err);
        CHECK(ok && c.valid && c.cues.size() == 4, "all four cues parse");
    }

    // ---- optional fields absent across every cmd (faction, pos, ease, side,
    //      actions, look_at, dur ...). None may throw.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("every cmd with only its minimal keys",
            R"({"timeline":[
                 {"cmd":"fade_in","t":0.0},
                 {"cmd":"fade_out","t":1.0},
                 {"cmd":"music","t":0.0},
                 {"cmd":"sfx","t":0.5},
                 {"cmd":"camera_path","t":0.5},
                 {"cmd":"spawn","actor":"hero","t":0.2},
                 {"cmd":"actor_path","actor":"hero","t":0.6},
                 {"cmd":"line","t":2.0},
                 {"cmd":"subtitle","t":3.0},
                 {"cmd":"end","t":9.0}
               ]})", c, err);
        CHECK(ok && c.valid, "minimal-key cues parse without throwing");
        CHECK(c.cues.size() == 10, "all ten cmd kinds survive");
        // spawn with no faction defaults to civilian; line with no side -> left.
        bool spawn_ok = false, line_ok = false;
        for (auto& cue : c.cues) {
            if (cue.cmd == cinematic::Cmd::Spawn)
                spawn_ok = (cue.faction == "civilian");
            if (cue.cmd == cinematic::Cmd::Line)
                line_ok = cue.side_left;   // default left
        }
        CHECK(spawn_ok, "spawn faction defaults to 'civilian'");
        CHECK(line_ok, "line side defaults to 'left'");
    }

    // ---- #10 empty timeline -> valid, zero cues (instant-complete upstream).
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("empty timeline",
            R"({"id":"empty","timeline":[]})", c, err);
        CHECK(ok && c.valid && c.cues.empty(), "empty timeline is valid + no cues");
    }

    // ---- #10 no timeline key at all.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("no timeline key",
            R"({"id":"bare"})", c, err);
        CHECK(ok && c.valid && c.cues.empty(), "missing timeline -> valid, 0 cues");
    }

    // ---- end-only timeline.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("end-only timeline",
            R"({"timeline":[{"cmd":"end","t":0.0}]})", c, err);
        CHECK(ok && c.valid && c.cues.size() == 1, "end-only is valid, 1 cue");
    }

    // ---- unknown cmd is skipped, valid cues around it survive.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("unknown cmd skipped",
            R"({"timeline":[
                 {"cmd":"explode_planet","t":1.0},
                 {"cmd":"subtitle","text":"ok","t":2.0}
               ]})", c, err);
        CHECK(ok && c.valid && c.cues.size() == 1, "unknown cmd dropped, good cue kept");
    }

    // ---- cue objects missing "cmd" entirely are skipped.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("cue with no cmd skipped",
            R"({"timeline":[
                 {"t":1.0,"text":"orphan"},
                 {"cmd":"subtitle","text":"ok","t":2.0}
               ]})", c, err);
        CHECK(ok && c.valid && c.cues.size() == 1, "cmd-less cue dropped");
    }

    // ---- non-object cue entries (a bare number / string in the array).
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("non-object timeline entries skipped",
            R"({"timeline":[42,"nope",{"cmd":"subtitle","text":"ok","t":1.0}]})",
            c, err);
        CHECK(ok && c.valid && c.cues.size() == 1, "garbage entries dropped");
    }

    // ---- wrong-typed fields: t as string, dur as string, side as number.
    //      Must fall back to defaults, not throw.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("wrong-typed scalar fields default",
            R"({"timeline":[
                 {"cmd":"line","t":"soon","dur":"long","side":7,"text":"hi"}
               ]})", c, err);
        CHECK(ok && c.valid && c.cues.size() == 1, "wrong types don't throw");
        CHECK(!c.cues.empty() && c.cues[0].t == 0.0f, "string t -> default 0");
        CHECK(!c.cues.empty() && c.cues[0].dur == 1.0f, "string dur -> default 1");
        CHECK(!c.cues.empty() && c.cues[0].side_left, "non-string side -> default left");
    }

    // ---- malformed vec3s: wrong length, string elements, non-array pos.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("malformed vec3s default to origin",
            R"({"timeline":[
                 {"cmd":"spawn","actor":"a","pos":[1,2],"t":0.0},
                 {"cmd":"spawn","actor":"b","pos":["x","y","z"],"t":0.1},
                 {"cmd":"spawn","actor":"c","pos":"nope","t":0.2}
               ]})", c, err);
        CHECK(ok && c.valid && c.cues.size() == 3, "bad vec3s don't throw");
    }

    // ---- camera_path: ship: look_at target + bare-array look_at + junk keys.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("camera_path look_at variants",
            R"({"timeline":[{"cmd":"camera_path","dur":5,"t":0.0,"keys":[
                 {"pos":[0,0,0],"look_at":"ship:raider"},
                 {"pos":[1,1,1],"look_at":[9,9,9]},
                 {"pos":[2,2,2]},
                 "junk",
                 {"nopos":true}
               ]}]})", c, err);
        CHECK(ok && c.valid && c.cues.size() == 1, "camera_path parses");
        if (!c.cues.empty()) {
            const auto& keys = c.cues[0].cam_keys;
            // 4 object keys survive ("junk" string is skipped).
            CHECK(keys.size() == 4, "non-object key entry skipped");
            CHECK(!keys.empty() && keys[0].look_is_ship &&
                  keys[0].look_actor == "raider", "ship:raider look target parsed");
            CHECK(keys.size() > 1 && keys[1].has_look && !keys[1].look_is_ship,
                  "array look_at parsed as world point");
        }
    }

    // ---- #: root that isn't an object (array / scalar / parse failure) ->
    //      parse_document returns false, valid stays false, no throw.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("root is an array (not object)",
            R"([{"cmd":"end","t":0}])", c, err);
        CHECK(!ok && !c.valid && !err.empty(), "array root rejected with error");
    }
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("totally malformed JSON",
            R"({"timeline": [ {"cmd": )", c, err);   // truncated garbage
        CHECK(!ok && !c.valid, "truncated JSON rejected, no throw");
    }
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("empty string", "", c, err);
        CHECK(!ok && !c.valid, "empty input rejected, no throw");
    }

    // ---- time-sort: out-of-order authoring is sorted ascending by t.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("out-of-order cues are time-sorted",
            R"({"timeline":[
                 {"cmd":"subtitle","text":"c","t":9.0},
                 {"cmd":"subtitle","text":"a","t":1.0},
                 {"cmd":"subtitle","text":"b","t":5.0}
               ]})", c, err);
        CHECK(ok && c.cues.size() == 3, "three cues");
        CHECK(c.cues.size() == 3 && c.cues[0].t == 1.0f &&
              c.cues[1].t == 5.0f && c.cues[2].t == 9.0f, "sorted ascending by t");
    }

    // ========================================================================
    // OUTCOME block (Cinematic Studio Phase A2)
    // ========================================================================

    // ---- full outcome block parses; defaults fill absent fields.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("outcome: full block",
            R"({"id":"x","timeline":[],"outcome":{
                 "player_at_nav":"Troy Nav 8",
                 "spawns":[
                   {"class":"stiletto","faction":"confed","count":3},
                   {"class":"dralthi","faction":"kilrathi","count":3,"hostile":true},
                   {"class":"talon"}
                 ]}})", c, err);
        CHECK(ok && c.valid, "outcome doc parses");
        CHECK(c.outcome.present, "outcome.present set");
        CHECK(c.outcome.player_at_nav == "Troy Nav 8", "player_at_nav read");
        CHECK(c.outcome.spawns.size() == 3, "three spawn groups");
        if (c.outcome.spawns.size() == 3) {
            CHECK(!c.outcome.spawns[0].hostile, "hostile defaults false");
            CHECK(c.outcome.spawns[1].hostile, "explicit hostile true");
            CHECK(c.outcome.spawns[2].count == 1 &&
                  c.outcome.spawns[2].faction == "civilian",
                  "count defaults 1, faction defaults civilian");
        }
    }

    // ---- no outcome block -> present stays false.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("outcome: absent",
            R"({"id":"x","timeline":[]})", c, err);
        CHECK(ok && !c.outcome.present, "absent outcome -> present=false");
    }

    // ---- mistyped outcome (string) + garbage spawn entries: no throw.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("outcome: mistyped block ignored",
            R"({"id":"x","outcome":"go to troy","timeline":[]})", c, err);
        CHECK(ok && !c.outcome.present, "string outcome -> ignored, no throw");
    }
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("outcome: garbage spawn entries skipped",
            R"({"id":"x","timeline":[],"outcome":{"spawns":[
                 42, "junk",
                 {"faction":"confed"},
                 {"class":"talon","count":0},
                 {"class":"talon","count":"many"}
               ]}})", c, err);
        CHECK(ok && c.outcome.present, "outcome survives garbage spawns");
        // non-object, class-less, count:0 dropped; "many" -> default 1 kept.
        CHECK(c.outcome.spawns.size() == 1 && c.outcome.spawns[0].count == 1,
              "only the recoverable spawn survives (string count -> 1)");
    }

    // ========================================================================
    // LOCATION block (entry-point teleport)
    // ========================================================================

    // ---- present: system + nav read.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("location: full block",
            R"({"id":"x","timeline":[],
                "location":{"system":"penders_star","nav":"Asteroid Field"}})",
            c, err);
        CHECK(ok && c.valid, "location doc parses");
        CHECK(c.location.present, "location.present set");
        CHECK(c.location.system == "penders_star", "location.system read");
        CHECK(c.location.nav == "Asteroid Field", "location.nav read");
    }

    // ---- absent: present stays false, fields empty.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("location: absent",
            R"({"id":"x","timeline":[]})", c, err);
        CHECK(ok && !c.location.present, "absent location -> present=false");
        CHECK(c.location.system.empty() && c.location.nav.empty(),
              "absent location -> empty system/nav");
    }

    // ---- garbage: mistyped block / mistyped fields degrade, never throw.
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("location: mistyped block ignored",
            R"({"id":"x","location":"penders_star","timeline":[]})", c, err);
        CHECK(ok && !c.location.present,
              "string location -> ignored, no throw");
    }
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("location: wrong-typed fields default",
            R"({"id":"x","timeline":[],
                "location":{"system":42,"nav":[1,2,3]}})", c, err);
        CHECK(ok && c.location.present, "object block still counts as present");
        CHECK(c.location.system.empty() && c.location.nav.empty(),
              "non-string system/nav -> empty defaults, no throw");
    }
    {
        cinematic::Cinematic c; std::string err;
        const bool ok = try_parse("location: nav-only (system defaults empty)",
            R"({"id":"x","timeline":[],"location":{"nav":"Troy Nav 8"}})",
            c, err);
        CHECK(ok && c.location.present && c.location.system.empty() &&
              c.location.nav == "Troy Nav 8",
              "nav-only location -> current-system snap semantics");
    }

    // ========================================================================
    // TRIGGERS (Cinematic Studio Phase A1) — parse
    // ========================================================================
    namespace ct = cinematic::triggers;

    // Parse a trigger doc, asserting no-throw (same contract as try_parse).
    auto try_parse_triggers = [](const char* label, const std::string& text,
                                 std::vector<ct::Trigger>& out,
                                 std::string& err) -> bool {
        std::printf("[case] %s\n", label);
        try {
            const json::Value root = json::parse(text);
            return ct::parse_triggers(root, out, err);
        } catch (...) {
            ++g_fail;
            std::printf("  FAIL: parse_triggers THREW (must never throw)\n");
            return false;
        }
    };

    // ---- the doc's own schema example parses field-for-field.
    {
        std::vector<ct::Trigger> ts; std::string err;
        const bool ok = try_parse_triggers("triggers: full schema example",
            R"({"triggers":[{
                 "cinematic":"ambush_troy","once":true,"cooldown_s":0,
                 "when":{"system":"troy","ship_class":"tarsus",
                         "missiles_max":0,
                         "cargo":[{"commodity":"iron","min_units":20}],
                         "near_nav":{"nav":"Troy Nav 8","radius_m":20000},
                         "requires_flags":[],
                         "forbids_flags":["ambush_troy_seen"]}}]})", ts, err);
        CHECK(ok && ts.size() == 1, "one trigger parses");
        if (ts.size() == 1) {
            const ct::Trigger& t = ts[0];
            CHECK(t.cinematic == "ambush_troy" && t.once, "id + once read");
            CHECK(t.system == "troy" && t.ship_class == "tarsus", "system + class");
            CHECK(t.missiles_max == 0 && t.missiles_min == -1,
                  "missiles_max read, absent min stays -1");
            CHECK(t.cargo.size() == 1 && t.cargo[0].commodity == "iron" &&
                  t.cargo[0].min_units == 20, "cargo requirement read");
            CHECK(t.has_near_nav && t.nav == "Troy Nav 8" &&
                  t.radius_m == 20000.0f, "near_nav read");
            CHECK(t.forbids_flags.size() == 1 &&
                  t.forbids_flags[0] == "ambush_troy_seen", "forbids_flags read");
        }
    }

    // ---- minimal trigger: everything defaults, empty when = always hot.
    {
        std::vector<ct::Trigger> ts; std::string err;
        const bool ok = try_parse_triggers("triggers: minimal entry defaults",
            R"({"triggers":[{"cinematic":"intro"}]})", ts, err);
        CHECK(ok && ts.size() == 1, "minimal trigger parses");
        if (ts.size() == 1) {
            CHECK(ts[0].once && ts[0].cooldown_s == 0.0f, "once/cooldown default");
            CHECK(ts[0].system.empty() && !ts[0].has_near_nav &&
                  ts[0].missiles_max == -1 && ts[0].missiles_min == -1 &&
                  ts[0].cargo.empty() && ts[0].requires_flags.empty(),
                  "absent when-fields stay unset");
        }
    }

    // ---- malformed docs: array root rejected; junk entries skipped;
    //      cinematic-less entries dropped; wrong types default. No throws.
    {
        std::vector<ct::Trigger> ts; std::string err;
        const bool ok = try_parse_triggers("triggers: array root rejected",
            R"([{"cinematic":"x"}])", ts, err);
        CHECK(!ok && ts.empty() && !err.empty(), "non-object root -> false+err");
    }
    {
        std::vector<ct::Trigger> ts; std::string err;
        const bool ok = try_parse_triggers("triggers: missing/mistyped list",
            R"({"triggers":"nope"})", ts, err);
        CHECK(ok && ts.empty(), "mistyped triggers list -> valid, zero");
    }
    {
        std::vector<ct::Trigger> ts; std::string err;
        const bool ok = try_parse_triggers("triggers: junk entries skipped",
            R"({"triggers":[42,"junk",{"once":false},
                 {"cinematic":"ok","once":"yes","cooldown_s":"long",
                  "when":{"near_nav":{"radius_m":500},
                          "cargo":[{"min_units":5},"junk"]}}]})", ts, err);
        CHECK(ok && ts.size() == 1, "only the id-bearing entry survives");
        if (ts.size() == 1) {
            CHECK(ts[0].once && ts[0].cooldown_s == 0.0f,
                  "wrong-typed once/cooldown -> defaults");
            CHECK(!ts[0].has_near_nav, "near_nav without a nav name is unset");
            CHECK(ts[0].cargo.empty(), "commodity-less cargo reqs dropped");
        }
    }

    // ========================================================================
    // TRIGGERS — once/cooldown latch + condition logic (headless, fake hook)
    // ========================================================================

    // Drive the module through load() with a temp file so we exercise the
    // real load path (file -> parse -> state) plus tick's latch bookkeeping.
    auto load_triggers_text = [](const std::string& text) {
        const char* tmp = "._test_triggers_tmp.json";
        FILE* f = std::fopen(tmp, "wb");
        if (f) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
        ct::load(tmp);
        std::remove(tmp);
    };

    PlayerState test_player;
    int fires = 0;
    bool allow_play = true;
    ct::set_play_hook([&](const std::string&) {
        if (!allow_play) return false;
        ++fires;
        return true;
    });

    // A ctx that satisfies the demo-style trigger below.
    ct::TriggerCtx ctx;
    ctx.system_id      = "troy";
    ctx.ship_class     = "tarsus";
    ctx.missiles_total = 0;
    ctx.player_pos     = HMM_V3(0, 0, 0);
    ctx.nav_pos = [](const std::string& nav, HMM_Vec3& out) {
        if (nav != "Troy Nav 8") return false;
        out = HMM_V3(10000.0f, 0.0f, 0.0f);   // 10 km out
        return true;
    };
    ctx.cargo_units = [](const std::string& c) { return c == "iron" ? 25 : 0; };
    ctx.player = &test_player;

    {
        std::printf("[case] trigger latch: once=true fires exactly once\n");
        load_triggers_text(
            R"({"triggers":[{"cinematic":"ambush_troy","once":true,
                 "when":{"system":"troy","ship_class":"tarsus",
                         "missiles_max":0,
                         "cargo":[{"commodity":"iron","min_units":20}],
                         "near_nav":{"nav":"Troy Nav 8","radius_m":20000},
                         "forbids_flags":["ambush_troy_seen"]}}]})");
        CHECK(ct::count() == 1, "one trigger loaded via load()");
        fires = 0;
        ct::tick(ctx, 0.0f);
        ct::tick(ctx, 1.0f);
        ct::tick(ctx, 2.0f);
        CHECK(fires == 1, "once=true latches after the first fire");
        ct::reset();
        ct::tick(ctx, 3.0f);
        CHECK(fires == 2, "reset() clears the once-latch");
    }

    {
        std::printf("[case] trigger latch: refused fire does NOT latch\n");
        ct::reset();
        fires = 0;
        allow_play = false;          // host says "not now" (e.g. mid-cinematic)
        ct::tick(ctx, 0.0f);
        CHECK(fires == 0, "refused fire");
        allow_play = true;
        ct::tick(ctx, 1.0f);
        CHECK(fires == 1, "trigger retried and fired once the host allowed it");
    }

    {
        std::printf("[case] trigger cooldown: once=false re-fires after cooldown_s\n");
        load_triggers_text(
            R"({"triggers":[{"cinematic":"nagger","once":false,"cooldown_s":10,
                 "when":{"system":"troy"}}]})");
        fires = 0;
        ct::tick(ctx, 100.0f);
        CHECK(fires == 1, "first fire");
        ct::tick(ctx, 105.0f);
        CHECK(fires == 1, "suppressed inside cooldown");
        ct::tick(ctx, 110.5f);
        CHECK(fires == 2, "re-fires after cooldown elapses");
    }

    {
        std::printf("[case] trigger conditions: each gate blocks independently\n");
        load_triggers_text(
            R"({"triggers":[{"cinematic":"ambush_troy","once":true,
                 "when":{"system":"troy","ship_class":"tarsus",
                         "missiles_max":0,
                         "cargo":[{"commodity":"iron","min_units":20}],
                         "near_nav":{"nav":"Troy Nav 8","radius_m":20000},
                         "forbids_flags":["ambush_troy_seen"]}}]})");
        fires = 0;
        ct::TriggerCtx bad = ctx;
        bad.system_id = "pyrenees";
        ct::tick(bad, 0.0f);
        CHECK(fires == 0, "wrong system blocks");
        bad = ctx; bad.ship_class = "centurion";
        ct::tick(bad, 0.0f);
        CHECK(fires == 0, "wrong ship class blocks");
        bad = ctx; bad.missiles_total = 2;
        ct::tick(bad, 0.0f);
        CHECK(fires == 0, "missiles above missiles_max block");
        bad = ctx;
        bad.cargo_units = [](const std::string&) { return 5; };
        ct::tick(bad, 0.0f);
        CHECK(fires == 0, "insufficient cargo blocks");
        bad = ctx; bad.player_pos = HMM_V3(80000.0f, 0.0f, 0.0f);
        ct::tick(bad, 0.0f);
        CHECK(fires == 0, "outside near_nav radius blocks");
        plot::set_flag(test_player, "ambush_troy_seen");
        ct::tick(ctx, 0.0f);
        CHECK(fires == 0, "forbidden flag blocks");
        plot::clear_flag(test_player, "ambush_troy_seen");
        ct::tick(ctx, 0.0f);
        CHECK(fires == 1, "all gates green -> fires");
    }

    {
        std::printf("[case] trigger load: missing file = zero triggers, no throw\n");
        ct::load("._no_such_trigger_file.json");
        CHECK(ct::count() == 0, "missing file -> 0 triggers");
        fires = 0;
        ct::tick(ctx, 0.0f);   // must be a silent no-op
        CHECK(fires == 0, "empty table never fires");
        load_triggers_text("{\"triggers\": [ {\"cinematic\": ");   // truncated
        CHECK(ct::count() == 0, "malformed file -> 0 triggers, no throw");
    }

    // ========================================================================
    // STUDIO REQUEST WRITER (Cinematic Studio Phase B) — docs §3 roundtrips
    // ========================================================================
    namespace sio = cinematic::studio_io;

    // ---- author request: emit -> reparse -> field-for-field; UTF-8 stays raw.
    {
        std::printf("[case] studio: author request roundtrip\n");
        sio::Request r;
        r.id            = "req_1730000000";
        r.kind          = "author";
        r.cinematic_id  = "ambush_troy";
        r.brief         = "Grayson gets ambushed \xe2\x80\x94 leaving Troy";
        r.triggers_text = "flying a tarsus, out of missiles";
        r.outcome_text  = "player ends at Troy Nav 3";
        r.image_quality = "high";
        r.image_style_extra = "harsher rim light";
        const std::string text = sio::request_to_json(r);
        CHECK(text.find("\\u") == std::string::npos,
              "no \\uXXXX escapes \u2014 UTF-8 passes through verbatim");
        CHECK(text.find("\xe2\x80\x94") != std::string::npos,
              "em-dash survives as raw UTF-8 bytes");
        const json::Value v = json::parse(text);
        CHECK(v.is_object(), "emitted request reparses");
        if (v.is_object()) {
            CHECK(v["id"].string_or("") == r.id &&
                  v["kind"].string_or("") == "author", "id + kind roundtrip");
            CHECK(v["brief"].string_or("") == r.brief, "brief roundtrips");
            CHECK(v["image"].is_object() &&
                  v["image"]["quality"].string_or("") == "high" &&
                  v["image"]["style_extra"].string_or("") == "harsher rim light",
                  "image block roundtrips");
            CHECK(!v.contains("line_overrides") && !v.contains("regen"),
                  "empty overrides/regen are OMITTED, not emitted");
        }
    }

    // ---- refine request: only overridden fields per line; regen lists.
    {
        std::printf("[case] studio: refine request roundtrip\n");
        sio::Request r;
        r.id           = "req_1730000001";
        r.kind         = "refine";
        r.cinematic_id = "ambush_troy";
        sio::LineOverride lo;
        lo.index        = 0;
        lo.has_text     = true;  lo.text     = "Wrong lane, Troy-boy.";
        lo.has_emotion  = true;  lo.emotion  = "cold sneer, leaning in";
        lo.has_voice_id = true;  lo.voice_id = "PrivFlightV1501";
        lo.has_speed    = true;  lo.speed    = 0.9f;
        r.line_overrides.push_back(lo);
        r.regen_portraits = { 0, 2 };
        r.regen_voices    = { 0 };
        const json::Value v = json::parse(sio::request_to_json(r));
        CHECK(v.is_object() && v["kind"].string_or("") == "refine",
              "refine request reparses");
        if (v.is_object()) {
            CHECK(!v.contains("brief") && !v.contains("triggers_text"),
                  "empty author-side fields omitted from a refine");
            const json::Value* los = v.find("line_overrides");
            CHECK(los && los->is_array() && los->as_array().size() == 1,
                  "one line override emitted");
            if (los && los->is_array() && !los->as_array().empty()) {
                const json::Value& o = los->as_array()[0];
                CHECK(o["index"].number_or(-1) == 0 &&
                      o["voice_id"].string_or("") == "PrivFlightV1501" &&
                      o["emotion"].string_or("") == "cold sneer, leaning in",
                      "override fields roundtrip");
                CHECK(!o.contains("portrait_prompt_extra") &&
                      !o.contains("seed_image"),
                      "un-overridden fields omitted from the override");
                CHECK(o["speed"].number_or(0) > 0.89 &&
                      o["speed"].number_or(0) < 0.91, "speed roundtrips");
            }
            const json::Value* rg = v.find("regen");
            CHECK(rg && rg->is_object() &&
                  rg->find("portraits") && (*rg)["portraits"].as_array().size() == 2 &&
                  rg->find("voices")    && (*rg)["voices"].as_array().size() == 1,
                  "regen index lists roundtrip");
        }
    }

    // ---- write / scan / respond / delete against a temp studio dir.
    {
        std::printf("[case] studio: write + scan + response join + delete\n");
        const std::string dir = "._test_studio_tmp";
        sio::Request r;
        r.id = "req_42"; r.kind = "author"; r.brief = "a test";
        std::string err;
        CHECK(sio::write_request(r, dir, err), "write_request creates dirs+file");
        auto rows = sio::scan_requests(dir);
        CHECK(rows.size() == 1 && rows[0].id == "req_42" &&
              rows[0].status == "pending", "no response file -> pending");
        // Bridge answers: done + a cinematic id. (Creating responses/ is
        // the bridge's job — the engine only ever creates requests/.)
        {
            std::filesystem::create_directories(dir + "/responses");
            FILE* f = std::fopen((dir + "/responses/req_42.json").c_str(), "wb");
            const char* resp = "{\"id\":\"req_42\",\"status\":\"done\","
                               "\"message\":\"built it\","
                               "\"cinematic_id\":\"test_scene\"}";
            if (f) { std::fwrite(resp, 1, std::strlen(resp), f); std::fclose(f); }
        }
        rows = sio::scan_requests(dir);
        CHECK(rows.size() == 1 && rows[0].status == "done" &&
              rows[0].cinematic_id == "test_scene" &&
              rows[0].message == "built it", "response file joined by id");
        CHECK(sio::delete_request(dir, "req_42"), "delete removes the pair");
        CHECK(sio::scan_requests(dir).empty(), "gone after delete");
        std::remove((dir + "/requests").c_str());
        std::remove((dir + "/responses").c_str());
        std::remove(dir.c_str());
        // seed-image character guess (Lines tab helper) rides along here.
        CHECK(sio::character_from_portrait(
                  "portraits/pirate/ambush_troy_01.png") == "pirate",
              "portrait path -> character guess");
        CHECK(sio::character_from_portrait("nope.png").empty(),
              "no portraits/ prefix -> empty guess");
    }

    std::printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

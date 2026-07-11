// -----------------------------------------------------------------------------
// cinematic_parse.cpp — the engine-free cutscene parser (see header).
//
// The Phase-6.2 hardening lives here: EVERY optional field is read through a
// defaulted, non-throwing accessor. The old code used `v["voice_file"]
// .string_or("")`, but json::Value::operator[] THROWS on a missing key BEFORE
// string_or() can supply its fallback — so an absent optional field (e.g. a
// line without a voice_file, like confrontation_demo's third pirate line)
// threw an uncaught std::runtime_error and could crash the game. The str()/
// num()/flag() helpers below go through find() so a missing key is simply the
// default, and parse_document() is additionally wrapped so any residual throw
// degrades to "log + no-op", never a crash.
// -----------------------------------------------------------------------------

#include "cinematic_parse.h"

#include <algorithm>
#include <cstdio>

namespace cinematic {

// ---- safe, non-throwing field accessors -----------------------------------
// find() returns nullptr for a missing key (or a non-object `v`), so these
// never touch the throwing operator[]. This is the whole ballgame for #1.
namespace {

std::string str(const json::Value& v, const char* key, const std::string& def) {
    const json::Value* f = v.find(key);
    return f ? f->string_or(def) : def;
}
float num(const json::Value& v, const char* key, float def) {
    const json::Value* f = v.find(key);
    return f ? (float)f->number_or(def) : def;
}

bool flag(const json::Value& v, const char* key, bool def) {
    const json::Value* f = v.find(key);
    return f ? f->bool_or(def) : def;
}

} // anonymous namespace

HMM_Vec3 read_vec3(const json::Value& v) {
    HMM_Vec3 out{ 0, 0, 0 };
    if (v.is_array() && v.as_array().size() == 3) {
        // Element access via as_array() (already type-checked) + number_or so
        // a stray string/null inside the triple defaults to 0 instead of
        // throwing.
        const std::vector<json::Value>& a = v.as_array();
        out.X = (float)a[0].number_or(0.0);
        out.Y = (float)a[1].number_or(0.0);
        out.Z = (float)a[2].number_or(0.0);
    }
    return out;
}

void parse_outcome(const json::Value& v, Outcome& out) {
    out = Outcome{};
    if (!v.is_object()) return;   // absent/mistyped block = no outcome, no throw
    out.present       = true;
    out.player_at_nav = str(v, "player_at_nav", "");
    if (const json::Value* pp = v.find("player_pos");
        pp && pp->is_array() && pp->as_array().size() == 3) {
        out.player_pos     = read_vec3(*pp);
        out.has_player_pos = true;
    }
    if (const json::Value* sp = v.find("spawns"); sp && sp->is_array()) {
        for (const json::Value& g : sp->as_array()) {
            if (!g.is_object()) continue;
            OutcomeSpawn s;
            s.klass   = str(g, "class", "");
            if (s.klass.empty()) continue;   // a spawn needs a ship class
            s.faction = str(g, "faction", "civilian");
            s.count   = (int)num(g, "count", 1.0f);
            if (s.count < 1) continue;       // 0 / negative = author says "none"
            s.hostile = flag(g, "hostile", false);
            s.cleared_flag = str(g, "cleared_flag", "");
            out.spawns.push_back(std::move(s));
        }
    }
}

void parse_location(const json::Value& v, Location& out) {
    out = Location{};
    if (!v.is_object()) return;   // absent/mistyped block = no location, no throw
    out.present = true;
    out.system  = str(v, "system", "");
    out.nav     = str(v, "nav", "");
}

bool parse_cue(const json::Value& v, Cue& out) {
    if (!v.is_object() || !v.contains("cmd")) return false;
    out.t   = num(v, "t", 0.0f);
    out.dur = num(v, "dur", 1.0f);   // defaulted; only some cmds use it
    const std::string cmd = str(v, "cmd", "");

    if (cmd == "fade_in")  out.cmd = Cmd::FadeIn;
    else if (cmd == "fade_out")    out.cmd = Cmd::FadeOut;
    else if (cmd == "music")       out.cmd = Cmd::Music;
    else if (cmd == "sfx")         out.cmd = Cmd::Sfx;
    else if (cmd == "camera_path") out.cmd = Cmd::CameraPath;
    else if (cmd == "spawn")       out.cmd = Cmd::Spawn;
    else if (cmd == "actor_path")  out.cmd = Cmd::ActorPath;
    else if (cmd == "line")        out.cmd = Cmd::Line;
    else if (cmd == "subtitle")    out.cmd = Cmd::Subtitle;
    else if (cmd == "end")         out.cmd = Cmd::End;
    else { std::printf("[cinematic] unknown cmd '%s' — skipped\n", cmd.c_str()); return false; }

    switch (out.cmd) {
        case Cmd::Music:
        case Cmd::Sfx:
            out.file = str(v, "file", "");
            if (const json::Value* p = v.find("pos")) { out.pos = read_vec3(*p); out.has_pos = true; }
            break;
        case Cmd::CameraPath: {
            out.ease_smooth = str(v, "ease", "smooth") != "linear";
            out.follow_actor = str(v, "follow", "");
            if (const json::Value* keys = v.find("keys"); keys && keys->is_array())
                for (const json::Value& k : keys->as_array()) {
                    if (!k.is_object()) continue;
                    CamKey ck;
                    if (const json::Value* p = k.find("pos")) ck.pos = read_vec3(*p);
                    if (const json::Value* la = k.find("look_at")) {
                        ck.has_look = true;
                        if (la->is_string()) {
                            const std::string s = la->as_string();
                            if (s.rfind("ship:", 0) == 0) {
                                ck.look_is_ship = true;
                                ck.look_actor   = s.substr(5);
                            }
                        } else {
                            ck.look_pt = read_vec3(*la);
                        }
                    }
                    out.cam_keys.push_back(std::move(ck));
                }
            break;
        }
        case Cmd::Spawn:
            out.actor        = str(v, "actor", "");
            out.klass        = str(v, "class", "");
            out.faction      = str(v, "faction", "civilian");
            out.display_name = str(v, "display_name", "");
            if (const json::Value* p = v.find("pos")) out.pos = read_vec3(*p);
            break;
        case Cmd::ActorPath:
            out.actor = str(v, "actor", "");
            if (const json::Value* keys = v.find("keys"); keys && keys->is_array())
                for (const json::Value& k : keys->as_array())
                    if (const json::Value* p = k.find("pos")) out.actor_keys.push_back(read_vec3(*p));
            break;
        case Cmd::Line:
            out.speaker    = str(v, "speaker", "");
            out.portrait   = str(v, "portrait", "");
            out.text       = str(v, "text", "");
            out.voice_file = str(v, "voice_file", "");   // OPTIONAL — was the crash
            out.side_left  = str(v, "side", "left") != "right";
            break;
        case Cmd::Subtitle:
            out.text = str(v, "text", "");
            break;
        case Cmd::End:
            if (const json::Value* a = v.find("actions"); a && a->is_array())
                for (const json::Value& s : a->as_array())
                    if (s.is_string()) out.actions.push_back(s.as_string());
            break;
        default: break;
    }
    return true;
}

bool parse_document(const json::Value& root, const std::string& default_id,
                    Cinematic& out, std::string& err) {
    out = Cinematic{};
    if (!root.is_object()) {
        err = "cinematic root is not a JSON object";
        return false;
    }
    // Belt-and-suspenders: parse_cue is already non-throwing, but wrap the
    // whole translation so ANY future throw (or a type-mismatch we missed)
    // degrades to a clean parse failure rather than unwinding into the game.
    try {
        out.id        = str(root, "id", default_id);
        out.letterbox = [&] { const json::Value* f = root.find("letterbox");
                              return f ? f->bool_or(true) : true; }();
        out.skippable = [&] { const json::Value* f = root.find("skippable");
                              return f ? f->bool_or(true) : true; }();

        // Optional post-cinematic world state (Studio Phase A2). Missing
        // block = outcome.present stays false; the host applies nothing.
        if (const json::Value* oc = root.find("outcome"))
            parse_outcome(*oc, out.outcome);

        // Optional entry-point location (pre-play teleport). Missing block =
        // location.present stays false; the host plays in place (legacy).
        if (const json::Value* loc = root.find("location"))
            parse_location(*loc, out.location);

        if (const json::Value* tl = root.find("timeline"); tl && tl->is_array()) {
            for (const json::Value& c : tl->as_array()) {
                Cue cue;
                if (parse_cue(c, cue)) out.cues.push_back(std::move(cue));
            }
        }
        // Order-independent authoring: the director evaluates by time.
        std::stable_sort(out.cues.begin(), out.cues.end(),
                         [](const Cue& a, const Cue& b) { return a.t < b.t; });
    } catch (const std::exception& e) {
        err = std::string("cinematic parse threw: ") + e.what();
        out = Cinematic{};
        return false;
    } catch (...) {
        err = "cinematic parse threw (unknown)";
        out = Cinematic{};
        return false;
    }
    out.valid = true;
    return true;
}

} // namespace cinematic

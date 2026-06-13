// -----------------------------------------------------------------------------
// savegame.cpp — JSON (de)serialization of PlayerState to numbered save slots.
//
// See savegame.h for the path choice, slot scheme, versioning + stable-key
// rationale. Two halves here:
//
//   * a tiny JsonWriter — just enough to emit objects/arrays/strings/numbers
//     with correct escaping and human-readable indentation. json.cpp is
//     parse-only by deliberate design (see its header: "NOT in scope:
//     emitting JSON"), so rather than widen that module's contract we keep
//     the writer local to the one caller that needs it. If a second writer
//     consumer ever appears, promote this into json:: then — until then,
//     YAGNI.
//   * save()/load() — the field-by-field marshalling. Reputation goes out as
//     an object keyed by faction NAME (faction::to_name) so an enum reorder
//     never scrambles old saves. credits goes out as a STRING, not a JSON
//     number, because our parser stores numbers as double — fine to ~2^53,
//     but credits is an int64 by design (header) and we promised bit-exact,
//     so a string + strtoll dodges any float rounding entirely.
// -----------------------------------------------------------------------------

#include "savegame.h"

#include "faction.h"
#include "json.h"
#include "player.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace savegame {

namespace {

// ---- tiny JSON writer -------------------------------------------------------
// Builds a pretty-printed JSON document. Tracks whether the current container
// already has a child so it can place commas correctly, and an indent depth
// for readability. Not a general-purpose serializer — exactly the shapes
// PlayerState needs (nested objects, arrays of objects, strings, ints, bools).
struct JsonWriter {
    std::string out;
    int         depth     = 0;
    bool        need_comma = false;   // a sibling precedes the next item

    void indent() { for (int i = 0; i < depth; ++i) out += "  "; }

    // Emit the leading ",\n" / "\n" + indentation before a value/member.
    void pre() {
        if (need_comma) out += ",\n"; else if (!out.empty()) out += "\n";
        indent();
        need_comma = false;
    }

    static std::string escape(const std::string& s) {
        std::string r;
        r.reserve(s.size() + 2);
        for (char c : s) {
            switch (c) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n";  break;
                case '\t': r += "\\t";  break;
                case '\r': r += "\\r";  break;
                default:   r += c;      break;
            }
        }
        return r;
    }

    void begin_object() { pre(); out += "{"; ++depth; need_comma = false; }
    void end_object()   { --depth; out += "\n"; indent(); out += "}"; need_comma = true; }
    void begin_array()  { pre(); out += "["; ++depth; need_comma = false; }
    void end_array()    { --depth; out += "\n"; indent(); out += "]"; need_comma = true; }

    // Named members (inside an object).
    void key(const std::string& k) {
        pre();
        out += "\"" + escape(k) + "\": ";
        // The value that follows is on this same line: suppress pre()'s
        // newline by faking "no comma, non-empty" via a sentinel below.
        suppress_pre = true;
    }
    bool suppress_pre = false;

    void value_raw(const std::string& raw) {
        if (suppress_pre) { suppress_pre = false; } else { pre(); }
        out += raw;
        need_comma = true;
    }
    void value_string(const std::string& s) { value_raw("\"" + escape(s) + "\""); }
    void value_int(long long v)             { value_raw(std::to_string(v)); }
    void value_bool(bool b)                  { value_raw(b ? "true" : "false"); }

    // Object/array openers that respect a pending key (member value).
    void member_object_begin() {
        if (suppress_pre) { suppress_pre = false; out += "{"; }
        else              { pre(); out += "{"; }
        ++depth; need_comma = false;
    }
    void member_array_begin() {
        if (suppress_pre) { suppress_pre = false; out += "["; }
        else              { pre(); out += "["; }
        ++depth; need_comma = false;
    }
};

// HOME-based application support dir. Returns "" if HOME is unset.
std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    return {};
}

} // namespace

std::string saves_dir() {
    const std::string home = home_dir();
    if (home.empty()) {
        std::fprintf(stderr, "[save] HOME unset — cannot locate saves dir\n");
        return {};
    }
    // ~/Library/Application Support/new_privateer/saves — see header.
    fs::path dir = fs::path(home) / "Library" / "Application Support" /
                   "new_privateer" / "saves";
    std::error_code ec;
    fs::create_directories(dir, ec);   // no-op if it already exists
    if (ec) {
        std::fprintf(stderr, "[save] could not create '%s': %s\n",
                     dir.c_str(), ec.message().c_str());
        return {};
    }
    return dir.string();
}

std::string slot_path(int slot) {
    const std::string home = home_dir();
    if (home.empty()) return {};
    fs::path dir = fs::path(home) / "Library" / "Application Support" /
                   "new_privateer" / "saves";
    return (dir / ("save_" + std::to_string(slot) + ".json")).string();
}

// ---- save -------------------------------------------------------------------

bool save(const PlayerState& p, int slot) {
    const std::string dir = saves_dir();   // ensures the tree exists
    if (dir.empty()) return false;

    const long long ts = (long long)std::time(nullptr);

    // Human label for a future load menu: base + credits, e.g.
    // "achilles - 2000 cr". last_docked_base may be empty pre-first-landing.
    char label[160];
    std::snprintf(label, sizeof(label), "%s - %lld cr",
                  p.last_docked_base.empty() ? "deep space" : p.last_docked_base.c_str(),
                  (long long)p.credits);

    JsonWriter w;
    w.begin_object();
      w.key("version");   w.value_int(k_format_version);
      w.key("timestamp"); w.value_int(ts);
      w.key("label");     w.value_string(label);

      w.key("player"); w.member_object_begin();
        // credits as STRING for guaranteed int64 bit-exactness (see header).
        w.key("credits"); w.value_string(std::to_string((long long)p.credits));

        // reputation: object keyed by faction NAME (stable across enum order).
        w.key("rep"); w.member_object_begin();
          for (int i = 0; i < kFactionCount; ++i) {
              w.key(faction::to_name((Faction)i));
              w.value_int((long long)p.rep.rep[i]);
          }
        w.end_object();

        w.key("ship_class_name"); w.value_string(p.ship_class_name);

        w.key("gun_mounts"); w.member_array_begin();
          for (const std::string& g : p.gun_mounts) w.value_string(g);
        w.end_array();

        w.key("shield_level");    w.value_int(p.shield_level);
        w.key("engine_level");    w.value_int(p.engine_level);
        w.key("cargo_expansion"); w.value_bool(p.cargo_expansion);

        w.key("cargo"); w.member_array_begin();
          for (const CargoEntry& e : p.cargo) {
              w.begin_object();
                w.key("commodity_id");    w.value_string(e.commodity_id);
                w.key("units");           w.value_int(e.units);
                w.key("bought_at_price"); w.value_int(e.bought_at_price);
              w.end_object();
          }
        w.end_array();

        // accepted missions (np-zte.1, format v2). Stable string keys; reward
        // as a STRING for the same int64 bit-exactness reason as credits.
        // Only the fields meaningful to each `type` are populated, but we
        // emit them all unconditionally — absent ones default harmlessly on
        // load and the uniform shape keeps the writer simple.
        w.key("missions"); w.member_array_begin();
          for (const ActiveMission& m : p.missions) {
              w.begin_object();
                w.key("id");             w.value_string(m.id);
                w.key("type");           w.value_int(m.type);
                w.key("giver_faction");  w.value_string(m.giver_faction);
                w.key("title");          w.value_string(m.title);
                w.key("reward");         w.value_string(std::to_string((long long)m.reward));
                w.key("commodity_id");   w.value_string(m.commodity_id);
                w.key("units");          w.value_int(m.units);
                w.key("dest_system");    w.value_string(m.dest_system);
                w.key("dest_base");      w.value_string(m.dest_base);
                w.key("target_faction"); w.value_string(m.target_faction);
                w.key("count_required"); w.value_int(m.count_required);
                w.key("progress");       w.value_int(m.progress);
              w.end_object();
          }
        w.end_array();

        // ordnance + fuel (np-zte.2, format v3). Missiles as a fixed-key
        // object (df/hs/ir) so adding a 4th type later doesn't shift array
        // meaning; fuel as a plain number (a float tank reading needs no
        // int64 bit-exactness). Absent on v1/v2 saves -> defaults on load.
        w.key("missiles"); w.member_object_begin();
          w.key("df"); w.value_int(p.missiles[0]);
          w.key("hs"); w.value_int(p.missiles[1]);
          w.key("ir"); w.value_int(p.missiles[2]);
        w.end_object();
        w.key("afterburner_fuel"); w.value_int((long long)(p.afterburner_fuel + 0.5f));

        w.key("current_system");   w.value_string(p.current_system);
        w.key("last_docked_base"); w.value_string(p.last_docked_base);
        w.key("docked");           w.value_bool(p.docked);
      w.end_object();   // player
    w.end_object();     // root
    w.out += "\n";

    // Write to a temp file then rename — a crash mid-write can't clobber an
    // existing good save (POSIX rename is atomic within a filesystem).
    const std::string path = slot_path(slot);
    const std::string tmp  = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::fprintf(stderr, "[save] cannot open '%s' for write\n", tmp.c_str());
            return false;
        }
        f << w.out;
        if (!f.good()) {
            std::fprintf(stderr, "[save] write error on '%s'\n", tmp.c_str());
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        std::fprintf(stderr, "[save] rename '%s' -> '%s' failed: %s\n",
                     tmp.c_str(), path.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }

    std::printf("[save] wrote slot %d -> %s (%lld cr)\n",
                slot, path.c_str(), (long long)p.credits);
    return true;
}

// ---- load -------------------------------------------------------------------

bool load(PlayerState& p, int slot) {
    const std::string path = slot_path(slot);
    if (path.empty()) return false;

    // Missing file is the common, non-error case (no save yet) — quiet-ish.
    if (!fs::exists(fs::path(path))) {
        std::printf("[save] slot %d: no file at %s\n", slot, path.c_str());
        return false;
    }

    // The parser logs its own diagnostics and returns a Null Value on any
    // syntax error, so a truncated/corrupt file lands here as !is_object().
    const json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[save] slot %d: corrupt or unparseable (%s)\n",
                     slot, path.c_str());
        return false;
    }

    // Belt-and-suspenders: typed accessors throw on a type mismatch we didn't
    // guard. Catch everything so a malformed-but-parseable file can't crash.
    try {
        const json::Value* vver = root.find("version");
        const int ver = (vver && vver->is_number()) ? vver->as_int() : 0;
        if (ver <= 0) {
            std::fprintf(stderr, "[save] slot %d: missing/invalid version\n", slot);
            return false;
        }
        if (ver > k_format_version) {
            std::fprintf(stderr, "[save] slot %d: format v%d newer than supported v%d — refusing\n",
                         slot, ver, k_format_version);
            return false;
        }

        const json::Value* pv = root.find("player");
        if (!pv || !pv->is_object()) {
            std::fprintf(stderr, "[save] slot %d: no player object\n", slot);
            return false;
        }
        const json::Value& pl = *pv;

        // Deserialize into a fresh struct, then commit on success so a
        // partial parse never leaves the live PlayerState half-overwritten.
        PlayerState out;

        // credits: string preferred (bit-exact int64); tolerate a legacy
        // number for forward compat with any pre-string saves.
        if (const json::Value* c = pl.find("credits")) {
            if (c->is_string())      out.credits = std::strtoll(c->as_string().c_str(), nullptr, 10);
            else if (c->is_number()) out.credits = (int64_t)c->as_number();
        }

        // reputation: read by faction NAME; missing factions stay zero.
        if (const json::Value* rep = pl.find("rep"); rep && rep->is_object()) {
            for (const auto& [name, val] : rep->as_object()) {
                const Faction f = faction::from_name(name);
                if (f == Faction::Count) continue;            // unknown faction key — skip
                if (!val.is_number())    continue;
                int v = val.as_int();
                if (v < -100) v = -100; if (v > 100) v = 100;  // clamp to rep range
                out.rep.rep[(int)f] = (int8_t)v;
            }
        }

        out.ship_class_name = pl.contains("ship_class_name")
            ? pl["ship_class_name"].string_or("") : "";

        if (const json::Value* gm = pl.find("gun_mounts"); gm && gm->is_array()) {
            for (const json::Value& g : gm->as_array())
                if (g.is_string()) out.gun_mounts.push_back(g.as_string());
        }

        out.shield_level    = pl.contains("shield_level")    ? (int)pl["shield_level"].number_or(0)  : 0;
        out.engine_level    = pl.contains("engine_level")    ? (int)pl["engine_level"].number_or(0)  : 0;
        out.cargo_expansion = pl.contains("cargo_expansion") ? pl["cargo_expansion"].bool_or(false)  : false;

        if (const json::Value* cg = pl.find("cargo"); cg && cg->is_array()) {
            for (const json::Value& e : cg->as_array()) {
                if (!e.is_object()) continue;
                CargoEntry ce;
                ce.commodity_id    = e.contains("commodity_id") ? e["commodity_id"].string_or("") : "";
                ce.units           = e.contains("units")           ? (int)e["units"].number_or(0)           : 0;
                ce.bought_at_price = e.contains("bought_at_price") ? (int)e["bought_at_price"].number_or(0) : 0;
                if (!ce.commodity_id.empty() && ce.units > 0) out.cargo.push_back(std::move(ce));
            }
        }

        // accepted missions (np-zte.1, v2). Absent on a v1 save -> empty list
        // (back-compat). reward read as a string (bit-exact int64), tolerating
        // a legacy number. An entry with no id is skipped defensively.
        if (const json::Value* ms = pl.find("missions"); ms && ms->is_array()) {
            for (const json::Value& e : ms->as_array()) {
                if (!e.is_object()) continue;
                ActiveMission am;
                am.id            = e.contains("id")            ? e["id"].string_or("")            : "";
                am.type          = e.contains("type")          ? (int)e["type"].number_or(0)      : 0;
                am.giver_faction = e.contains("giver_faction") ? e["giver_faction"].string_or("") : "";
                am.title         = e.contains("title")         ? e["title"].string_or("")         : "";
                if (const json::Value* r = e.find("reward")) {
                    if (r->is_string())      am.reward = std::strtoll(r->as_string().c_str(), nullptr, 10);
                    else if (r->is_number()) am.reward = (int64_t)r->as_number();
                }
                am.commodity_id   = e.contains("commodity_id")   ? e["commodity_id"].string_or("")   : "";
                am.units          = e.contains("units")          ? (int)e["units"].number_or(0)          : 0;
                am.dest_system    = e.contains("dest_system")    ? e["dest_system"].string_or("")    : "";
                am.dest_base      = e.contains("dest_base")      ? e["dest_base"].string_or("")      : "";
                am.target_faction = e.contains("target_faction") ? e["target_faction"].string_or("") : "";
                am.count_required = e.contains("count_required") ? (int)e["count_required"].number_or(0) : 0;
                am.progress       = e.contains("progress")       ? (int)e["progress"].number_or(0)       : 0;
                if (!am.id.empty()) out.missions.push_back(std::move(am));
            }
        }

        // ordnance + fuel (np-zte.2, v3). Missiles default to 0 (an old save
        // genuinely had none); fuel defaults to a FULL tank so a v1/v2 save
        // doesn't load grounded with a dry afterburner.
        out.afterburner_fuel = player::k_afterburner_fuel_max;
        if (const json::Value* ms = pl.find("missiles"); ms && ms->is_object()) {
            out.missiles[0] = ms->contains("df") ? (int)(*ms)["df"].number_or(0) : 0;
            out.missiles[1] = ms->contains("hs") ? (int)(*ms)["hs"].number_or(0) : 0;
            out.missiles[2] = ms->contains("ir") ? (int)(*ms)["ir"].number_or(0) : 0;
        }
        if (pl.contains("afterburner_fuel"))
            out.afterburner_fuel = (float)pl["afterburner_fuel"].number_or(player::k_afterburner_fuel_max);

        // Clamp loaded ordnance/fuel to their invariants (#8): a
        // hand-edited save mustn't smuggle negative or over-cap values
        // past the player:: setters, same as rep is clamped above.
        for (int& m : out.missiles) { if (m < 0) m = 0; }
        if (out.afterburner_fuel < 0.0f) out.afterburner_fuel = 0.0f;
        if (out.afterburner_fuel > player::k_afterburner_fuel_max)
            out.afterburner_fuel = player::k_afterburner_fuel_max;

        out.current_system   = pl.contains("current_system")   ? pl["current_system"].string_or("")   : "";
        out.last_docked_base = pl.contains("last_docked_base") ? pl["last_docked_base"].string_or("") : "";
        out.docked           = pl.contains("docked")           ? pl["docked"].bool_or(false)          : false;

        p = std::move(out);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[save] slot %d: deserialize error — %s\n", slot, ex.what());
        return false;
    }

    std::printf("[save] loaded slot %d (%lld cr, system '%s', base '%s')\n",
                slot, (long long)p.credits, p.current_system.c_str(),
                p.last_docked_base.c_str());
    return true;
}

// ---- peek -------------------------------------------------------------------

SlotInfo peek(int slot) {
    SlotInfo info;
    const std::string path = slot_path(slot);
    if (path.empty() || !fs::exists(fs::path(path))) return info;

    const json::Value root = json::parse_file(path);
    if (!root.is_object()) return info;   // corrupt -> exists stays false

    try {
        info.version   = root.contains("version")   ? root["version"].as_int()        : 0;
        info.timestamp = root.contains("timestamp") ? (int64_t)root["timestamp"].number_or(0) : 0;
        info.label     = root.contains("label")     ? root["label"].string_or("")      : "";
        if (const json::Value* pv = root.find("player"); pv && pv->is_object()) {
            const json::Value& pl = *pv;
            info.base   = pl.contains("last_docked_base") ? pl["last_docked_base"].string_or("") : "";
            info.system = pl.contains("current_system")   ? pl["current_system"].string_or("")   : "";
            if (const json::Value* c = pl.find("credits")) {
                if (c->is_string())      info.credits = std::strtoll(c->as_string().c_str(), nullptr, 10);
                else if (c->is_number()) info.credits = (int64_t)c->as_number();
            }
        }
        info.exists = info.version > 0;
    } catch (const std::exception&) {
        info = SlotInfo{};   // anything unexpected -> treat as absent
    }
    return info;
}

} // namespace savegame

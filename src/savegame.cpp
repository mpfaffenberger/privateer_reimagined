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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

// Build the full timestamped title (np-3dp.19):
//   "YYYY-MM-DD HH:MM - <system> - <base> - <ship> - <credits> cr"
// Unset fields read as placeholders ("deep space" base, "?" system/ship).
static std::string make_label(const PlayerState& p, long long ts) {
    char when[32] = "0000-00-00 00:00";   // overwritten below; avoid ?\?- trigraphs
    const std::time_t tt = (std::time_t)ts;
    if (std::tm* lt = std::localtime(&tt))
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", lt);
    char label[256];
    std::snprintf(label, sizeof(label), "%s - %s - %s - %s - %lld cr",
                  when,
                  p.current_system.empty()  ? "?"          : p.current_system.c_str(),
                  p.last_docked_base.empty() ? "deep space" : p.last_docked_base.c_str(),
                  p.ship_class_name.empty()  ? "?"          : p.ship_class_name.c_str(),
                  (long long)p.credits);
    return label;
}

// Serialize `p` into the full save JSON (version + ts + label + player blob).
// Shared by the slot + timestamped save entrypoints.
static std::string serialize_player(const PlayerState& p) {
    const long long ts = (long long)std::time(nullptr);
    const std::string label = make_label(p, ts);

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
        // afterburner_fuel removed (np-zte.2 merged pool). Old keys in v3
        // saves are ignored on load; new saves omit the key entirely.

        // career kills per faction (np-3dp.19, v4). Object keyed by faction
        // NAME (stable across enum reorder, like rep). Counts as STRINGS
        // for int64 bit-exactness.
        w.key("faction_kills"); w.member_object_begin();
          for (int i = 0; i < kFactionCount; ++i) {
              w.key(faction::to_name((Faction)i));
              w.value_string(std::to_string((long long)p.faction_kills[i]));
          }
        w.end_object();

        // live ship-damage snapshot (np-3dp.19, v4). Absent/hp_valid=false
        // -> the loaded ship stays at full health.
        w.key("ship_health"); w.member_object_begin();
          w.key("valid");       w.value_bool(p.hp_valid);
          w.key("armor_fore");  w.value_raw(std::to_string(p.hp_armor_fore));
          w.key("armor_aft");   w.value_raw(std::to_string(p.hp_armor_aft));
          w.key("armor_side");  w.value_raw(std::to_string(p.hp_armor_side));
          w.key("shield_fore"); w.value_raw(std::to_string(p.hp_shield_fore));
          w.key("shield_aft");  w.value_raw(std::to_string(p.hp_shield_aft));
          w.key("shield_side"); w.value_raw(std::to_string(p.hp_shield_side));
          w.key("energy");      w.value_raw(std::to_string(p.hp_energy));
        w.end_object();

        w.key("current_system");   w.value_string(p.current_system);
        w.key("last_docked_base"); w.value_string(p.last_docked_base);
        w.key("docked");           w.value_bool(p.docked);
      w.end_object();   // player
    w.end_object();     // root
    w.out += "\n";
    return w.out;
}

// Atomic write: temp file then rename (POSIX rename is atomic within a
// filesystem), so a crash mid-write can't clobber an existing good save.
static bool write_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::fprintf(stderr, "[save] cannot open '%s' for write\n", tmp.c_str());
            return false;
        }
        f << content;
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
    return true;
}

bool save(const PlayerState& p, int slot) {
    if (saves_dir().empty()) return false;   // ensures the tree exists
    const std::string path = slot_path(slot);
    if (!write_atomic(path, serialize_player(p))) return false;
    std::printf("[save] wrote slot %d -> %s (%lld cr)\n",
                slot, path.c_str(), (long long)p.credits);
    return true;
}

std::string save_timestamped(const PlayerState& p) {
    const std::string dir = saves_dir();   // ensures the tree exists
    if (dir.empty()) return {};
    // Unique filename from nanoseconds-since-epoch: saves never collide and
    // never overwrite — unbounded accumulation by design (np-3dp.19).
    const long long nanos = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string path =
        (fs::path(dir) / ("save_" + std::to_string(nanos) + ".json")).string();
    if (!write_atomic(path, serialize_player(p))) return {};
    std::printf("[save] wrote %s (%lld cr)\n", path.c_str(), (long long)p.credits);
    return path;
}

// ---- load -------------------------------------------------------------------

bool load(PlayerState& p, int slot) {
    const std::string path = slot_path(slot);
    if (path.empty()) return false;
    return load(p, path);
}

bool load(PlayerState& p, const std::string& path) {
    if (path.empty()) return false;
    const int slot = -1;   // path-based load; the slot # is only for logs

    // Missing file is the common, non-error case (no save yet) — quiet-ish.
    if (!fs::exists(fs::path(path))) {
        std::printf("[save] no file at %s\n", path.c_str());
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

        // ordnance (np-zte.2, v3). Missiles default to 0 (an old save
        // genuinely had none). The legacy `afterburner_fuel` key is no
        // longer a PlayerState field — the fuel pool merged into the
        // player Ship's energy_gj. We tolerate the key in older saves by
        // just ignoring it; new saves don't emit it.
        if (const json::Value* ms = pl.find("missiles"); ms && ms->is_object()) {
            out.missiles[0] = ms->contains("df") ? (int)(*ms)["df"].number_or(0) : 0;
            out.missiles[1] = ms->contains("hs") ? (int)(*ms)["hs"].number_or(0) : 0;
            out.missiles[2] = ms->contains("ir") ? (int)(*ms)["ir"].number_or(0) : 0;
        }

        // Clamp loaded ordnance to its invariants (#8): a hand-edited
        // save mustn't smuggle negative values past the player:: setters,
        // same as rep is clamped above.
        for (int& m : out.missiles) { if (m < 0) m = 0; }

        // career kills per faction (np-3dp.19, v4). Read by faction NAME;
        // missing keys / older saves stay 0. STRING preferred (bit-exact),
        // legacy number tolerated.
        if (const json::Value* fk = pl.find("faction_kills"); fk && fk->is_object()) {
            for (const auto& [name, val] : fk->as_object()) {
                const Faction f = faction::from_name(name);
                if (f == Faction::Count) continue;
                int64_t v = 0;
                if (val.is_string())      v = std::strtoll(val.as_string().c_str(), nullptr, 10);
                else if (val.is_number()) v = (int64_t)val.as_number();
                if (v < 0) v = 0;
                out.faction_kills[(int)f] = v;
            }
        }

        // live ship-damage snapshot (np-3dp.19, v4). hp_valid gates whether
        // it's applied to the spawned ship; absent on older saves -> false.
        if (const json::Value* sh = pl.find("ship_health"); sh && sh->is_object()) {
            const json::Value& h = *sh;
            out.hp_valid       = h.contains("valid")       ? h["valid"].bool_or(false)            : false;
            out.hp_armor_fore  = h.contains("armor_fore")  ? (float)h["armor_fore"].number_or(0)  : 0.0f;
            out.hp_armor_aft   = h.contains("armor_aft")   ? (float)h["armor_aft"].number_or(0)   : 0.0f;
            out.hp_armor_side  = h.contains("armor_side")  ? (float)h["armor_side"].number_or(0)  : 0.0f;
            out.hp_shield_fore = h.contains("shield_fore") ? (float)h["shield_fore"].number_or(0) : 0.0f;
            out.hp_shield_aft  = h.contains("shield_aft")  ? (float)h["shield_aft"].number_or(0)  : 0.0f;
            out.hp_shield_side = h.contains("shield_side") ? (float)h["shield_side"].number_or(0) : 0.0f;
            out.hp_energy      = h.contains("energy")      ? (float)h["energy"].number_or(0)      : 0.0f;
        }

        out.current_system   = pl.contains("current_system")   ? pl["current_system"].string_or("")   : "";
        out.last_docked_base = pl.contains("last_docked_base") ? pl["last_docked_base"].string_or("") : "";
        out.docked           = pl.contains("docked")           ? pl["docked"].bool_or(false)          : false;

        p = std::move(out);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[save] slot %d: deserialize error — %s\n", slot, ex.what());
        return false;
    }

    std::printf("[save] loaded %s (%lld cr, system '%s', base '%s')\n",
                path.c_str(), (long long)p.credits, p.current_system.c_str(),
                p.last_docked_base.c_str());
    return true;
}

// ---- peek -------------------------------------------------------------------

SlotInfo peek_path(const std::string& path) {
    SlotInfo info;
    if (path.empty() || !fs::exists(fs::path(path))) return info;
    info.path = path;

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
            info.ship   = pl.contains("ship_class_name")  ? pl["ship_class_name"].string_or("")  : "";
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

SlotInfo peek(int slot) {
    const std::string path = slot_path(slot);
    if (path.empty()) return SlotInfo{};
    return peek_path(path);
}

// ---- list -------------------------------------------------------------------

std::vector<SlotInfo> list_saves() {
    std::vector<SlotInfo> out;
    const std::string dir = saves_dir();
    if (dir.empty()) return out;

    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(fs::path(dir), ec)) {
        if (ec) break;
        if (!ent.is_regular_file()) continue;
        const std::string fname = ent.path().filename().string();
        // Only our save files; skip the .tmp scratch + anything else.
        if (fname.rfind("save_", 0) != 0) continue;
        if (ent.path().extension() != ".json") continue;
        SlotInfo info = peek_path(ent.path().string());
        if (info.exists) out.push_back(std::move(info));
    }
    // Newest first (by embedded unix timestamp; ties broken by path so the
    // order is stable).
    std::sort(out.begin(), out.end(), [](const SlotInfo& a, const SlotInfo& b) {
        if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
        return a.path > b.path;
    });
    return out;
}

} // namespace savegame
// 1781715641488072000

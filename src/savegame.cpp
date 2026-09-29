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
#include "inventory.h"
#include "json.h"
#include "player.h"
#include "repair.h"
#include "ship_class.h"
#include "world_clock.h"

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

// On-disk key per missile rack slot, indexed like PlayerState::missiles.
// A persistence contract: append new types, never rename or reorder.
constexpr const char* k_missile_save_keys[k_missile_rack_types] = {
    "df", "hs", "ir", "ff"
};

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

// Platform-appropriate per-user data dir. On macOS this is
// `~/Library/Application Support/`; on Windows it's `%APPDATA%` (typically
// `C:\Users\<user>\AppData\Roaming`); on Linux it's `$XDG_DATA_HOME` or
// `~/.local/share/`. Falls back to `$HOME` (Unix) or `%USERPROFILE%` (Win)
// only when the proper override isn't set.
//
// Returns "" if no usable path can be found (degenerate; caller logs).
//
// NP_DATA_DIR overrides the platform default entirely. Test harnesses use it
// to keep scratch/corrupt fixture saves out of the player's real save picker
// (#242); it also lets a portable install carry its saves alongside the game.
std::string user_data_dir() {
    if (const char* o = std::getenv("NP_DATA_DIR"); o && *o) {
        return (fs::path(o) / "new_privateer").string();
    }
#ifdef _WIN32
    // Prefer APPDATA — that's where Windows apps are expected to stash
    // mutable per-user state. %USERPROFILE% is a last-resort fallback.
    if (const char* a = std::getenv("APPDATA"); a && *a) {
        fs::path p = fs::path(a);
        p /= "new_privateer";
        return p.string();
    }
    if (const char* u = std::getenv("USERPROFILE"); u && *u) {
        fs::path p = fs::path(u);
        p /= "AppData";
        p /= "Roaming";
        p /= "new_privateer";
        return p.string();
    }
    return {};
#else
    // macOS: $HOME/Library/Application Support/new_privateer
    // Linux: $XDG_DATA_HOME/new_privateer  or  $HOME/.local/share/new_privateer
    if (const char* h = std::getenv("HOME"); h && *h) {
        fs::path p = fs::path(h);
#ifdef __APPLE__
        p /= "Library";
        p /= "Application Support";
        p /= "new_privateer";
        return p.string();
#else
        if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
            fs::path px = fs::path(x);
            px /= "new_privateer";
            return px.string();
        }
        p /= ".local";
        p /= "share";
        p /= "new_privateer";
        return p.string();
#endif
    }
    return {};
#endif
}

// Pre-v10 saves predate turret hardware (#145): back then every hull came
// with its turret mounts, so treat a turret slot as owned iff one of its
// mounts already carries a gun. Nothing the player paid for goes inert.
void grandfather_turrets(PlayerState& p) {
    const ShipClass* klass = ship_class::find(p.ship_class_name);
    if (!klass) return;
    for (const TurretSlot& t : klass->turret_slots) {
        const bool armed = std::any_of(t.mounts.begin(), t.mounts.end(), [&](int m) {
            return m < (int)p.gun_mounts.size() && !p.gun_mounts[(size_t)m].gun_id.empty();
        });
        if (armed) p.turrets.push_back(t.id);
    }
}

} // namespace

std::string saves_dir() {
    std::string base = user_data_dir();
    if (base.empty()) {
        std::fprintf(stderr, "[save] no user data dir (HOME/APPDATA unset) — cannot locate saves dir\n");
        return {};
    }
    fs::path dir = fs::path(base) / "saves";
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
    std::string base = user_data_dir();
    if (base.empty()) return {};
    fs::path dir = fs::path(base) / "saves";
    return (dir / ("save_" + std::to_string(slot) + ".json")).string();
}

// ---- save -------------------------------------------------------------------

// Build the full timestamped title (np-3dp.19):
//   "YYYY-MM-DD HH:MM - <stardate> - <system> - <base> - <ship> - <credits> cr"
// Unset fields read as placeholders ("deep space" base, "?" system/ship).
static std::string make_label(const PlayerState& p, long long ts) {
    char when[32] = "0000-00-00 00:00";   // overwritten below; avoid ?\?- trigraphs
    const std::time_t tt = (std::time_t)ts;
    if (std::tm* lt = std::localtime(&tt))
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", lt);
    char label[288];
    const std::string stardate = world_clock::stardate_string(p.day);
    std::snprintf(label, sizeof(label), "%s - %s - %s - %s - %s - %lld cr",
                  when, stardate.c_str(),
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

        // gun_mounts (Phase 4d Wave 1, #88/#89). Each mount is now an OBJECT
        // carrying gun_id + rarity + per-shot mods, mirroring the items array
        // above. Old saves wrote bare strings; the loader still accepts those
        // (is_string -> Basic) for back-compat.
        w.key("gun_mounts"); w.member_array_begin();
          for (const MountSlot& m : p.gun_mounts) {
              w.begin_object();
                w.key("gun_id");          w.value_string(m.gun_id);
                w.key("rarity");          w.value_int((int)m.rarity);
                w.key("fire_rate_mult");  w.value_raw(std::to_string(m.mods.fire_rate_mult));
                w.key("energy_mult");     w.value_raw(std::to_string(m.mods.energy_mult));
              w.end_object();
          }
        w.end_array();

        w.key("shield_level");    w.value_int(p.shield_level);
        w.key("engine_level");    w.value_int(p.engine_level);
        w.key("armor_name");      w.value_string(p.armor_name);
        w.key("cargo_expansion"); w.value_bool(p.cargo_expansion);
        // fitted scanner (#143, v9). "" = none fitted (sold).
        w.key("scanner_id");      w.value_string(p.scanner_id);
        // turret hardware (#145, v10): owned TurretSlot ids.
        w.key("turrets"); w.member_array_begin();
          for (const std::string& t : p.turrets) w.value_string(t);
        w.end_array();
        w.key("has_jump_drive");   w.value_bool(p.has_jump_drive);
        w.key("ecm_level");        w.value_int(p.ecm_level);
        w.key("has_repair_droid"); w.value_bool(p.has_repair_droid);
        w.key("adv_repair_droid"); w.value_bool(p.adv_repair_droid);
        w.key("has_tractor_beam"); w.value_bool(p.has_tractor_beam);
        // guild memberships (#16, v6). Absent on older saves -> false.
        w.key("merc_guild_member");     w.value_bool(p.merc_guild_member);
        w.key("merchant_guild_member"); w.value_bool(p.merchant_guild_member);

        // campaign plot state (#138, v7). Two flat string arrays — see
        // plot.h for the naming conventions. Absent on older saves ->
        // empty == campaign not started.
        w.key("plot_flags"); w.member_array_begin();
          for (const std::string& s : p.plot_flags) w.value_string(s);
        w.end_array();
        w.key("plot_items"); w.member_array_begin();
          for (const std::string& s : p.plot_items) w.value_string(s);
        w.end_array();

        w.key("cargo"); w.member_array_begin();
          for (const CargoEntry& e : p.cargo) {
              w.begin_object();
                w.key("commodity_id");    w.value_string(e.commodity_id);
                w.key("units");           w.value_int(e.units);
                w.key("bought_at_price"); w.value_int(e.bought_at_price);
                // Secret-compartment stow (#116). Written unconditionally;
                // absent on older saves -> false (visible hold stack).
                w.key("hidden");          w.value_bool(e.hidden);
              w.end_object();
          }
        w.end_array();

        // unified-hold items (Phase 4 Wave 1, #80 + #81). Discrete
        // things dropped, bought, or salvaged into the hold. Stable
        // string keys for every field; enums are written as ints so a
        // reorder of the ItemKind / Rarity enum values can't scramble
        // old saves. Float mods use the same std::to_string form as
        // ship_health below (default precision; 1.0 round-trips fine).
        w.key("items"); w.member_array_begin();
          for (const inventory::InventoryItem& it : p.items) {
              w.begin_object();
                w.key("id");              w.value_string(it.id);
                w.key("kind");            w.value_int((int)it.kind);
                w.key("rarity");          w.value_int((int)it.rarity);
                w.key("qty");             w.value_int(it.qty);
                w.key("fire_rate_mult");  w.value_raw(std::to_string(it.mods.fire_rate_mult));
                w.key("energy_mult");     w.value_raw(std::to_string(it.mods.energy_mult));
              w.end_object();
          }
        w.end_array();

        // permanent upgrades (Phase 4e, #92). Installed Upgrade items reborn
        // as permanent effects. Stable string keys for id/effect; value is a
        // float written with the same std::to_string form as ship_health
        // below. Absent on older saves -> empty list (back-compat).
        w.key("permanent_mods"); w.member_array_begin();
          for (const PermanentMod& pm : p.permanent_mods) {
              w.begin_object();
                w.key("id");     w.value_string(pm.id);
                w.key("effect"); w.value_string(pm.effect);
                w.key("value");  w.value_raw(std::to_string(pm.value));
              w.end_object();
          }
        w.end_array();

        // accepted missions (np-zte.1, format v2). Stable string keys; reward
        // as a STRING for the same int64 bit-exactness reason as credits.
        // Only the fields meaningful to each `type` are populated, but we
        // emit them all unconditionally — absent ones default harmlessly on
        // load and the uniform shape keeps the writer simple.
        //
        // (#8, format v5) the per-mission payload grew: `source`, the
        // non-cargo `target_system`, the parallel `nav_targets` array,
        // `nav_count`, the attack `hostiles_required`, the defend
        // `target_base`, the bounty's `bounty_region` and
        // `last_seen(_alt)?_system`. Same flat-shape policy — emit all,
        // load tolerates missing keys.
        w.key("missions"); w.member_array_begin();
          for (const ActiveMission& m : p.missions) {
              w.begin_object();
                w.key("id");             w.value_string(m.id);
                w.key("type");           w.value_int(m.type);
                w.key("source");         w.value_int(m.source);
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
                w.key("target_system");  w.value_string(m.target_system);
                w.key("target_base");    w.value_string(m.target_base);
                w.key("last_seen_system");     w.value_string(m.last_seen_system);
                w.key("last_seen_alt_system"); w.value_string(m.last_seen_alt_system);
                w.key("nav_count");      w.value_int(m.nav_count);
                w.key("hostiles_required"); w.value_int(m.hostiles_required);
                // Vector<string> payloads (nav_targets + bounty_region).
                w.key("nav_targets"); w.member_array_begin();
                  for (const std::string& s : m.nav_targets)   w.value_string(s);
                w.end_array();
                w.key("bounty_region"); w.member_array_begin();
                  for (const std::string& s : m.bounty_region) w.value_string(s);
                w.end_array();
                // Live in-flight progress (#12/#13): per-nav reached flags, so
                // mid-mission survey progress survives save/reload. Written as
                // 0/1 ints; absent on older saves -> resized empty on load.
                w.key("nav_done"); w.member_array_begin();
                  for (uint8_t d : m.nav_done) w.value_int(d ? 1 : 0);
                w.end_array();
              w.end_object();
          }
        w.end_array();

        // ordnance + fuel (np-zte.2, format v3). Missiles as a fixed-key
        // object (df/hs/ir) so adding a 4th type later doesn't shift array
        // meaning; fuel as a plain number (a float tank reading needs no
        // int64 bit-exactness). Absent on v1/v2 saves -> defaults on load.
        // FF (#144) joined as "ff" — that's the payoff of fixed keys.
        w.key("missiles"); w.member_object_begin();
        for (int i = 0; i < k_missile_rack_types; ++i) {
            w.key(k_missile_save_keys[i]); w.value_int(p.missiles[i]);
        }
        w.end_object();
        // Torpedo rack (np-zte.2 expansion, np-9cu-launchers-bump).
        // Single counter now (was a DF/HS/IR fan-out; that was overkill
        // since Privateer canon has exactly one torpedo ammo type).
        w.key("torpedoes"); w.value_int(p.torpedoes);
        // Per-side hardware slots (left + right for each ammo type).
        // Four booleans; new-save format. Old saves that used the integer
        // missile_launchers_owned / torpedo_tubes_owned keys get migrated
        // on load (see the read half below).
        w.key("missile_launcher_left");   w.value_bool(p.missile_launcher_left);
        w.key("missile_launcher_right");  w.value_bool(p.missile_launcher_right);
        w.key("torpedo_launcher_left");   w.value_bool(p.torpedo_launcher_left);
        w.key("torpedo_launcher_right");  w.value_bool(p.torpedo_launcher_right);
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
          w.key("valid");          w.value_bool(p.hp_valid);
          w.key("armor_fore");     w.value_raw(std::to_string(p.hp_armor_fore));
          w.key("armor_aft");      w.value_raw(std::to_string(p.hp_armor_aft));
          w.key("armor_port");     w.value_raw(std::to_string(p.hp_armor_port));
          w.key("armor_starboard"); w.value_raw(std::to_string(p.hp_armor_starboard));
          w.key("shield_fore");    w.value_raw(std::to_string(p.hp_shield_fore));
          w.key("shield_aft");     w.value_raw(std::to_string(p.hp_shield_aft));
          w.key("shield_port");    w.value_raw(std::to_string(p.hp_shield_port));
          w.key("shield_starboard"); w.value_raw(std::to_string(p.hp_shield_starboard));
          w.key("energy");         w.value_raw(std::to_string(p.hp_energy));
        w.end_object();

        // Persistent world clock (Gemini Lives #171, format v8).
        w.key("day");              w.value_int(p.day);
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

        // gun_mounts (Phase 4d Wave 1, #88/#89). Back-compat: old saves
        // stored bare strings (-> Basic rarity, 1.0/1.0 mods); new saves
        // store objects with gun_id/rarity/mods. Empty/missing -> skip.
        if (const json::Value* gm = pl.find("gun_mounts"); gm && gm->is_array()) {
            for (const json::Value& g : gm->as_array()) {
                if (g.is_string()) {
                    out.gun_mounts.push_back(MountSlot{g.as_string(), inventory::Rarity::Basic});
                } else if (g.is_object()) {
                    MountSlot m;
                    m.gun_id = g.contains("gun_id") ? g["gun_id"].string_or("") : "";
                    m.rarity = g.contains("rarity")
                        ? (inventory::Rarity)(int)g["rarity"].number_or(0)
                        : inventory::Rarity::Basic;
                    m.mods.fire_rate_mult = g.contains("fire_rate_mult")
                        ? (float)g["fire_rate_mult"].number_or(1.0) : 1.0f;
                    m.mods.energy_mult = g.contains("energy_mult")
                        ? (float)g["energy_mult"].number_or(1.0) : 1.0f;
                    out.gun_mounts.push_back(std::move(m));
                }
            }
        }

        out.shield_level    = pl.contains("shield_level")    ? (int)pl["shield_level"].number_or(0)  : 0;
        out.engine_level    = pl.contains("engine_level")    ? (int)pl["engine_level"].number_or(0)  : 0;
        out.armor_name      = pl.contains("armor_name")      ? pl["armor_name"].string_or("")       : "";
        out.cargo_expansion = pl.contains("cargo_expansion") ? pl["cargo_expansion"].bool_or(false)  : false;
        // scanner (#143, v9). A pre-v9 save never had one to lose, so it gets
        // the new-game scanner; a present "" means the pilot sold it.
        out.scanner_id      = pl.contains("scanner_id")      ? pl["scanner_id"].string_or("")
                                                             : player::k_starting_scanner;
        // turret hardware (#145, v10). Pre-v10 saves have no key: grandfather
        // every turret slot that already carries a fitted gun.
        if (const json::Value* tv = pl.find("turrets"); tv && tv->is_array()) {
            for (const json::Value& t : tv->as_array())
                if (t.is_string()) out.turrets.push_back(t.as_string());
        } else {
            grandfather_turrets(out);
        }
        out.has_jump_drive   = pl.contains("has_jump_drive")   ? pl["has_jump_drive"].bool_or(false)   : false;
        out.ecm_level        = pl.contains("ecm_level")        ? (int)pl["ecm_level"].number_or(0)    : 0;
        out.has_repair_droid = pl.contains("has_repair_droid") ? pl["has_repair_droid"].bool_or(false) : false;
        out.adv_repair_droid = pl.contains("adv_repair_droid") ? pl["adv_repair_droid"].bool_or(false) : false;
        out.has_tractor_beam = pl.contains("has_tractor_beam") ? pl["has_tractor_beam"].bool_or(false) : false;
        // guild memberships (#16, v6). Older saves default to non-member.
        out.merc_guild_member     = pl.contains("merc_guild_member")     ? pl["merc_guild_member"].bool_or(false)     : false;

    // campaign plot state (#138, v7). Missing on pre-v7 saves -> both
    // lists stay empty (campaign not started). Non-string entries are
    // skipped, same tolerance as nav_targets below.
    if (const json::Value* pf = pl.find("plot_flags"); pf && pf->is_array()) {
        for (const json::Value& g : pf->as_array())
            if (g.is_string()) out.plot_flags.push_back(g.as_string());
    }
    if (const json::Value* pi = pl.find("plot_items"); pi && pi->is_array()) {
        for (const json::Value& g : pi->as_array())
            if (g.is_string()) out.plot_items.push_back(g.as_string());
    }
        out.merchant_guild_member = pl.contains("merchant_guild_member") ? pl["merchant_guild_member"].bool_or(false) : false;

        if (const json::Value* cg = pl.find("cargo"); cg && cg->is_array()) {
            for (const json::Value& e : cg->as_array()) {
                if (!e.is_object()) continue;
                CargoEntry ce;
                ce.commodity_id    = e.contains("commodity_id") ? e["commodity_id"].string_or("") : "";
                ce.units           = e.contains("units")           ? (int)e["units"].number_or(0)           : 0;
                ce.bought_at_price = e.contains("bought_at_price") ? (int)e["bought_at_price"].number_or(0) : 0;
                ce.hidden          = e.contains("hidden")          ? e["hidden"].bool_or(false)             : false;
                if (!ce.commodity_id.empty() && ce.units > 0) out.cargo.push_back(std::move(ce));
            }
        }

        // unified-hold items (Phase 4 Wave 1, #80 + #81). Absent on
        // older saves -> empty list (back-compat with no-items era).
        // Enums read back as int via static_cast; mirrors the per-
        // faction int-key pattern already used here.
        if (const json::Value* it = pl.find("items"); it && it->is_array()) {
            for (const json::Value& e : it->as_array()) {
                if (!e.is_object()) continue;
                inventory::InventoryItem im;
                im.id     = e.contains("id")     ? e["id"].string_or("")  : "";
                im.kind   = e.contains("kind")   ? (inventory::ItemKind)(int)e["kind"].number_or(0)   : inventory::ItemKind::Salvage;
                im.rarity = e.contains("rarity") ? (inventory::Rarity)(int)e["rarity"].number_or(0)   : inventory::Rarity::Basic;
                im.qty    = e.contains("qty")    ? (int)e["qty"].number_or(0) : 0;
                im.mods.fire_rate_mult = e.contains("fire_rate_mult") ? (float)e["fire_rate_mult"].number_or(1.0) : 1.0f;
                im.mods.energy_mult    = e.contains("energy_mult")    ? (float)e["energy_mult"].number_or(1.0)    : 1.0f;
                // Skip defensive: empty id or non-positive qty means a
                // hand-edited or corrupt entry -- drop instead of push.
                if (!im.id.empty() && im.qty > 0) out.items.push_back(std::move(im));
            }
        }

        // permanent upgrades (Phase 4e, #92). Absent on older saves ->
        // empty list (back-compat with the pre-upgrade era). id/effect as
        // strings, value as a float; an entry with no id is skipped.
        if (const json::Value* pm = pl.find("permanent_mods"); pm && pm->is_array()) {
            for (const json::Value& e : pm->as_array()) {
                if (!e.is_object()) continue;
                PermanentMod m;
                m.id     = e.contains("id")     ? e["id"].string_or("")     : "";
                m.effect = e.contains("effect") ? e["effect"].string_or("") : "";
                m.value  = e.contains("value")  ? (float)e["value"].number_or(0.0) : 0.0f;
                if (!m.id.empty()) out.permanent_mods.push_back(std::move(m));
            }
        }

        // accepted missions (np-zte.1, v2). Absent on a v1 save -> empty list
        // (back-compat). reward read as a string (bit-exact int64), tolerating
        // a legacy number. An entry with no id is skipped defensively.
        //
        // (#8, v5) the per-mission payload grew (see writer block). Old
        // saves from before #6's MissionType renumber carry `type` values
        // that no longer correspond to any legal mission — drop them and
        // log one line so a hand-edited or pre-#6 save can't silently
        // invent a bogus mission. Also drop any `type` outside [0,5].
        if (const json::Value* ms = pl.find("missions"); ms && ms->is_array()) {
            for (const json::Value& e : ms->as_array()) {
                if (!e.is_object()) continue;
                ActiveMission am;
                am.id            = e.contains("id")            ? e["id"].string_or("")            : "";
                am.type          = e.contains("type")          ? (int)e["type"].number_or(0)      : 0;
                // (#9,#11) Pre-v5 saves used the OLD 2-value mission enum
                // (0=CargoDelivery, 1=Bounty); remap to the current 0..5
                // numbering (now 4=Bounty, 5=CargoDelivery; see MissionType
                // in missions.h). Other values in old saves pass through to
                // the existing 0..5 range check, which drop+logs them.
                if (ver < 5) {
                    if      (am.type == 0) am.type = 5;   // old CargoDelivery -> CargoDelivery
                    else if (am.type == 1) am.type = 4;   // old Bounty        -> Bounty
                }
                if (am.type < 0 || am.type > 5) {
                    std::fprintf(stderr,
                        "[save] slot %d: dropping mission '%s' with out-of-range type %d\n",
                        slot, am.id.c_str(), am.type);
                    continue;
                }
                am.source        = e.contains("source")        ? (int)e["source"].number_or(0)    : 0;
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
                am.target_system  = e.contains("target_system")  ? e["target_system"].string_or("")  : "";
                am.target_base    = e.contains("target_base")    ? e["target_base"].string_or("")    : "";
                am.last_seen_system     = e.contains("last_seen_system")     ? e["last_seen_system"].string_or("")     : "";
                am.last_seen_alt_system = e.contains("last_seen_alt_system") ? e["last_seen_alt_system"].string_or("") : "";
                am.nav_count      = e.contains("nav_count")      ? (int)e["nav_count"].number_or(0)         : 0;
                am.hostiles_required = e.contains("hostiles_required") ? (int)e["hostiles_required"].number_or(0) : 0;
                if (const json::Value* nt = e.find("nav_targets"); nt && nt->is_array()) {
                    for (const json::Value& g : nt->as_array())
                        if (g.is_string()) am.nav_targets.push_back(g.as_string());
                }
                if (const json::Value* br = e.find("bounty_region"); br && br->is_array()) {
                    for (const json::Value& g : br->as_array())
                        if (g.is_string()) am.bounty_region.push_back(g.as_string());
                }
                // Live in-flight progress (#12/#13). Absent on older saves ->
                // stays empty here, then normalized to nav_targets length below
                // so the #13 tracker always has a flag per nav target.
                if (const json::Value* nd = e.find("nav_done"); nd && nd->is_array()) {
                    for (const json::Value& g : nd->as_array())
                        am.nav_done.push_back((uint8_t)(g.number_or(0) != 0 ? 1 : 0));
                }
                if (am.nav_done.size() != am.nav_targets.size())
                    am.nav_done.resize(am.nav_targets.size(), (uint8_t)0);
                if (!am.id.empty()) out.missions.push_back(std::move(am));
            }
        }

        // ordnance (np-zte.2, v3). Missiles default to 0 (an old save
        // genuinely had none). The legacy `afterburner_fuel` key is no
        // longer a PlayerState field — the fuel pool merged into the
        // player Ship's energy_gj. We tolerate the key in older saves by
        // just ignoring it; new saves don't emit it.
        // A key missing from an older save (e.g. "ff" pre-#144) reads 0.
        if (const json::Value* ms = pl.find("missiles"); ms && ms->is_object()) {
            for (int i = 0; i < k_missile_rack_types; ++i) {
                const char* key = k_missile_save_keys[i];
                out.missiles[i] = ms->contains(key) ? (int)(*ms)[key].number_or(0) : 0;
            }
        }
        // Torpedo rack (np-zte.2, np-launchers-bump). Absent on v1/v2
        // saves -> defaults to 0, which is the correct empty state. v1/v2
        // saves that stored torpedoes as a {df,hs,ir} object also read OK
        // (we sum the three counts together).
        if (const json::Value* ts = pl.find("torpedoes"); ts) {
            if (ts->is_object()) {
                int sum = 0;
                for (const char* k : {"df", "hs", "ir"}) {
                    if (ts->contains(k)) sum += (int)(*ts)[k].number_or(0);
                }
                out.torpedoes = sum;
            } else {
                out.torpedoes = std::max(0, (int)ts->number_or(0));
            }
        }
        // Launcher hardware (np-zte.2, np-launchers-bump).
        // Each side is its own bool. New keys: missile_launcher_left,
        // missile_launcher_right, torpedo_launcher_left, torpedo_launcher_right.
        // Old format (missile_launchers_owned / torpedo_tubes_owned as ints)
        // gets migrated: count == 1 -> fill the LEFT, count == 2 -> fill both.
        // Defaults match the new-game loadout: missile LEFT on, others off.
        auto fill_missile = [&](int n_owned) {
            out.missile_launcher_left  = n_owned >= 1;
            out.missile_launcher_right = n_owned >= 2;
        };
        auto fill_torpedo = [&](int n_owned) {
            out.torpedo_launcher_left  = n_owned >= 1;
            out.torpedo_launcher_right = n_owned >= 2;
        };
        bool got_ml = false, got_tl = false;
        if (const json::Value* ml = pl.find("missile_launcher_left"); ml) {
            out.missile_launcher_left  = ml->bool_or(false);
            out.missile_launcher_right = pl.find("missile_launcher_right")->bool_or(false);
            got_ml = true;
        }
        if (const json::Value* tl = pl.find("torpedo_launcher_left"); tl) {
            out.torpedo_launcher_left  = tl->bool_or(false);
            out.torpedo_launcher_right = pl.find("torpedo_launcher_right")->bool_or(false);
            got_tl = true;
        }
        if (!got_ml) {
            if (const json::Value* ml = pl.find("missile_launchers_owned"); ml) {
                fill_missile(std::clamp((int)ml->number_or(1),
                                        0, repair::k_max_missile_launchers));
            } else {
                fill_missile(1);   // Tarsus default: just the LEFT
            }
        }
        if (!got_tl) {
            if (const json::Value* tl = pl.find("torpedo_tubes_owned"); tl) {
                fill_torpedo(std::clamp((int)tl->number_or(0),
                                        0, repair::k_max_torpedo_launchers));
            }
            // else: leave torpedos empty
        }

        // Clamp loaded ordnance to its invariants (#8): a hand-edited
        // save mustn't smuggle negative values past the player:: setters,
        // same as rep is clamped above.
        for (int& m : out.missiles) { if (m < 0) m = 0; }
        if (out.torpedoes < 0) out.torpedoes = 0;

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
            // v5 split sides into port + starboard (issue #30). If the
            // save still has the old *_side keys, split the cm evenly
            // across the two new faces so total protection is preserved.
            out.hp_armor_fore  = h.contains("armor_fore")  ? (float)h["armor_fore"].number_or(0)  : 0.0f;
            out.hp_armor_aft   = h.contains("armor_aft")   ? (float)h["armor_aft"].number_or(0)   : 0.0f;
            out.hp_shield_fore = h.contains("shield_fore") ? (float)h["shield_fore"].number_or(0) : 0.0f;
            out.hp_shield_aft  = h.contains("shield_aft")  ? (float)h["shield_aft"].number_or(0)  : 0.0f;
            out.hp_energy      = h.contains("energy")      ? (float)h["energy"].number_or(0)      : 0.0f;
            if (h.contains("armor_port") && h.contains("armor_starboard")) {
                out.hp_armor_port     = (float)h["armor_port"].number_or(0);
                out.hp_armor_starboard = (float)h["armor_starboard"].number_or(0);
            } else if (h.contains("armor_side")) {
                // Old save had a single shared side value; under the
                // issue-30 redesign each flank gets the FULL value (not
                // half) so port/starboard don't halve each other.
                const float v = (float)h["armor_side"].number_or(0);
                out.hp_armor_port = out.hp_armor_starboard = v;
            } else {
                out.hp_armor_port = out.hp_armor_starboard = 0.0f;
            }
            if (h.contains("shield_port") && h.contains("shield_starboard")) {
                out.hp_shield_port     = (float)h["shield_port"].number_or(0);
                out.hp_shield_starboard = (float)h["shield_starboard"].number_or(0);
            } else if (h.contains("shield_side")) {
                const float v = (float)h["shield_side"].number_or(0);
                out.hp_shield_port = out.hp_shield_starboard = v;
            } else {
                out.hp_shield_port = out.hp_shield_starboard = 0.0f;
            }
        }

        // Persistent world clock (Gemini Lives #171, format v8). Pre-v8
        // saves begin at the epoch; hand-edited negative days clamp to zero.
        out.day = pl.contains("day")
            ? std::max(0, (int)pl["day"].number_or(0)) : 0;
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
        if (!info.exists) {
            // Name the offender: the raw [json] parse error above this line
            // carries no filename, which made #242 needlessly mysterious.
            std::fprintf(stderr, "[save] skipping unreadable save file '%s'\n",
                         ent.path().string().c_str());
            continue;
        }
        out.push_back(std::move(info));
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

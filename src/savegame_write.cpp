// -----------------------------------------------------------------------------
// savegame_write.cpp — PlayerState -> save document (see savegame_codec.h).
//
//   * a tiny JsonWriter — just enough to emit objects/arrays/strings/numbers
//     with correct escaping and human-readable indentation. json.cpp is
//     parse-only by deliberate design (see its header: "NOT in scope:
//     emitting JSON"), so rather than widen that module's contract we keep
//     the writer local to the one caller that needs it. If a second writer
//     consumer ever appears, promote this into json:: then — until then,
//     YAGNI.
//   * one write_<group>() per field group, called in on-disk key order.
//     Reputation goes out as an object keyed by faction NAME
//     (faction::to_name) so an enum reorder never scrambles old saves.
//     credits goes out as a STRING, not a JSON number, because our parser
//     stores numbers as double — fine to ~2^53, but credits is an int64 by
//     design (header) and we promised bit-exact, so a string + strtoll
//     dodges any float rounding entirely.
// -----------------------------------------------------------------------------

#include "savegame_codec.h"

#include "faction.h"
#include "inventory.h"
#include "player.h"
#include "ship_systems.h"
#include "world_clock.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace savegame::codec {

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

    // `key: [ "a", "b", ... ]` — the flat string-list shape several groups use.
    void member_string_array(const std::string& k, const std::vector<std::string>& v) {
        key(k); member_array_begin();
          for (const std::string& s : v) value_string(s);
        end_array();
    }
};

// Build the full timestamped title (np-3dp.19):
//   "YYYY-MM-DD HH:MM - <stardate> - <system> - <base> - <ship> - <credits> cr"
// Unset fields read as placeholders ("deep space" base, "?" system/ship).
std::string make_label(const PlayerState& p, long long ts) {
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

// credits + reputation.
void write_standing(JsonWriter& w, const PlayerState& p) {
    // credits as STRING for guaranteed int64 bit-exactness (see header).
    w.key("credits"); w.value_string(std::to_string((long long)p.credits));

    // reputation: object keyed by faction NAME (stable across enum order).
    w.key("rep"); w.member_object_begin();
      for (int i = 0; i < kFactionCount; ++i) {
          w.key(faction::to_name((Faction)i));
          w.value_int((long long)p.rep.rep[i]);
      }
    w.end_object();
}

// Hull + everything fitted to it.
void write_loadout(JsonWriter& w, const PlayerState& p) {
    w.key("ship_class_name"); w.value_string(p.ship_class_name);

    // gun_mounts (Phase 4d Wave 1, #88/#89). Each mount is now an OBJECT
    // carrying gun_id + rarity + per-shot mods, mirroring the items array
    // below. Old saves wrote bare strings; the loader still accepts those
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
    // turret hardware (#145, v10): owned TurretSlot ids.
    w.member_string_array("turrets", p.turrets);
    w.key("has_jump_drive");   w.value_bool(p.has_jump_drive);
    w.key("ecm_level");        w.value_int(p.ecm_level);
    w.key("has_repair_droid"); w.value_bool(p.has_repair_droid);
    w.key("adv_repair_droid"); w.value_bool(p.adv_repair_droid);
    w.key("has_tractor_beam"); w.value_bool(p.has_tractor_beam);
}

// guild memberships (#16, v6). Absent on older saves -> false.
void write_guilds(JsonWriter& w, const PlayerState& p) {
    w.key("merc_guild_member");     w.value_bool(p.merc_guild_member);
    w.key("merchant_guild_member"); w.value_bool(p.merchant_guild_member);
}

// campaign plot state (#138, v7). Two flat string arrays — see plot.h for the
// naming conventions. Absent on older saves -> empty == campaign not started.
void write_plot(JsonWriter& w, const PlayerState& p) {
    w.member_string_array("plot_flags", p.plot_flags);
    w.member_string_array("plot_items", p.plot_items);
}

// Commodity cargo, discrete hold items, and installed permanent upgrades.
void write_hold(JsonWriter& w, const PlayerState& p) {
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

    // unified-hold items (Phase 4 Wave 1, #80 + #81). Discrete things
    // dropped, bought, or salvaged into the hold. Stable string keys for
    // every field; enums are written as ints so a reorder of the ItemKind /
    // Rarity enum values can't scramble old saves. Float mods use the same
    // std::to_string form as ship_health (default precision; 1.0 round-trips).
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

    // permanent upgrades (Phase 4e, #92). Installed Upgrade items reborn as
    // permanent effects. Stable string keys for id/effect; value is a float
    // written with the same std::to_string form as ship_health. Absent on
    // older saves -> empty list (back-compat).
    w.key("permanent_mods"); w.member_array_begin();
      for (const PermanentMod& pm : p.permanent_mods) {
          w.begin_object();
            w.key("id");     w.value_string(pm.id);
            w.key("effect"); w.value_string(pm.effect);
            w.key("value");  w.value_raw(std::to_string(pm.value));
          w.end_object();
      }
    w.end_array();
}

// accepted missions (np-zte.1, format v2). Stable string keys; reward as a
// STRING for the same int64 bit-exactness reason as credits. Only the fields
// meaningful to each `type` are populated, but we emit them all
// unconditionally — absent ones default harmlessly on load and the uniform
// shape keeps the writer simple.
//
// (#8, format v5) the per-mission payload grew: `source`, the non-cargo
// `target_system`, the parallel `nav_targets` array, `nav_count`, the attack
// `hostiles_required`, the defend `target_base`, the bounty's `bounty_region`
// and `last_seen(_alt)?_system`. Same flat-shape policy — emit all, load
// tolerates missing keys.
void write_missions(JsonWriter& w, const PlayerState& p) {
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
            w.member_string_array("nav_targets",   m.nav_targets);
            w.member_string_array("bounty_region", m.bounty_region);
            // Live in-flight progress (#12/#13): per-nav reached flags, so
            // mid-mission survey progress survives save/reload. Written as
            // 0/1 ints; absent on older saves -> resized empty on load.
            w.key("nav_done"); w.member_array_begin();
              for (uint8_t d : m.nav_done) w.value_int(d ? 1 : 0);
            w.end_array();
          w.end_object();
      }
    w.end_array();
}

// ordnance + launchers (np-zte.2, format v3).
void write_ordnance(JsonWriter& w, const PlayerState& p) {
    // Missiles as a fixed-key object (df/hs/ir) so adding a 4th type later
    // doesn't shift array meaning. Absent on v1/v2 saves -> defaults on load.
    // FF (#144) joined as "ff" — that's the payoff of fixed keys.
    w.key("missiles"); w.member_object_begin();
    for (int i = 0; i < k_missile_rack_types; ++i) {
        w.key(k_missile_save_keys[i]); w.value_int(p.missiles[i]);
    }
    w.end_object();
    // Torpedo rack (np-zte.2 expansion, np-9cu-launchers-bump). Single
    // counter now (was a DF/HS/IR fan-out; that was overkill since Privateer
    // canon has exactly one torpedo ammo type).
    w.key("torpedoes"); w.value_int(p.torpedoes);
    // Per-side hardware slots (left + right for each ammo type). Four
    // booleans; new-save format. Old saves that used the integer
    // missile_launchers_owned / torpedo_tubes_owned keys get migrated on
    // load (see read_ordnance).
    w.key("missile_launcher_left");   w.value_bool(p.missile_launcher_left);
    w.key("missile_launcher_right");  w.value_bool(p.missile_launcher_right);
    w.key("torpedo_launcher_left");   w.value_bool(p.torpedo_launcher_left);
    w.key("torpedo_launcher_right");  w.value_bool(p.torpedo_launcher_right);
    // afterburner_fuel removed (np-zte.2 merged pool). Old keys in v3 saves
    // are ignored on load; new saves omit the key entirely.
}

// career kills per faction (np-3dp.19, v4). Object keyed by faction NAME
// (stable across enum reorder, like rep). Counts as STRINGS for int64
// bit-exactness.
void write_career(JsonWriter& w, const PlayerState& p) {
    w.key("faction_kills"); w.member_object_begin();
      for (int i = 0; i < kFactionCount; ++i) {
          w.key(faction::to_name((Faction)i));
          w.value_string(std::to_string((long long)p.faction_kills[i]));
      }
    w.end_object();
}

// live ship-damage snapshot (np-3dp.19, v4). Absent/hp_valid=false -> the
// loaded ship stays at full health.
void write_ship_health(JsonWriter& w, const PlayerState& p) {
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
      // Component integrity (#141, v11), keyed by the stable system key.
      w.key("systems"); w.member_object_begin();
        for (int i = 0; i < kShipSystemCount; ++i) {
            w.key(ship_systems::key(ship_systems::at(i)));
            w.value_raw(std::to_string(p.hp_systems[i]));
        }
      w.end_object();
    w.end_object();
}

// Where + when: persistent world clock (Gemini Lives #171, format v8) and
// the player's location.
void write_location(JsonWriter& w, const PlayerState& p) {
    w.key("day");              w.value_int(p.day);
    w.key("current_system");   w.value_string(p.current_system);
    w.key("last_docked_base"); w.value_string(p.last_docked_base);
    w.key("docked");           w.value_bool(p.docked);
}

} // namespace

std::string encode(const PlayerState& p) {
    const long long ts = (long long)std::time(nullptr);

    JsonWriter w;
    w.begin_object();
      w.key("version");   w.value_int(k_format_version);
      w.key("timestamp"); w.value_int(ts);
      w.key("label");     w.value_string(make_label(p, ts));

      w.key("player"); w.member_object_begin();
        write_standing(w, p);
        write_loadout(w, p);
        write_guilds(w, p);
        write_plot(w, p);
        write_hold(w, p);
        write_missions(w, p);
        write_ordnance(w, p);
        write_career(w, p);
        write_ship_health(w, p);
        write_location(w, p);
      w.end_object();   // player
    w.end_object();     // root
    w.out += "\n";
    return w.out;
}

} // namespace savegame::codec

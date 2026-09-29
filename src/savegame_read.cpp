// -----------------------------------------------------------------------------
// savegame_read.cpp — save document -> PlayerState (see savegame_codec.h).
//
// One read_<group>() per field group, mirroring savegame_write.cpp. Every
// reader tolerates MISSING keys from older formats by leaving the
// default-constructed value (or an explicit migration default) in place, and
// clamps hand-edited junk back into each field's invariants. Legacy shapes
// (bare-string gun mounts, integer launcher counts, *_side hull faces, the
// pre-#6 mission enum, ...) are migrated here, next to the field they touch.
// -----------------------------------------------------------------------------

#include "savegame_codec.h"

#include "faction.h"
#include "inventory.h"
#include "player.h"
#include "repair.h"
#include "ship_class.h"
#include "ship_systems.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace savegame::codec {

namespace {

// int64 fields are written as STRINGS for bit-exactness (see
// savegame_write.cpp); a legacy JSON number is tolerated. Anything else
// leaves `fallback`.
int64_t int64_or(const json::Value& v, int64_t fallback) {
    if (v.is_string()) return std::strtoll(v.as_string().c_str(), nullptr, 10);
    if (v.is_number()) return (int64_t)v.as_number();
    return fallback;
}

// Appends the string entries of array member `key` to `out`, skipping
// non-strings. Returns false when the member is absent or not an array.
bool read_string_list(const json::Value& obj, const char* key,
                      std::vector<std::string>& out) {
    const json::Value* arr = obj.find(key);
    if (!arr || !arr->is_array()) return false;
    for (const json::Value& g : arr->as_array())
        if (g.is_string()) out.push_back(g.as_string());
    return true;
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

// credits + reputation.
void read_standing(const json::Value& pl, PlayerState& out) {
    // credits: string preferred (bit-exact int64); tolerate a legacy number.
    if (const json::Value* c = pl.find("credits")) out.credits = int64_or(*c, out.credits);

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
}

// Hull + everything fitted to it. gun_mounts must be read before turrets:
// the pre-v10 grandfathering inspects them.
void read_loadout(const json::Value& pl, PlayerState& out) {
    out.ship_class_name = pl.contains("ship_class_name")
        ? pl["ship_class_name"].string_or("") : "";

    // gun_mounts (Phase 4d Wave 1, #88/#89). Back-compat: old saves stored
    // bare strings (-> Basic rarity, 1.0/1.0 mods); new saves store objects
    // with gun_id/rarity/mods. Empty/missing -> skip.
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
    if (!read_string_list(pl, "turrets", out.turrets)) grandfather_turrets(out);
    out.has_jump_drive   = pl.contains("has_jump_drive")   ? pl["has_jump_drive"].bool_or(false)   : false;
    out.ecm_level        = pl.contains("ecm_level")        ? (int)pl["ecm_level"].number_or(0)    : 0;
    out.has_repair_droid = pl.contains("has_repair_droid") ? pl["has_repair_droid"].bool_or(false) : false;
    out.adv_repair_droid = pl.contains("adv_repair_droid") ? pl["adv_repair_droid"].bool_or(false) : false;
    out.has_tractor_beam = pl.contains("has_tractor_beam") ? pl["has_tractor_beam"].bool_or(false) : false;
}

// guild memberships (#16, v6). Older saves default to non-member.
void read_guilds(const json::Value& pl, PlayerState& out) {
    out.merc_guild_member     = pl.contains("merc_guild_member")     ? pl["merc_guild_member"].bool_or(false)     : false;
    out.merchant_guild_member = pl.contains("merchant_guild_member") ? pl["merchant_guild_member"].bool_or(false) : false;
}

// campaign plot state (#138, v7). Missing on pre-v7 saves -> both lists stay
// empty (campaign not started). Non-string entries are skipped.
void read_plot(const json::Value& pl, PlayerState& out) {
    read_string_list(pl, "plot_flags", out.plot_flags);
    read_string_list(pl, "plot_items", out.plot_items);
}

// Commodity cargo, discrete hold items, and installed permanent upgrades.
void read_hold(const json::Value& pl, PlayerState& out) {
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

    // unified-hold items (Phase 4 Wave 1, #80 + #81). Absent on older saves
    // -> empty list (back-compat with no-items era). Enums read back as int
    // via static_cast; mirrors the per-faction int-key pattern.
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

    // permanent upgrades (Phase 4e, #92). Absent on older saves -> empty list
    // (back-compat with the pre-upgrade era). id/effect as strings, value as
    // a float; an entry with no id is skipped.
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
}

// accepted missions (np-zte.1, v2). Absent on a v1 save -> empty list
// (back-compat). reward read as a string (bit-exact int64), tolerating a
// legacy number. An entry with no id is skipped defensively.
//
// (#8, v5) the per-mission payload grew (see write_missions). Old saves from
// before #6's MissionType renumber carry `type` values that no longer
// correspond to any legal mission — drop them and log one line so a
// hand-edited or pre-#6 save can't silently invent a bogus mission. Also
// drop any `type` outside [0,5].
void read_missions(const json::Value& pl, int ver, int slot, PlayerState& out) {
    const json::Value* ms = pl.find("missions");
    if (!ms || !ms->is_array()) return;
    for (const json::Value& e : ms->as_array()) {
        if (!e.is_object()) continue;
        ActiveMission am;
        am.id            = e.contains("id")            ? e["id"].string_or("")            : "";
        am.type          = e.contains("type")          ? (int)e["type"].number_or(0)      : 0;
        // (#9,#11) Pre-v5 saves used the OLD 2-value mission enum
        // (0=CargoDelivery, 1=Bounty); remap to the current 0..5 numbering
        // (now 4=Bounty, 5=CargoDelivery; see MissionType in missions.h).
        // Other values in old saves pass through to the existing 0..5 range
        // check, which drop+logs them.
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
        if (const json::Value* r = e.find("reward")) am.reward = int64_or(*r, am.reward);
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
        read_string_list(e, "nav_targets",   am.nav_targets);
        read_string_list(e, "bounty_region", am.bounty_region);
        // Live in-flight progress (#12/#13). Absent on older saves -> stays
        // empty here, then normalized to nav_targets length below so the #13
        // tracker always has a flag per nav target.
        if (const json::Value* nd = e.find("nav_done"); nd && nd->is_array()) {
            for (const json::Value& g : nd->as_array())
                am.nav_done.push_back((uint8_t)(g.number_or(0) != 0 ? 1 : 0));
        }
        if (am.nav_done.size() != am.nav_targets.size())
            am.nav_done.resize(am.nav_targets.size(), (uint8_t)0);
        if (!am.id.empty()) out.missions.push_back(std::move(am));
    }
}

// One per-side launcher pair. Either key present means the per-side format,
// and a missing side reads off: a half-written pair must neither dereference
// null nor be discarded for the legacy default (#538). Returns false when
// neither key exists (a pre-per-side save; the caller migrates its count).
bool read_launcher_pair(const json::Value& pl, const char* left_key,
                        const char* right_key, bool& left, bool& right) {
    const json::Value* l = pl.find(left_key);
    const json::Value* r = pl.find(right_key);
    if (!l && !r) return false;
    left  = l && l->bool_or(false);
    right = r && r->bool_or(false);
    return true;
}

// ordnance + launchers (np-zte.2, v3).
void read_ordnance(const json::Value& pl, PlayerState& out) {
    // Missiles default to 0 (an old save genuinely had none). The legacy
    // `afterburner_fuel` key is no longer a PlayerState field — the fuel
    // pool merged into the player Ship's energy_gj. We tolerate the key in
    // older saves by just ignoring it; new saves don't emit it. A key
    // missing from an older save (e.g. "ff" pre-#144) reads 0.
    if (const json::Value* ms = pl.find("missiles"); ms && ms->is_object()) {
        for (int i = 0; i < k_missile_rack_types; ++i) {
            const char* key = k_missile_save_keys[i];
            out.missiles[i] = ms->contains(key) ? (int)(*ms)[key].number_or(0) : 0;
        }
    }
    // Torpedo rack (np-zte.2, np-launchers-bump). Absent on v1/v2 saves ->
    // defaults to 0, which is the correct empty state. v1/v2 saves that
    // stored torpedoes as a {df,hs,ir} object also read OK (we sum the
    // three counts together).
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
    // Launcher hardware (np-zte.2, np-launchers-bump). Each side is its own
    // bool. New keys: missile_launcher_left, missile_launcher_right,
    // torpedo_launcher_left, torpedo_launcher_right. Old format
    // (missile_launchers_owned / torpedo_tubes_owned as ints) gets migrated:
    // count == 1 -> fill the LEFT, count == 2 -> fill both. Defaults match
    // the new-game loadout: missile LEFT on, others off.
    auto fill_missile = [&](int n_owned) {
        out.missile_launcher_left  = n_owned >= 1;
        out.missile_launcher_right = n_owned >= 2;
    };
    auto fill_torpedo = [&](int n_owned) {
        out.torpedo_launcher_left  = n_owned >= 1;
        out.torpedo_launcher_right = n_owned >= 2;
    };
    if (!read_launcher_pair(pl, "missile_launcher_left", "missile_launcher_right",
                            out.missile_launcher_left, out.missile_launcher_right)) {
        if (const json::Value* ml = pl.find("missile_launchers_owned"); ml) {
            fill_missile(std::clamp((int)ml->number_or(1),
                                    0, repair::k_max_missile_launchers));
        } else {
            fill_missile(1);   // Tarsus default: just the LEFT
        }
    }
    if (!read_launcher_pair(pl, "torpedo_launcher_left", "torpedo_launcher_right",
                            out.torpedo_launcher_left, out.torpedo_launcher_right)) {
        if (const json::Value* tl = pl.find("torpedo_tubes_owned"); tl) {
            fill_torpedo(std::clamp((int)tl->number_or(0),
                                    0, repair::k_max_torpedo_launchers));
        }
        // else: leave torpedos empty
    }

    // Clamp loaded ordnance to its invariants (#8): a hand-edited save
    // mustn't smuggle negative values past the player:: setters, same as
    // rep is clamped in read_standing.
    for (int& m : out.missiles) { if (m < 0) m = 0; }
    if (out.torpedoes < 0) out.torpedoes = 0;
}

// career kills per faction (np-3dp.19, v4). Read by faction NAME; missing
// keys / older saves stay 0. STRING preferred (bit-exact), legacy number
// tolerated.
void read_career(const json::Value& pl, PlayerState& out) {
    const json::Value* fk = pl.find("faction_kills");
    if (!fk || !fk->is_object()) return;
    for (const auto& [name, val] : fk->as_object()) {
        const Faction f = faction::from_name(name);
        if (f == Faction::Count) continue;
        int64_t v = int64_or(val, 0);
        if (v < 0) v = 0;
        out.faction_kills[(int)f] = v;
    }
}

// live ship-damage snapshot (np-3dp.19, v4). hp_valid gates whether it's
// applied to the spawned ship; absent on older saves -> false.
void read_ship_health(const json::Value& pl, PlayerState& out) {
    const json::Value* sh = pl.find("ship_health");
    if (!sh || !sh->is_object()) return;
    const json::Value& h = *sh;
    out.hp_valid       = h.contains("valid")       ? h["valid"].bool_or(false)            : false;
    // v5 split sides into port + starboard (issue #30). If the save still
    // has the old *_side keys, each flank gets the FULL old value.
    out.hp_armor_fore  = h.contains("armor_fore")  ? (float)h["armor_fore"].number_or(0)  : 0.0f;
    out.hp_armor_aft   = h.contains("armor_aft")   ? (float)h["armor_aft"].number_or(0)   : 0.0f;
    out.hp_shield_fore = h.contains("shield_fore") ? (float)h["shield_fore"].number_or(0) : 0.0f;
    out.hp_shield_aft  = h.contains("shield_aft")  ? (float)h["shield_aft"].number_or(0)  : 0.0f;
    out.hp_energy      = h.contains("energy")      ? (float)h["energy"].number_or(0)      : 0.0f;
    if (h.contains("armor_port") && h.contains("armor_starboard")) {
        out.hp_armor_port     = (float)h["armor_port"].number_or(0);
        out.hp_armor_starboard = (float)h["armor_starboard"].number_or(0);
    } else if (h.contains("armor_side")) {
        // Old save had a single shared side value; under the issue-30
        // redesign each flank gets the FULL value (not half) so
        // port/starboard don't halve each other.
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
    // Component integrity (#141, v11). Missing object/key (pre-v11 save) =
    // pristine; junk clamps into [0, 1].
    if (const json::Value* sys = h.find("systems"); sys && sys->is_object()) {
        for (int i = 0; i < kShipSystemCount; ++i) {
            const char* k = ship_systems::key(ship_systems::at(i));
            if (sys->contains(k))
                out.hp_systems[i] = std::clamp(
                    (float)(*sys)[k].number_or(1.0), 0.0f, 1.0f);
        }
    }
}

// Where + when. Persistent world clock (Gemini Lives #171, format v8):
// pre-v8 saves begin at the epoch; hand-edited negative days clamp to zero.
void read_location(const json::Value& pl, PlayerState& out) {
    out.day = pl.contains("day")
        ? std::max(0, (int)pl["day"].number_or(0)) : 0;
    out.current_system   = pl.contains("current_system")   ? pl["current_system"].string_or("")   : "";
    out.last_docked_base = pl.contains("last_docked_base") ? pl["last_docked_base"].string_or("") : "";
    out.docked           = pl.contains("docked")           ? pl["docked"].bool_or(false)          : false;
}

} // namespace

bool decode(const json::Value& root, int slot, PlayerState& out) {
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

    read_standing(pl, out);
    read_loadout(pl, out);
    read_guilds(pl, out);
    read_plot(pl, out);
    read_hold(pl, out);
    read_missions(pl, ver, slot, out);
    read_ordnance(pl, out);
    read_career(pl, out);
    read_ship_health(pl, out);
    read_location(pl, out);
    return true;
}

SlotInfo peek(const json::Value& root) {
    SlotInfo info;
    info.version   = root.contains("version")   ? root["version"].as_int()        : 0;
    info.timestamp = root.contains("timestamp") ? (int64_t)root["timestamp"].number_or(0) : 0;
    info.label     = root.contains("label")     ? root["label"].string_or("")      : "";
    if (const json::Value* pv = root.find("player"); pv && pv->is_object()) {
        const json::Value& pl = *pv;
        info.base   = pl.contains("last_docked_base") ? pl["last_docked_base"].string_or("") : "";
        info.system = pl.contains("current_system")   ? pl["current_system"].string_or("")   : "";
        info.ship   = pl.contains("ship_class_name")  ? pl["ship_class_name"].string_or("")  : "";
        if (const json::Value* c = pl.find("credits")) info.credits = int64_or(*c, info.credits);
    }
    info.exists = info.version > 0;
    return info;
}

} // namespace savegame::codec

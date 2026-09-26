// -----------------------------------------------------------------------------
// system_def.cpp — JSON → StarSystem materialisation.
// -----------------------------------------------------------------------------

#include "system_def.h"
#include "json.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace {

// Read an optional [x, y, z] triple into a HMM_Vec3, leaving the caller-
// supplied default if the key is absent or the value doesn't parse.
HMM_Vec3 vec3_or(const json::Value* v, HMM_Vec3 def) {
    if (!v || !v->is_array() || v->as_array().size() != 3) return def;
    const auto& arr = v->as_array();
    if (!arr[0].is_number() || !arr[1].is_number() || !arr[2].is_number()) return def;
    return { arr[0].as_float(), arr[1].as_float(), arr[2].as_float() };
}

// Read an optional [x, y] pair into a HMM_Vec2. Used for authored navmap
// layout coordinates; gameplay remains on the 3D `position` above.
bool vec2_into(const json::Value* v, HMM_Vec2& out) {
    if (!v || !v->is_array() || v->as_array().size() != 2) return false;
    const auto& arr = v->as_array();
    if (!arr[0].is_number() || !arr[1].is_number()) return false;
    out = { arr[0].as_float(), arr[1].as_float() };
    return true;
}

PlacedMeshDef parse_mesh(const json::Value& v) {
    PlacedMeshDef m;
    if (auto* p = v.find("obj"))            m.obj_path      = p->as_string();
    m.position  = vec3_or(v.find("position"),  m.position);
    m.euler_deg = vec3_or(v.find("euler_deg"), m.euler_deg);
    m.tint      = vec3_or(v.find("tint"),       m.tint);
    if (auto* p = v.find("length_meters")) m.length_meters  = p->as_float();
    if (auto* p = v.find("scale"))         m.scale          = p->as_float();
    if (auto* p = v.find("spec"))          m.spec           = p->as_float();
    if (auto* p = v.find("texture_preset"))m.texture_preset = p->as_string();
    if (auto* p = v.find("double_sided")) m.double_sided   = p->as_bool();
    if (auto* p = v.find("clay_mode"))    m.clay_mode      = p->as_bool();
    if (auto* p = v.find("ambient_floor")) m.ambient_floor = p->as_float();
    if (auto* p = v.find("rim_strength"))  m.rim_strength  = p->as_float();
    // Atmosphere block: { "thickness": 0.04, "color": [r,g,b], "strength": 1.2 }
    if (auto* atm = v.find("atmosphere"); atm && atm->is_object()) {
        if (auto* p = atm->find("thickness")) m.atm_thickness = p->as_float();
        m.atm_color = vec3_or(atm->find("color"), m.atm_color);
        if (auto* p = atm->find("strength"))  m.atm_strength  = p->as_float();
    }
    return m;
}

// A placed sprite entry. Minimal schema — sprite stem, world position,
// and length in world units. Everything else about the sprite (lights,
// textures) is resolved from files next to the PNG.
PlacedSpriteDef parse_sprite(const json::Value& v) {
    PlacedSpriteDef s;
    if (auto* p = v.find("sprite"))         s.sprite        = p->as_string();
    s.position = vec3_or(v.find("position"), s.position);
    if (auto* p = v.find("length_meters"))  s.length_meters = p->as_float();
    return s;
}

PlacedShipSpriteDef parse_ship_sprite(const json::Value& v) {
    PlacedShipSpriteDef s;
    if (auto* p = v.find("atlas"))          s.atlas         = p->as_string();
    s.position = vec3_or(v.find("position"), s.position);
    if (auto* p = v.find("length_meters"))  s.length_meters = p->as_float();
    if (auto* p = v.find("lights_enabled")) s.lights_enabled = p->as_bool();

    // Optional `motion` block. Either field is independently optional;
    // omitting both keeps the ship static (back-compat for every existing
    // scene). Angular velocity is authored in deg/s for human readability;
    // the runtime converts to rad/s once at scene-load time.
    if (auto* m = v.find("motion")) {
        s.angular_velocity_deg = vec3_or(m->find("angular_velocity_deg"),
                                         s.angular_velocity_deg);
        if (auto* p = m->find("forward_speed")) s.forward_speed = p->as_float();
    }

    // Optional explicit ship_class — useful when the atlas path doesn't
    // match the registry key (rare, but possible if someone aliases
    // assets). Empty default lets main.cpp derive from the atlas path.
    if (auto* p = v.find("ship_class")) s.ship_class       = p->as_string();
    if (auto* p = v.find("faction"))    s.faction_override = p->as_string();
    if (auto* p = v.find("inert"))      s.inert            = p->as_bool();

    // Optional `behavior` block. Drives the flight controller instead
    // of (or alongside) the legacy motion fields — see ship.h. When
    // absent, the ship runs on whatever motion was set above, which is
    // the back-compat path every existing scene relies on.
    if (auto* b = v.find("behavior")) {
        if (auto* k = b->find("kind")) s.behavior_kind = k->as_string();
        s.behavior_target_pos = vec3_or(b->find("target_pos"), s.behavior_target_pos);
    }

    // Optional `ai` block. When present the AI state machine drives
    // the ship — initial_state seeds the machine, patrol_anchor (if
    // set) tells the Patrol state where to loiter. Mutually exclusive
    // with `behavior` for any given ship: ai_enabled wins.
    if (auto* ai = v.find("ai")) {
        s.ai_enabled = true;
        if (auto* p = ai->find("initial_state")) s.ai_initial_state = p->as_string();
        if (auto* anchor = ai->find("patrol_anchor")) {
            s.ai_patrol_anchor     = vec3_or(anchor, s.ai_patrol_anchor);
            s.ai_has_patrol_anchor = true;
        }
    }
    return s;
}

// Far-field sky prop. Directions are normalised; angular size is clamped
// so a typo can't push the billboard through the camera far plane.
SkyPropDef parse_sky_prop(const json::Value& v) {
    SkyPropDef p;
    if (auto* s = v.find("sprite")) p.sprite = s->as_string();
    if (const json::Value* d = v.find("dir"))
        p.direction = vec3_or(d, p.direction);
    else if (const json::Value* d = v.find("direction"))
        p.direction = vec3_or(d, p.direction);
    const float len = std::sqrt(p.direction.X * p.direction.X +
                                p.direction.Y * p.direction.Y +
                                p.direction.Z * p.direction.Z);
    if (len > 1e-4f) {
        p.direction.X /= len;
        p.direction.Y /= len;
        p.direction.Z /= len;
    } else {
        p.direction = { 0.0f, 1.0f, 0.0f };
    }
    if (auto* a = v.find("angular_deg")) p.angular_deg = a->as_float();
    if (auto* r = v.find("roll_rad"))    p.roll_rad    = r->as_float();
    if (auto* r = v.find("roll_deg"))    p.roll_rad    = r->as_float() * 0.01745329251f;
    if (auto* a = v.find("alpha"))       p.alpha       = a->as_float();
    if (p.alpha < 0.0f) p.alpha = 0.0f;
    if (p.alpha > 1.0f) p.alpha = 1.0f;
    if (p.angular_deg < 2.0f)  p.angular_deg = 2.0f;
    if (p.angular_deg > 28.0f) p.angular_deg = 28.0f;
    return p;
}

// A nav waypoint — name, kind tag, position. Kind defaults to "nav" so a
// minimal entry { "name": "X", "position": [...] } parses cleanly.
NavPointDef parse_nav(const json::Value& v) {
    NavPointDef n;
    if (auto* p = v.find("name")) n.name = p->as_string();
    if (auto* p = v.find("kind")) n.kind = p->as_string();
    n.position = vec3_or(v.find("position"), n.position);
    n.has_map_position = vec2_into(v.find("map_position"), n.map_position);
    // Docking metadata (np-9cu.1). Both optional — a plain nav point
    // omits them and stays non-dockable with an empty base_id.
    if (auto* p = v.find("dockable")) n.dockable = p->as_bool();
    if (auto* p = v.find("base_id"))  n.base_id  = p->as_string();
    // Jump-link metadata (np-6al.1) — both optional; a non-jump nav or a
    // dangling frontier gate simply omits them.
    if (auto* p = v.find("links_to"))     n.links_to     = p->as_string();
    if (auto* p = v.find("links_to_nav")) n.links_to_nav = p->as_string();
    // wcnews per-nav encounter table: [{chance, members:[{faction,ship,count}]}]
    if (auto* enc = v.find("encounters"); enc && enc->is_array()) {
        for (const json::Value& g : enc->as_array()) {
            if (!g.is_object()) continue;
            EncounterGroupDef grp;
            if (auto* c = g.find("chance")) grp.chance = (float)c->as_float();
            if (auto* ms = g.find("members"); ms && ms->is_array()) {
                for (const json::Value& m : ms->as_array()) {
                    if (!m.is_object()) continue;
                    EncounterMemberDef mem;
                    if (auto* p = m.find("faction")) mem.faction    = p->as_string();
                    if (auto* p = m.find("ship"))    mem.ship_class = p->as_string();
                    if (auto* p = m.find("count"))   mem.count      = (int)p->as_float();
                    if (!mem.ship_class.empty() && !mem.faction.empty())
                        grp.members.push_back(mem);
                }
            }
            if (!grp.members.empty()) n.encounters.push_back(grp);
        }
    }
    return n;
}

// A weighted faction/class entry: { "name": "talon", "weight": 3 }.
// Weight defaults to 1.0 so a bare { "name": "x" } is a uniform pick.
EncounterWeight parse_weight(const json::Value& v) {
    EncounterWeight w;
    if (auto* p = v.find("name"))   w.name   = p->as_string();
    if (auto* p = v.find("weight")) w.weight = p->as_float();
    return w;
}

// An encounter spawn rule. Region defaults to "anywhere" so a minimal
// rule needs only a faction + class mix to roam near the player.
EncounterRuleDef parse_encounter(const json::Value& v) {
    EncounterRuleDef e;
    if (auto* p = v.find("name"))           e.name        = p->as_string();
    if (auto* p = v.find("region"))         e.region      = p->as_string();
    if (auto* p = v.find("field_index"))    e.field_index = p->as_int();
    if (auto* p = v.find("max_concurrent")) e.max_concurrent = p->as_int();
    if (auto* p = v.find("spawn_interval")) e.spawn_interval = p->as_float();
    if (auto* p = v.find("initial_state"))  e.initial_ai_state = p->as_string();

    if (auto* lane = v.find("lane"); lane && lane->is_array()) {
        for (const auto& n : lane->as_array()) {
            if (n.is_string()) e.lane.push_back(n.as_string());
        }
    }
    if (auto* fs = v.find("factions"); fs && fs->is_array()) {
        for (const auto& f : fs->as_array()) e.factions.push_back(parse_weight(f));
    }
    if (auto* cs = v.find("classes"); cs && cs->is_array()) {
        for (const auto& c : cs->as_array()) e.classes.push_back(parse_weight(c));
    }
    return e;
}

AsteroidFieldDef parse_field(const json::Value& v) {
    AsteroidFieldDef f;
    f.center      = vec3_or(v.find("center"),      f.center);
    f.half_extent = vec3_or(v.find("half_extent"), f.half_extent);
    if (auto* p = v.find("count"))       f.count       = p->as_int();
    if (auto* p = v.find("base_radius")) f.base_radius = p->as_float();
    if (auto* p = v.find("size_min"))    f.size_min    = p->as_float();
    if (auto* p = v.find("size_max"))    f.size_max    = p->as_float();
    if (auto* p = v.find("seed"))        f.seed        = p->as_u32();
    return f;
}

// If the caller passed "troy", resolve to assets/systems/troy.json.
// If they passed a path with a slash or .json suffix, use it literally.
std::string resolve_path(const std::string& s) {
    const bool has_slash = s.find('/') != std::string::npos;
    // Guard the .json-suffix check with a length test — otherwise `size()-5`
    // underflows when the name is < 5 chars (hello, "troy") and matches
    // std::string::npos by coincidence, sending us down the literal-path
    // branch with a relative filename like "troy" that doesn't exist.
    const bool has_json_suffix = s.size() >= 5 &&
                                 s.compare(s.size() - 5, 5, ".json") == 0;
    if (has_slash || has_json_suffix) return s;
    return "assets/systems/" + s + ".json";
}

} // namespace

namespace {

// FNV-1a 64-bit. Same offset and prime as sky_family_hash(), so a
// skybox_seed hashes identically here and in the sky-family picker.
uint64_t sky_prop_hash(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= (uint64_t)c;
        h *= 1099511628211ull;
    }
    return h ? h : 0x9E3779B97F4A7C15ull;
}

struct SkyRng {
    uint64_t s;
    explicit SkyRng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint32_t u32() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (uint32_t)((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    float unit() { return (float)(u32() >> 8) * (1.0f / 16777216.0f); }
};

HMM_Vec3 sky_rand_dir(SkyRng& r) {
    for (;;) {
        const float x = r.unit() * 2.0f - 1.0f;
        const float y = r.unit() * 2.0f - 1.0f;
        const float q = x * x + y * y;
        if (q >= 1.0f || q == 0.0f) continue;
        const float k = 2.0f * std::sqrt(1.0f - q);
        return HMM_V3(x * k, y * k, 1.0f - 2.0f * q);
    }
}

bool sky_separated(HMM_Vec3 dir, const std::vector<SkyPropDef>& prev) {
    for (const SkyPropDef& p : prev) {
        if (HMM_DotV3(dir, p.direction) > 0.35f) return false;
    }
    return true;
}

// Rotate `v` by `radians` toward a direction perpendicular to `axis_hint`.
HMM_Vec3 sky_rotate(HMM_Vec3 v, HMM_Vec3 axis_hint, float radians) {
    HMM_Vec3 axis = HMM_Cross(v, axis_hint);
    float al = HMM_LenV3(axis);
    if (al < 1e-4f) {
        axis = HMM_Cross(v, HMM_V3(1.0f, 0.0f, 0.0f));
        al = HMM_LenV3(axis);
    }
    axis = HMM_DivV3F(axis, al > 1e-6f ? al : 1.0f);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const HMM_Vec3 term1 = HMM_MulV3F(v, c);
    const HMM_Vec3 term2 = HMM_MulV3F(HMM_Cross(axis, v), s);
    const HMM_Vec3 term3 = HMM_MulV3F(axis, HMM_DotV3(axis, v) * (1.0f - c));
    return HMM_NormV3(HMM_AddV3(term1, HMM_AddV3(term2, term3)));
}

const SkyPropCatalogEntry k_sky_prop_catalog[] = {
    { "sky/props/pixel_galaxy_spiral_pink",      14.0f },
    { "sky/props/pixel_galaxy_elliptical_amber", 12.0f },
    { "sky/props/pixel_galaxy_edgeon_green",     16.0f },
    { "sky/props/pixel_galaxy_irregular_candy",  11.0f },
    { "sky/props/pixel_anomaly_wormhole_purple",  8.0f },
    { "sky/props/pixel_anomaly_ice_nebula",      13.0f },
    { "sky/props/pixel_anomaly_pulsar_red",       9.0f },
    { "sky/props/pixel_anomaly_plasma_blob",     10.0f },
};
constexpr int k_sky_prop_catalog_count = 8;
constexpr int k_sky_prop_galaxy_count  = 4;   // first entries are galaxies

} // namespace

const SkyPropCatalogEntry* sky_prop_catalog(int* count) {
    if (count) *count = k_sky_prop_catalog_count;
    return k_sky_prop_catalog;
}

std::vector<SkyPropDef> autogen_sky_props(const std::string& skybox_seed) {
    SkyRng rng(sky_prop_hash(skybox_seed));
    const int count = 1 + (int)(rng.u32() % 3u);

    // Every system gets at least one galaxy. The rest are a shuffle of
    // whatever is left in the pack, so two systems rarely share a trio.
    const int galaxy = (int)(rng.u32() % (uint32_t)k_sky_prop_galaxy_count);
    int pool[k_sky_prop_catalog_count];
    int n_pool = 0;
    for (int i = 0; i < k_sky_prop_catalog_count; ++i) {
        if (i == galaxy) continue;
        pool[n_pool++] = i;
    }
    for (int i = n_pool - 1; i > 0; --i) {
        const int j = (int)(rng.u32() % (uint32_t)(i + 1));
        std::swap(pool[i], pool[j]);
    }

    int chosen[3] = { galaxy, 0, 0 };
    for (int i = 1; i < count; ++i) chosen[i] = pool[i - 1];

    std::vector<SkyPropDef> out;
    out.reserve((size_t)count);
    for (int n = 0; n < count; ++n) {
        const SkyPropCatalogEntry& cat = k_sky_prop_catalog[chosen[n]];
        SkyPropDef p;
        p.sprite = cat.sprite;

        HMM_Vec3 dir = sky_rand_dir(rng);
        bool ok = sky_separated(dir, out);
        for (int attempt = 0; attempt < 24 && !ok; ++attempt) {
            dir = sky_rand_dir(rng);
            ok = sky_separated(dir, out);
        }
        if (!ok && !out.empty()) {
            dir = sky_rotate(out.back().direction, HMM_V3(0.0f, 1.0f, 0.0f), 1.9f);
            if (!sky_separated(dir, out))
                dir = sky_rotate(dir, HMM_V3(1.0f, 0.0f, 0.0f), 1.4f);
        }
        const float len = HMM_LenV3(dir);
        p.direction = HMM_DivV3F(dir, len > 1e-6f ? len : 1.0f);

        const float jitter = 0.85f + 0.30f * rng.unit();
        p.angular_deg = cat.angular_deg * jitter;
        p.roll_rad    = rng.unit() * 6.28318530718f;
        p.alpha       = 0.92f + 0.08f * rng.unit();
        out.push_back(p);
    }
    return out;
}

HMM_Vec3 sky_prop_world_position(HMM_Vec3 camera_pos, HMM_Vec3 direction) {
    const float len = HMM_LenV3(direction);
    const HMM_Vec3 dir = (len > 1e-4f)
        ? HMM_DivV3F(direction, len)
        : HMM_V3(0.0f, 1.0f, 0.0f);
    return HMM_AddV3(camera_pos, HMM_MulV3F(dir, k_sky_prop_dome_radius));
}

float sky_prop_world_size(float angular_deg) {
    float deg = angular_deg;
    if (deg < 2.0f)  deg = 2.0f;
    if (deg > 28.0f) deg = 28.0f;
    const float half = deg * 0.5f * 0.01745329251f;
    return 2.0f * k_sky_prop_dome_radius * std::tan(half);
}

std::optional<StarSystem> load_system(const std::string& name_or_path) {
    const std::string path = resolve_path(name_or_path);
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "[system] file not found: %s\n", path.c_str());
        return std::nullopt;
    }

    json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[system] '%s': root is not a JSON object\n", path.c_str());
        return std::nullopt;
    }

    StarSystem s;
    s.name        = root.find("name")        ? root["name"].string_or(s.name)               : s.name;
    s.description = root.find("description") ? root["description"].string_or(s.description) : s.description;
    s.skybox_seed = root.find("skybox_seed") ? root["skybox_seed"].string_or(s.skybox_seed) : s.skybox_seed;

    if (auto* star = root.find("star")) {
        if (auto* p = star->find("preset")) { s.star_preset = p->as_string(); s.star_preset_set = true; }
        if (auto* pos = star->find("position")) {
            s.star_position     = vec3_or(pos, s.star_position);
            s.star_position_set = true;
        }
        if (auto* p = star->find("radius"))             s.star_radius             = p->as_float();
        if (auto* p = star->find("gas_strength"))       s.star_gas_strength       = p->as_float();
        if (auto* p = star->find("gas_radius_mult"))    s.star_gas_radius_mult    = p->as_float();
        if (auto* p = star->find("corona_alpha"))       s.star_corona_alpha       = p->as_float();
        if (auto* p = star->find("corona_radius_mult")) s.star_corona_radius_mult = p->as_float();
    }
    if (auto* sky = root.find("sky")) {
        if (auto* p = sky->find("family")) s.sky_family = p->as_string();
    }

    if (auto* p = root.find("studio_lighting")) s.studio_lighting = p->as_bool();

    if (auto* fields = root.find("asteroid_fields"); fields && fields->is_array()) {
        for (const auto& f : fields->as_array()) {
            s.asteroid_fields.push_back(parse_field(f));
        }
    }

    if (auto* meshes = root.find("placed_meshes"); meshes && meshes->is_array()) {
        for (const auto& m : meshes->as_array()) {
            s.placed_meshes.push_back(parse_mesh(m));
        }
    }

    if (auto* sprites = root.find("placed_sprites"); sprites && sprites->is_array()) {
        for (const auto& sp : sprites->as_array()) {
            s.placed_sprites.push_back(parse_sprite(sp));
        }
    }

    if (auto* props = root.find("sky_props"); props && props->is_array()) {
        s.sky_props_authored = true;
        for (const auto& sp : props->as_array()) {
            if (!sp.is_object()) continue;
            SkyPropDef p = parse_sky_prop(sp);
            if (p.sprite.empty()) continue;
            s.sky_props.push_back(std::move(p));
        }
    } else {
        s.sky_props = autogen_sky_props(s.skybox_seed);
    }

    if (auto* ships = root.find("placed_ship_sprites"); ships && ships->is_array()) {
        for (const auto& sp : ships->as_array()) {
            s.placed_ship_sprites.push_back(parse_ship_sprite(sp));
        }
    }

    if (auto* navs = root.find("nav_points"); navs && navs->is_array()) {
        for (const auto& n : navs->as_array()) {
            s.nav_points.push_back(parse_nav(n));
        }
    }

    if (auto* enc = root.find("encounters"); enc && enc->is_array()) {
        for (const auto& e : enc->as_array()) {
            s.encounters.push_back(parse_encounter(e));
        }
    }

    if (auto* ps = root.find("player_start")) {
        s.player_start = vec3_or(ps->find("position"), s.player_start);
        if (auto* la = ps->find("look_at")) {
            s.player_look_at     = vec3_or(la, s.player_look_at);
            s.player_look_at_set = true;
        }
    }

    std::printf("[system] loaded '%s' — %s (skybox=%s, star=%s, fields=%zu, meshes=%zu, sprites=%zu, ship_sprites=%zu, navs=%zu, sky_props=%zu%s)\n",
                path.c_str(), s.name.c_str(), s.skybox_seed.c_str(),
                s.star_preset.c_str(), s.asteroid_fields.size(),
                s.placed_meshes.size(), s.placed_sprites.size(),
                s.placed_ship_sprites.size(), s.nav_points.size(),
                s.sky_props.size(),
                s.sky_props_authored ? " authored" : " seeded");
    for (const SkyPropDef& p : s.sky_props) {
        std::printf("[sky]   %s  ang=%.1f°  dir=(%.2f, %.2f, %.2f)\n",
                    p.sprite.c_str(), p.angular_deg,
                    p.direction.X, p.direction.Y, p.direction.Z);
    }
    if (!s.encounters.empty()) {
        std::printf("[system] '%s' has %zu encounter rule(s)\n",
                    s.name.c_str(), s.encounters.size());
    }
    return s;
}

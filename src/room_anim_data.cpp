// -----------------------------------------------------------------------------
// room_anim_data.cpp — see room_anim_data.h. Pure: json in, structs out.
// -----------------------------------------------------------------------------

#include "room_anim_data.h"

#include <cmath>

namespace room_anim {

namespace {

// Non-throwing lookup (json::Value::operator[] throws on a missing key): a
// missing key or non-object yields Null, whose *_or() returns the fallback.
const json::Value& field(const json::Value& obj, const char* key) {
    static const json::Value kNull;
    const json::Value* v = obj.is_object() ? obj.find(key) : nullptr;
    return v ? *v : kNull;
}

// Fixed-length numeric array; false on any shape or type mismatch.
template <size_t N>
bool read_floats(const json::Value& obj, const char* key, float (&out)[N]) {
    const json::Value& r = field(obj, key);
    if (!r.is_array() || r.as_array().size() != N) return false;
    for (size_t i = 0; i < N; ++i) {
        if (!r[i].is_number()) return false;
        out[i] = r[i].as_float();
    }
    return true;
}

bool read_rect(const json::Value& obj, const char* key, float (&out)[4]) {
    return read_floats(obj, key, out) && out[2] > 0.0f && out[3] > 0.0f;
}

int wrap(long long v, int period) {
    const long long m = v % period;
    return (int)(m < 0 ? m + period : m);
}

}  // namespace

bool parse_sprite_sheet(const json::Value& m, SpriteSheet& out, std::string& err) {
    out = SpriteSheet{};
    if (!m.is_object()) { err = "manifest is not an object"; return false; }
    out.atlas = field(m, "atlas").string_or("");
    float canvas[2] = {0.0f, 0.0f};
    if (out.atlas.empty() || !read_floats(m, "canvas", canvas) || canvas[0] <= 0.0f ||
        canvas[1] <= 0.0f) {
        err = "missing atlas or canvas";
        return false;
    }
    out.canvas_w = canvas[0];
    out.canvas_h = canvas[1];
    out.fps    = (float)field(m, "fps").number_or(24.0);
    out.period = (int)field(m, "period_frames").number_or(0.0);
    out.offset = (int)field(m, "offset_frames").number_or(0.0);
    if (out.fps <= 0.0f || out.period <= 0) { err = "fps and period_frames must be > 0"; return false; }

    out.slot_frame.assign((size_t)out.period, -1);
    const json::Value* frames = m.find("frames");
    if (!frames || !frames->is_array()) { err = "missing frames"; return false; }
    for (const json::Value& f : frames->as_array()) {
        SpriteFrame fr;
        const int slot = (int)field(f, "slot").number_or(-1.0);
        if (slot < 0 || slot >= out.period || !read_rect(f, "src", fr.src) ||
            !read_rect(f, "dst", fr.dst)) {
            err = "bad frame (slot out of range or degenerate rect)";
            return false;
        }
        out.slot_frame[(size_t)slot] = (int)out.frames.size();
        out.frames.push_back(fr);
    }
    return true;
}

void parse_room_anim(const json::Value& room, RoomAnimDef& out) {
    out = RoomAnimDef{};
    if (!room.is_object()) return;
    if (const json::Value* sky = room.find("sky"); sky && sky->is_object()) {
        out.sky.mask = field(*sky, "mask").string_or("");
        out.sky.fill = field(*sky, "fill").string_or("");
        if (const json::Value* stars = sky->find("stars"); stars && stars->is_array()) {
            for (const json::Value& s : stars->as_array()) {
                StarLayerDef def;
                def.tile = field(s, "tile").string_or("");
                if (!def.tile.empty() && read_floats(s, "velocity", def.velocity))
                    out.sky.stars.push_back(def);
            }
        }
        out.has_sky = !out.sky.mask.empty() && !out.sky.fill.empty();
    }
    if (const json::Value* layers = room.find("layers"); layers && layers->is_array()) {
        for (const json::Value& l : layers->as_array()) {
            const std::string path = l.string_or("");
            if (!path.empty()) out.layers.push_back(path);
        }
    }
}

int slot_at(const SpriteSheet& sheet, double seconds) {
    const long long tick = (long long)std::floor(seconds * (double)sheet.fps);
    return wrap(tick + sheet.offset, sheet.period);
}

const SpriteFrame* frame_at(const SpriteSheet& sheet, double seconds) {
    if (sheet.slot_frame.empty()) return nullptr;
    const int idx = sheet.slot_frame[(size_t)slot_at(sheet, seconds)];
    return idx < 0 ? nullptr : &sheet.frames[(size_t)idx];
}

float scroll_uv(float velocity_px, double seconds, float tile_px) {
    if (tile_px <= 0.0f) return 0.0f;
    const double travelled = std::fmod((double)velocity_px * seconds, (double)tile_px);
    return (float)(travelled / (double)tile_px);
}

}  // namespace room_anim

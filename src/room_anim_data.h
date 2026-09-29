#pragma once
// -----------------------------------------------------------------------------
// room_anim_data.h — pure data + timing for animated base-room art (#515).
//
// A room (assets/concourse/<type>/concourse.json rooms.<room>) may add:
//
//   "sky": { "mask": "anim/sky_mask.png",      // L8, 255 = see-through window
//            "fill": "anim/sky_fill.png",      // starless sky, drawn stretched
//            "stars": [ { "tile": "anim/stars_far.png", "velocity": [-1.6, 0.35] } ] },
//   "layers": [ "anim/car_receding.json", ... ]
//
// Sky: the painted plate gets the mask as alpha, and the fill + scrolling
// star tiles are drawn underneath it, so the stars drift behind the arches.
// Velocities are plate pixels per second.
//
// Layers: sprite sheets baked by tools/newcon_concourse/bake_layer.py. Every
// frame has an atlas `src` rect and a plate-pixel `dst` rect (src may be
// smaller: big frames are stored at half resolution). The timeline has
// `period_frames` slots at `fps`; slots without a frame draw nothing (the gap
// between passes). Sprites are encoded against the plate, so plain
// straight-alpha "over" reproduces their reflections and shadows exactly.
//
// No GPU or ImGui here — see room_anim.h for loading and drawing.
// -----------------------------------------------------------------------------

#include "json.h"

#include <string>
#include <vector>

namespace room_anim {

struct SpriteFrame {
    float src[4] = {0, 0, 0, 0};   // x, y, w, h in atlas pixels
    float dst[4] = {0, 0, 0, 0};   // x, y, w, h in plate (canvas) pixels
};

struct SpriteSheet {
    std::string              atlas;                 // relative to the manifest
    float                    canvas_w = 0.0f, canvas_h = 0.0f;
    float                    fps = 24.0f;
    int                      period = 1;            // timeline slots per loop
    int                      offset = 0;            // phase, in slots
    std::vector<int>         slot_frame;            // slot -> frames index, -1 = blank
    std::vector<SpriteFrame> frames;
};

struct StarLayerDef {
    std::string tile;
    float       velocity[2] = {0.0f, 0.0f};         // plate px / second
};

struct SkyDef {
    std::string               mask, fill;
    std::vector<StarLayerDef> stars;
};

struct RoomAnimDef {
    bool                     has_sky = false;
    SkyDef                   sky;
    std::vector<std::string> layers;                // manifest paths, room-relative
    bool empty() const { return !has_sky && layers.empty(); }
};

// Parse a bake_layer.py manifest. On failure returns false with `err` set.
bool parse_sprite_sheet(const json::Value& manifest, SpriteSheet& out, std::string& err);

// Read the optional "sky" / "layers" keys of a room object. Missing keys
// leave `out` empty; a present-but-malformed sky is dropped (has_sky=false).
void parse_room_anim(const json::Value& room, RoomAnimDef& out);

// Timeline slot shown at `seconds` (non-negative modulo the period).
int slot_at(const SpriteSheet& sheet, double seconds);

// Frame drawn at `seconds`, or nullptr during a blank gap.
const SpriteFrame* frame_at(const SpriteSheet& sheet, double seconds);

// Texture-space offset (0..1) of a tile of `tile_px` scrolling at
// `velocity_px` per second, wrapped so precision holds over long sessions.
float scroll_uv(float velocity_px, double seconds, float tile_px);

}  // namespace room_anim

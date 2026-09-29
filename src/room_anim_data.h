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
// A manifest may add "under": true (drawn beneath the plate, so it only
// shows through the sky mask) and "anchor": [cx, cy, r], the circle it was
// rendered against (#553). Anchored frames are remapped onto the room's own
// anchor, so one layer fits every framing of a scene.
//
// Per-plate rooms (#553): the New Con landing pad shows one of many
// full-frame composites, one per player hull. Its "composite" object holds
// the same keys plus "anchors" (json {"<plate>": [cx, cy, r]}), and any
// "{plate}" in its paths is replaced by the composite's name (for_plate).
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
    bool                     under = false;         // beneath the plate (seen through the sky)
    bool                     anchored = false;      // dst is relative to `anchor`
    float                    anchor[3] = {0, 0, 0}; // cx, cy, r rendered against
};

struct StarLayerDef {
    std::string tile;
    float       velocity[2] = {0.0f, 0.0f};         // plate px / second
    float       spin = 0.0f;                        // deg / second, clockwise on screen,
                                                    // about the anchor (else canvas centre)
};

struct SkyDef {
    std::string               mask, fill;
    std::vector<StarLayerDef> stars;
};

struct RoomAnimDef {
    bool                     has_sky = false;
    SkyDef                   sky;
    std::vector<std::string> layers;                // manifest paths, room-relative
    std::string              anchors;               // anchors json path ("" = none)
    std::string              plate;                 // set by for_plate()
    bool empty() const { return !has_sky && layers.empty(); }
};

// Parse a bake_layer.py manifest. On failure returns false with `err` set.
bool parse_sprite_sheet(const json::Value& manifest, SpriteSheet& out, std::string& err);

// Read the optional "sky" / "layers" keys of a room object. Missing keys
// leave `out` empty; a present-but-malformed sky is dropped (has_sky=false).
void parse_room_anim(const json::Value& room, RoomAnimDef& out);

// `def` for one named plate: every "{plate}" in its paths becomes `plate`.
RoomAnimDef for_plate(const RoomAnimDef& def, const std::string& plate);

// anchors[plate] as [cx, cy, r]; false if absent or malformed (r <= 0).
bool read_anchor(const json::Value& anchors, const std::string& plate, float (&out)[3]);

// Plate-pixel rect for frame `f`: its dst, remapped from the sheet's anchor
// onto `to` (cx, cy, r) when the sheet is anchored and `to` is given.
void place(const SpriteSheet& sheet, const SpriteFrame& f, const float* to, float (&out)[4]);

// Timeline slot shown at `seconds` (non-negative modulo the period).
int slot_at(const SpriteSheet& sheet, double seconds);

// Frame drawn at `seconds`, or nullptr during a blank gap.
const SpriteFrame* frame_at(const SpriteSheet& sheet, double seconds);

// Texture-space offset (0..1) of a tile of `tile_px` scrolling at
// `velocity_px` per second, wrapped so precision holds over long sessions.
float scroll_uv(float velocity_px, double seconds, float tile_px);

// Tile-space UVs of the plate corners (0,0), (w,0), (w,h), (0,h) for star
// layer `s` at `seconds`: drifting at its velocity and spinning about
// `centre` (plate px). The map is affine, so a quad with these corner UVs
// and a REPEAT sampler draws the field exactly. spin 0 = plain drift.
void star_uvs(const StarLayerDef& s, float tile_w, float tile_h, float canvas_w,
              float canvas_h, const float centre[2], double seconds, float (&uv)[4][2]);

}  // namespace room_anim

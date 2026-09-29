#pragma once
// -----------------------------------------------------------------------------
// room_anim.h — GPU side of animated base-room art (#515).
//
// Loads what room_anim_data.h describes and draws it around the painted plate:
//
//   draw_under()   sky fill + drifting star tiles (only if the room has a sky),
//                  then "under" sprite layers (e.g. ships out in space)
//   <plate>        the room background; with a sky it carries the mask as alpha
//   draw_over()    the other sprite layers (vehicles, pedestrians), JSON order
//
// Rooms without "sky"/"layers" keys load nothing and draw nothing, so every
// other base keeps its static art.
// -----------------------------------------------------------------------------

#include "json.h"
#include "material.h"
#include "room_anim_data.h"

#include <string>
#include <vector>

struct ImDrawList;

namespace room_anim {

struct SpriteLayer {
    SpriteSheet sheet;
    TextureSlot atlas;
    float       atlas_w = 1.0f, atlas_h = 1.0f;
};

struct StarLayer {
    TextureSlot tile;
    float       tile_w = 1.0f, tile_h = 1.0f;
    float       velocity[2] = {0.0f, 0.0f};
};

struct RoomAnim {
    float                    canvas_w = 0.0f, canvas_h = 0.0f;  // plate pixels
    TextureSlot              sky_fill;                          // valid => has a sky
    std::vector<StarLayer>   stars;
    std::vector<SpriteLayer> layers;
    bool                     has_anchor = false;                // plate's anchor known
    float                    anchor[3] = {0.0f, 0.0f, 0.0f};    // cx, cy, r
};

// Load the room background into `background` plus any animation into `out`.
// With a sky, the background is uploaded with the sky mask as its alpha;
// otherwise it is loaded exactly as before. Animation parts that fail to
// load are skipped with a log line. Returns false only if the background
// itself cannot be loaded.
bool load(const std::string& dir, const json::Value& room, TextureSlot& background,
          RoomAnim& out);

// The same for an explicit plate image (`plate_path`) animated by `def`,
// whose paths are relative to `dir` (see for_plate() for per-plate rooms).
// Anchored layers are skipped if def.anchors has no entry for def.plate.
bool load(const std::string& dir, const RoomAnimDef& def, const std::string& plate_path,
          TextureSlot& background, RoomAnim& out);

void draw_under(ImDrawList* dl, float screen_w, float screen_h, const RoomAnim& anim,
                double seconds);
void draw_over(ImDrawList* dl, float screen_w, float screen_h, const RoomAnim& anim,
               double seconds);

void release(RoomAnim& anim);

}  // namespace room_anim

#pragma once
// -----------------------------------------------------------------------------
// sprite_light_rec.h — "find more lights like this one" recommender for the
// F2 sprite light editor (issue #111).
//
// Problem: authoring ~5-10 lights across ~80 ship-atlas cells is thousands of
// repetitive clicks. Most lights sit on REPEATED hull features — the matching
// wingtip pod, the other engine intake, a symmetric antenna. Given one light
// the author already placed, this finds the top-K visually similar features
// ON THE SAME SPRITE and marks each at its CENTRE so they can be accepted in
// one keystroke.
//
// Backend (dependency-free, region-based): we do NOT use corner/keypoint
// detection (an earlier ORB/Harris attempt latched onto high-contrast edges
// instead of feature centres — wrong primitive). Instead:
//   * Segment the sprite into regions by union-find over adjacent-pixel
//     colour difference (groups smoothly-shaded materials, splits at sharp
//     edges; the `color_tol` knob is the merge threshold).
//   * Take the region under the query point as the template.
//   * Score every other region by colour similarity x size similarity.
//   * Non-max suppress on centroid distance, return the top-K region CENTROIDS.
//
// This is inherently mirror-symmetric (a port pod and its starboard twin have
// the same colour + area), places the marker dead-centre on the feature, and
// needs no scale/rotation machinery (v1 matches one canonical orientation).
// Pixels are read from the hull PNG on disk (offline authoring tool — a
// one-time load, then cached per sprite).
// -----------------------------------------------------------------------------

#include <string>
#include <vector>

namespace sprite_light_rec {

// One recommended spot, in the same UV space (0..1) the editor uses for
// LightSpot — the CENTROID of a matched region. `score` is in [0,1]
// (1 = identical colour and size); candidates are returned best-first.
struct Candidate {
    float u = 0.0f;
    float v = 0.0f;
    float score = 0.0f;
};

// Find up to `k` regions on `hull_png_path` whose colour + size best match the
// region under (query_u, query_v), returning each region's centroid in UV.
// Best-first, NMS-deduplicated, excluding the query region itself. Empty on
// any failure (missing file, decode error, query on transparent pixel).
//
// Two author-tunable knobs (both exposed as F2 sliders):
//   * `color_tol` — segmentation merge threshold in 0..255 RGB-distance
//     units. LOWER splits the sprite into smaller, more colour-specific
//     regions (separates a pod from the hull it sits on); HIGHER merges
//     similar shades into bigger regions.
//   * `patch_px` — the feature SCALE. The image is box-blurred by this
//     radius before segmentation (so tiny high-contrast specks get averaged
//     into their surrounding material instead of fragmenting it), and
//     regions smaller than ~a patch are dropped as noise. Bigger = coarser
//     (whole pods); smaller = finer (little intakes).
// Both are clamped internally.
//
// The decoded image + its segmentation are cached keyed by (path, color_tol,
// patch_px), so repeated calls at the same settings are effectively free.
std::vector<Candidate> find_candidates(const std::string& hull_png_path,
                                       float query_u, float query_v,
                                       int k, int color_tol, int patch_px);

// Drop cached analysis. clear_cache_for() invalidates a single sprite (call
// when its pixels could have changed); clear_cache() drops everything.
void clear_cache_for(const std::string& hull_png_path);
void clear_cache();

} // namespace sprite_light_rec

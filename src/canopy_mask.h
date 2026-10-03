// canopy_mask.h — where a cockpit painting leaves the canopy open (#732).
//
// World markers draw BEHIND the cockpit art (#429), so off-screen pointers
// clamped to the screen edge vanished under the frame: with art up, most of
// the screen edge is dashboard or strut. This is a coarse CPU copy of the
// art's alpha, so a pointer can stop where the glass ends instead: march from
// the gunsight toward the target and halt at the first metal.
//
// Pure maths plus one PNG loader, so tools/test_cockpit_overlay can check
// every hull against its real art without a GPU.
#pragma once

#include "cockpit_overlay_layout.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cockpit_overlay {

struct CanopyMask {
    int cell = 4;                 // art px per mask cell (each axis)
    int w = 0, h = 0;             // size in cells
    std::vector<uint8_t> metal;   // 1 = the cell has an opaque art px

    bool empty() const { return metal.empty(); }

    // True where the art is opaque. Outside the painting is open space: the
    // fit slides and stretches the art, it never letterboxes it.
    bool solid(Vec2 art) const {
        if (empty() || art.x < 0.0f || art.y < 0.0f) return false;
        const int x = (int)art.x / cell, y = (int)art.y / cell;
        return x < w && y < h && metal[(size_t)y * (size_t)w + (size_t)x];
    }
};

// Conservative downsample of tightly packed RGBA8: a cell is metal if ANY of
// its pixels is at least half opaque, so pointers never straddle a thin strut.
inline CanopyMask make_canopy_mask(const uint8_t* rgba, int w, int h, int cell = 4) {
    CanopyMask m;
    m.cell = cell;
    m.w = (w + cell - 1) / cell;
    m.h = (h + cell - 1) / cell;
    m.metal.assign((size_t)m.w * (size_t)m.h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (rgba[((size_t)y * (size_t)w + (size_t)x) * 4 + 3] >= 128)
                m.metal[(size_t)(y / cell) * (size_t)m.w + (size_t)(x / cell)] = 1;
    return m;
}

// Screen -> art px: undoes the fit and the pilot head sway, the exact inverse
// of how finalize() places the painting.
inline Homography screen_to_art(const Fit& f, const Homography& head) {
    Homography unfit;
    unfit.m[0] = 1.0f / f.scale_x; unfit.m[2] = -f.ox / f.scale_x;
    unfit.m[4] = 1.0f / f.scale_y; unfit.m[5] = -f.oy / f.scale_y;
    return multiply(unfit, inverse(head));
}

// How far (screen px) a ray from `from` along unit `dir` stays on open
// canopy: its last clear sample, capped at `max_px`. 0 when `from` itself is
// behind metal.
inline float clear_reach(const CanopyMask& m, const Homography& to_art,
                         Vec2 from, Vec2 dir, float max_px) {
    constexpr float kStep = 2.0f;   // screen px; finer than any strut we paint
    if (m.empty()) return max_px;
    for (float t = 0.0f; t < max_px; t += kStep)
        if (m.solid(apply(to_art, { from.x + dir.x * t, from.y + dir.y * t })))
            return std::max(0.0f, t - kStep);
    return max_px;
}

// Off-screen pointers never sit nearer the gun crosshair than this (logical
// px), even where a dash rides high (the Galaxy's is ~50 px below it).
constexpr float kMinPointerReach = 40.0f;

// How far from the viewport centre `c` along unit `dir` an off-screen pointer
// sits: out to the edge box `margin` px inside the viewport (half-size
// `half`), pulled back so a glyph reaching `glyph` px further out and
// `half_width` px to either side stays on canopy glass. Three parallel rays
// (centre and both flanks), because a ray grazing a near-flat dash edge
// reaches metal at the glyph's corners first. An empty mask (no art) keeps
// the classic screen edge.
inline float pointer_reach(const CanopyMask& m, const Homography& to_art, Vec2 c, Vec2 half,
                           Vec2 dir, float margin, float glyph, float half_width) {
    const float edge = std::min((half.x - margin) / std::max(std::fabs(dir.x), 1e-6f),
                                (half.y - margin) / std::max(std::fabs(dir.y), 1e-6f));
    float glass = edge + glyph;
    for (const float side : { -half_width, 0.0f, half_width })
        glass = std::min(glass, clear_reach(m, to_art, { c.x - dir.y * side, c.y + dir.x * side },
                                            dir, edge + glyph));
    return std::max(glass - glyph, std::min(edge, kMinPointerReach));
}

// Decode the art PNG at `path` into `out`. False (and `out` untouched) when
// the file is missing or unreadable.
bool load_canopy_mask(const std::string& path, CanopyMask& out);

} // namespace cockpit_overlay

// cockpit_overlay_layout.h — pure geometry for per-hull cockpit art (#426).
//
// A cockpit overlay is a full-screen RGBA painting of the canopy frame and
// dashboard. Its alpha is REAL: the canopy and the display glass are
// transparent holes, so space shows through the canopy and live instruments
// show through the displays. This header owns only the maths — which hull
// gets which art, how the art is fitted to the window, where each display
// hole lands on screen, and the perspective warp that maps a flat panel onto
// a skewed bezel — so tools/test_cockpit_overlay.cpp can check it without a
// GPU.
//
// Display quads are authored in ART pixels, measured from the PNG's alpha:
// straight edge lines least-squares fitted to the alpha=128 crossings (away
// from the rounded corners), corners = line intersections. Screen positions
// are ImGui LOGICAL pixels.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cockpit_overlay {

// Glass displays an overlay can carry: the three MFDs plus optional
// single-line strips (autopilot banner, commanded SET speed, KPS + AUTO).
// Everything shown in them is LIVE — art must never bake readouts in.
enum class Display { Left, Center, Right, Banner, SetSpeed, Velocity };
constexpr int kDisplayCount = 6;

struct Vec2 { float x = 0, y = 0; };
struct Rect { float x = 0, y = 0, w = 0, h = 0; };
// Corners in order TL, TR, BR, BL. All-zero = display absent.
struct Quad { Vec2 p[4]; };

constexpr Quad quad_from_rect(float x, float y, float w, float h) {
    return { { { x, y }, { x + w, y }, { x + w, y + h }, { x, y + h } } };
}

inline bool present(const Quad& q) {
    return (q.p[2].x - q.p[0].x) > 0.0f && (q.p[2].y - q.p[0].y) > 0.0f;
}

struct CockpitArt {
    const char* ship_class;      // ShipClass / PlayerState::ship_class_name
    const char* path;            // relative to the working dir (assets/...)
    float       art_w, art_h;    // authoring resolution of the quads below
    float       boresight_y;     // art row that should sit at screen centre
    Quad        display[kDisplayCount];   // indexed by Display
};

// One row per hull with cockpit art. Hulls without a row keep the classic
// free-floating HUD.
inline constexpr CockpitArt kCockpitArts[] = {
    // Talon: the amber industrial v7 cockpit (first built for the Centurion,
    // reassigned when the classic silhouette was approved). Near-frontal
    // MFDs — the side screens' outer top corners lean in ~1.5 deg (<= 2.8 px),
    // which the warp honours exactly. Canopy glass ends at y 335, so the
    // boresight rides at 260, clear of the dash.
    { "talon", "assets/cockpits/talon.png", 1280.0f, 720.0f, 260.0f,
      { { { { 305.8f, 462.2f }, { 428.7f, 462.1f }, { 427.2f, 546.1f }, { 302.7f, 547.0f } } },
        { { { 560.1f, 396.1f }, { 719.8f, 396.1f }, { 721.2f, 503.7f }, { 558.9f, 503.7f } } },
        { { { 846.0f, 462.1f }, { 969.2f, 462.3f }, { 971.3f, 547.8f }, { 846.3f, 546.1f } } },
        {}, {}, {} } },
    // Centurion: v10c classic Wing Commander silhouette (curved arch + triple
    // MFD), clean: no baked gunsight/readouts, space visible all round.
    // Pink-key-only alpha with soft edges; the dark dash metal stays solid.
    // Skewed bezels: side MFDs shear outward 3.6-7.1 deg, the centre one is
    // keystoned (287 px wide at the top, 304 at the bottom), and the small
    // SET / KPS boxes lean too — hence quads + the perspective warp. The
    // boresight is the canvas centre (clear canopy), so at 16:9 the art maps
    // 1:1 with no slide.
    { "centurion", "assets/cockpits/centurion.png", 1280.0f, 720.0f, 360.0f,
      { { { { 210.4f, 515.3f }, { 384.5f, 515.3f }, { 373.7f, 662.7f }, { 192.2f, 662.7f } } },
        { { { 511.4f, 475.3f }, { 797.6f, 475.3f }, { 806.4f, 712.7f }, { 502.3f, 712.7f } } },
        { { { 915.8f, 514.3f }, { 1092.2f, 514.3f }, { 1109.5f, 662.7f }, { 925.2f, 662.7f } } },
        { { { 539.8f, 150.3f }, { 761.4f, 148.7f }, { 759.8f, 178.8f }, { 541.9f, 178.7f } } },
        { { { 318.4f, 453.3f }, { 423.7f, 453.3f }, { 423.7f, 472.7f }, { 316.7f, 472.7f } } },
        { { { 871.1f, 452.3f }, { 981.3f, 452.3f }, { 982.4f, 472.4f }, { 871.4f, 473.6f } } } } },
};

inline const CockpitArt* find_art(const char* ship_class) {
    if (!ship_class) return nullptr;
    for (const CockpitArt& a : kCockpitArts)
        if (std::strcmp(a.ship_class, ship_class) == 0) return &a;
    return nullptr;
}

// ---- art -> screen fit ------------------------------------------------------
// screen = origin + art_px * scale.
struct Fit { float scale = 1.0f, ox = 0.0f, oy = 0.0f; };

inline Vec2 to_screen(const Fit& f, Vec2 art) {
    return { f.ox + art.x * f.scale, f.oy + art.y * f.scale };
}
inline Quad to_screen(const Fit& f, const Quad& q) {
    Quad s;
    for (int i = 0; i < 4; ++i) s.p[i] = to_screen(f, q.p[i]);
    return s;
}
inline Rect to_screen(const Fit& f, const Rect& r) {
    return { f.ox + r.x * f.scale, f.oy + r.y * f.scale, r.w * f.scale, r.h * f.scale };
}

// Lowest art row any present display reaches.
inline float display_bottom(const CockpitArt& a) {
    float bottom = 0.0f;
    for (const Quad& q : a.display)
        if (present(q))
            for (const Vec2& p : q.p) bottom = std::max(bottom, p.y);
    return bottom;
}

// Cover-and-slide fit: the art spans the viewport (no letterbox), centred
// horizontally. Vertically it slides so the boresight lands on the screen
// centre — everything outside the frame is transparent space, so the slide
// never opens a visible seam at the top. Two clamps, in priority order:
// never lift the art's bottom edge off the viewport (dash must reach the
// bottom), and keep every display on screen.
//
// Very wide windows: covering the width would scale the art so far that the
// boresight could no longer sit at centre with the lowest display still on
// screen (the dash would ride up over the gunsight). The scale is capped
// there instead (Hor+): extra width just shows more space at the sides.
inline Fit fit_to_viewport(const CockpitArt& a, float vp_x, float vp_y,
                           float vp_w, float vp_h) {
    Fit f;
    const float cover = std::max(vp_w / a.art_w, vp_h / a.art_h);
    const float below = display_bottom(a) - a.boresight_y;
    const float cap   = below > 0.0f ? (vp_h * 0.5f) / below : cover;
    f.scale = std::max(vp_h / a.art_h, std::min(cover, cap));
    f.ox = vp_x + (vp_w - a.art_w * f.scale) * 0.5f;
    const float bottom = vp_y + vp_h;
    f.oy = vp_y + vp_h * 0.5f - a.boresight_y * f.scale;
    f.oy = std::min(f.oy, bottom - display_bottom(a) * f.scale);
    f.oy = std::max(f.oy, bottom - a.art_h * f.scale);
    return f;
}

// ---- perspective warp ------------------------------------------------------
// Instruments are laid out flat in an axis-aligned "panel" rect, then every
// vertex is pushed through a homography onto the display's screen quad.
// The panel rect is the quad's AVERAGE rect (mean of opposite edges), so the
// warp stays close to identity and ImGui's flat hit-testing is near-right
// even before the exact inverse mapping is applied to the mouse.
inline Rect panel_rect(const Quad& q) {
    const float x0 = (q.p[0].x + q.p[3].x) * 0.5f, x1 = (q.p[1].x + q.p[2].x) * 0.5f;
    const float y0 = (q.p[0].y + q.p[1].y) * 0.5f, y1 = (q.p[3].y + q.p[2].y) * 0.5f;
    return { x0, y0, x1 - x0, y1 - y0 };
}

// Row-major 3x3: (x, y, 1) -> (X, Y, W), result (X/W, Y/W).
struct Homography { float m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; };

inline Vec2 apply(const Homography& h, Vec2 p) {
    const float* m = h.m;
    const float w = m[6] * p.x + m[7] * p.y + m[8];
    return { (m[0] * p.x + m[1] * p.y + m[2]) / w,
             (m[3] * p.x + m[4] * p.y + m[5]) / w };
}

inline Homography multiply(const Homography& a, const Homography& b) {
    Homography r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i * 3 + j] = a.m[i * 3 + 0] * b.m[0 * 3 + j] +
                             a.m[i * 3 + 1] * b.m[1 * 3 + j] +
                             a.m[i * 3 + 2] * b.m[2 * 3 + j];
    return r;
}

inline Homography inverse(const Homography& h) {
    const float* m = h.m;
    const float c0 = m[4] * m[8] - m[5] * m[7];
    const float c1 = m[5] * m[6] - m[3] * m[8];
    const float c2 = m[3] * m[7] - m[4] * m[6];
    const float det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    const float k = det != 0.0f ? 1.0f / det : 0.0f;
    Homography r;
    r.m[0] = c0 * k; r.m[1] = (m[2] * m[7] - m[1] * m[8]) * k; r.m[2] = (m[1] * m[5] - m[2] * m[4]) * k;
    r.m[3] = c1 * k; r.m[4] = (m[0] * m[8] - m[2] * m[6]) * k; r.m[5] = (m[2] * m[3] - m[0] * m[5]) * k;
    r.m[6] = c2 * k; r.m[7] = (m[1] * m[6] - m[0] * m[7]) * k; r.m[8] = (m[0] * m[4] - m[1] * m[3]) * k;
    return r;
}

// Unit square (0,0),(1,0),(1,1),(0,1) -> quad TL,TR,BR,BL (Heckbert's
// projective mapping). Degenerates gracefully to affine when the quad is a
// parallelogram (g = h = 0).
inline Homography unit_square_to_quad(const Quad& q) {
    const float x0 = q.p[0].x, y0 = q.p[0].y, x1 = q.p[1].x, y1 = q.p[1].y;
    const float x2 = q.p[2].x, y2 = q.p[2].y, x3 = q.p[3].x, y3 = q.p[3].y;
    const float sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    const float dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
    const float den = dx1 * dy2 - dx2 * dy1;
    const float g = den != 0.0f ? (sx * dy2 - dx2 * sy) / den : 0.0f;
    const float h = den != 0.0f ? (dx1 * sy - sx * dy1) / den : 0.0f;
    Homography r;
    r.m[0] = x1 - x0 + g * x1; r.m[1] = x3 - x0 + h * x3; r.m[2] = x0;
    r.m[3] = y1 - y0 + g * y1; r.m[4] = y3 - y0 + h * y3; r.m[5] = y0;
    r.m[6] = g;                r.m[7] = h;                r.m[8] = 1.0f;
    return r;
}

// Flat panel rect -> skewed screen quad.
inline Homography rect_to_quad(const Rect& r, const Quad& q) {
    Homography to_unit;
    to_unit.m[0] = 1.0f / r.w; to_unit.m[2] = -r.x / r.w;
    to_unit.m[4] = 1.0f / r.h; to_unit.m[5] = -r.y / r.h;
    return multiply(unit_square_to_quad(q), to_unit);
}

// Shrink (or with a negative amount, grow) a rect on every side.
inline Rect inset(const Rect& r, float by) {
    return { r.x + by, r.y + by,
             std::max(0.0f, r.w - 2.0f * by), std::max(0.0f, r.h - 2.0f * by) };
}

// The centre MFD carries RADAR plus two flank strips for compact flight
// readouts (speed / ordnance left, energy right) — Privateer's gauges-round-
// the-radar. The disc takes a square of at most 62% of the width so the
// flanks keep legible room even on a near-square MFD.
struct RadarSplit { Rect left, disc, right; };

inline RadarSplit split_radar(const Rect& r) {
    const float side  = std::min(r.h, r.w * 0.62f);
    const float flank = std::max(0.0f, (r.w - side) * 0.5f);
    return { { r.x,                r.y, flank, r.h },
             { r.x + flank,        r.y, side,  r.h },
             { r.x + flank + side, r.y, flank, r.h } };
}

} // namespace cockpit_overlay

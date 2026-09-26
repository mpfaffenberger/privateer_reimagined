// cockpit_overlay_layout.h — pure geometry for per-hull cockpit art (#426).
//
// A cockpit overlay is a full-screen RGBA painting of the canopy frame and
// dashboard. Its alpha is REAL: the canopy and the MFD glass are transparent
// holes, so space shows through the canopy and live instruments show
// through the MFDs. This header owns only the maths — which hull gets which
// art, how the art is fitted to the window, and where each MFD hole lands on
// screen — so tools/test_cockpit_overlay.cpp can check it without a GPU.
//
// Art rects are authored in ART pixels, measured from the PNG's alpha (flood
// fill of alpha < 128). Screen rects are ImGui LOGICAL pixels.
#pragma once

#include <algorithm>
#include <cstring>

namespace cockpit_overlay {

enum class Mfd { Left, Center, Right };
constexpr int kMfdCount = 3;

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

struct CockpitArt {
    const char* ship_class;     // ShipClass / PlayerState::ship_class_name
    const char* path;           // relative to the working dir (assets/...)
    float       art_w, art_h;   // authoring resolution of the rects below
    float       boresight_y;    // art row that should sit at screen centre
    Rect        mfd[kMfdCount]; // glass holes, indexed by Mfd
};

// One row per hull with cockpit art. Hulls without a row keep the classic
// free-floating HUD. Centurion MFD rects measured from centurion.png alpha:
// left 303..428 x 462..546, centre 559..720 x 396..503, right 846..971 x
// 462..547 (inclusive), each ~97.6% of its bbox (softly rounded corners).
// Boresight: the centre-column canopy glass spans y 110..335 and the dash
// starts at 336, so a plain centred fit (screen centre = y 360) would park
// the gun crosshair ON the dashboard. y 260 keeps the crosshair + nav
// reticle ~75 art px clear of the dash while cropping as little as possible.
inline constexpr CockpitArt kCockpitArts[] = {
    { "centurion", "assets/cockpits/centurion.png", 1280.0f, 720.0f, 260.0f,
      { { 303.0f, 462.0f, 126.0f,  85.0f },
        { 559.0f, 396.0f, 162.0f, 108.0f },
        { 846.0f, 462.0f, 126.0f,  86.0f } } },
};

inline const CockpitArt* find_art(const char* ship_class) {
    if (!ship_class) return nullptr;
    for (const CockpitArt& a : kCockpitArts)
        if (std::strcmp(a.ship_class, ship_class) == 0) return &a;
    return nullptr;
}

// Art -> screen mapping: screen = origin + art_px * scale.
struct Fit { float scale = 1.0f, ox = 0.0f, oy = 0.0f; };

// Lowest art row any MFD glass reaches.
inline float mfd_bottom(const CockpitArt& a) {
    float b = 0.0f;
    for (const Rect& r : a.mfd) b = std::max(b, r.y + r.h);
    return b;
}

// Cover-scaled fit: the art always spans the viewport (no letterbox bars),
// centred horizontally. Vertically it slides so `boresight_y` lands on the
// screen centre — where the camera looks and the guns converge — because
// everything outside the canopy frame is transparent space anyway, so the
// slide never opens a visible seam at the top. Two clamps, in priority
// order: never lift the art's bottom edge off the viewport (dash must reach
// the bottom), and keep every MFD fully on screen (ultrawide windows).
// MFD rects follow the same transform, so they stay glued to the art.
inline Fit fit_to_viewport(const CockpitArt& a, float vp_x, float vp_y,
                           float vp_w, float vp_h) {
    Fit f;
    f.scale = std::max(vp_w / a.art_w, vp_h / a.art_h);
    f.ox = vp_x + (vp_w - a.art_w * f.scale) * 0.5f;
    const float bottom = vp_y + vp_h;
    f.oy = vp_y + vp_h * 0.5f - a.boresight_y * f.scale;
    f.oy = std::min(f.oy, bottom - mfd_bottom(a) * f.scale);
    f.oy = std::max(f.oy, bottom - a.art_h * f.scale);
    return f;
}

inline Rect to_screen(const Fit& f, const Rect& art) {
    return { f.ox + art.x * f.scale, f.oy + art.y * f.scale,
             art.w * f.scale,        art.h * f.scale };
}

// Shrink (or with a negative amount, grow) a rect on every side.
inline Rect inset(const Rect& r, float by) {
    return { r.x + by, r.y + by,
             std::max(0.0f, r.w - 2.0f * by), std::max(0.0f, r.h - 2.0f * by) };
}

// The centre MFD is wider than tall: the radar disc takes the square middle
// and the spare width becomes two flank strips for compact flight readouts
// (speed / ordnance left, energy right) — Privateer's gauges-round-the-radar.
struct RadarSplit { Rect left, disc, right; };

inline RadarSplit split_radar(const Rect& r) {
    const float side = std::min(r.w, r.h);
    const float flank = std::max(0.0f, (r.w - side) * 0.5f);
    return { { r.x,                r.y, flank, r.h },
             { r.x + flank,        r.y, side,  r.h },
             { r.x + flank + side, r.y, flank, r.h } };
}

} // namespace cockpit_overlay

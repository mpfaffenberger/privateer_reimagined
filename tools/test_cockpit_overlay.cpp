// Pure cockpit-art geometry harness (#426). Run from the repo root:
//   cmake --build build --target test_cockpit_overlay && ./build/test_cockpit_overlay
//
// Checks the viewport fit, that every MFD hole stays on screen at common
// aspects, and — against the real PNG — that each table rect really is
// transparent glass ringed by opaque bezel (so a re-export of the art can't
// silently leave the instruments floating over the dashboard) and that the
// screen centre (gun boresight) always looks through canopy glass.
//
// It also guards the flat-window design: cockpit_mfd pins axis-aligned
// ImGui windows to the holes, which is only right while the glass is a
// frontal rectangle. Every art row's holes are measured (edge lines fitted
// to the alpha, corners intersected) and must sit within
// kFlatTolerancePx of their table rect. Genuinely skewed/perspective MFDs
// would fail here, and then need a perspective quad path, not a nudge.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "cockpit_overlay_layout.h"

#include <cmath>
#include <cstdio>

using namespace cockpit_overlay;

namespace {

int g_failures = 0;
void check(bool ok, const char* label) {
    std::printf("%-62s %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}
bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

// Fraction of pixels in `r` (art px) whose alpha satisfies want_clear.
float alpha_fraction(const unsigned char* px, int w, int h, const Rect& r,
                     bool want_clear) {
    int hit = 0, total = 0;
    for (int y = (int)r.y; y < (int)(r.y + r.h); ++y)
        for (int x = (int)r.x; x < (int)(r.x + r.w); ++x) {
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            const bool clear = px[(y * w + x) * 4 + 3] < 128;
            hit += (clear == want_clear);
            ++total;
        }
    return total ? (float)hit / (float)total : 0.0f;
}

// Fraction of opaque pixels on the 1px ring `by` px outside `r`.
float ring_opaque(const unsigned char* px, int w, int h, const Rect& r, int by) {
    int hit = 0, total = 0;
    const int x0 = (int)r.x - by, x1 = (int)(r.x + r.w) - 1 + by;
    const int y0 = (int)r.y - by, y1 = (int)(r.y + r.h) - 1 + by;
    auto sample = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        hit += px[(y * w + x) * 4 + 3] >= 128;
        ++total;
    };
    for (int x = x0; x <= x1; ++x) { sample(x, y0); sample(x, y1); }
    for (int y = y0 + 1; y < y1; ++y) { sample(x0, y); sample(x1, y); }
    return total ? (float)hit / (float)total : 0.0f;
}

// ---- hole flatness (perspective guard) -------------------------------------
// Max art px a measured hole corner may sit off its table rect and still be
// served by a flat window: the MFD content padding (5 logical px) plus the
// glass's rounded corners absorb this much lean without visible overlap.
constexpr float kFlatTolerancePx = 4.0f;

struct AlphaImage {
    const unsigned char* px; int w, h;
    int at(int x, int y) const { return px[(y * w + x) * 4 + 3]; }
};

// Sub-pixel coordinate (pixel-centre convention) along the walk axis where
// alpha crosses 128, walking from (x,y) in steps of (dx,dy). NAN if none.
float edge_crossing(const AlphaImage& a, int x, int y, int dx, int dy, int limit) {
    int prev = a.at(x, y);
    for (int i = 1; i <= limit; ++i) {
        const int nx = x + dx * i, ny = y + dy * i;
        if (nx < 0 || ny < 0 || nx >= a.w || ny >= a.h) break;
        const int cur = a.at(nx, ny);
        if ((prev >= 128) != (cur >= 128)) {
            const float t = (128.0f - prev) / (float)(cur - prev);
            const int   from = dx ? nx - dx : ny - dy;   // walk-axis index of prev
            return (float)from + 0.5f + t * (float)(dx ? dx : dy);
        }
        prev = cur;
    }
    return NAN;
}

// Least-squares u = k*v + c.
struct Line { float k = 0, c = 0; };
Line fit_line(const float* v, const float* u, int n) {
    double mv = 0, mu = 0;
    for (int i = 0; i < n; ++i) { mv += v[i]; mu += u[i]; }
    mv /= n; mu /= n;
    double sxy = 0, sxx = 0;
    for (int i = 0; i < n; ++i) { sxy += (v[i] - mv) * (u[i] - mu); sxx += (v[i] - mv) * (v[i] - mv); }
    const double k = sxx > 0 ? sxy / sxx : 0.0;
    return { (float)k, (float)(mu - k * mv) };
}

// Largest corner offset between the hole's measured quad and `r`. Edges are
// sampled 10 px in from the rounded corners; side edges are x = k*y + c,
// top/bottom are y = k*x + c; corners are their intersections.
float max_corner_offset(const AlphaImage& a, const Rect& r) {
    const int x0 = (int)r.x, y0 = (int)r.y, x1 = (int)(r.x + r.w) - 1, y1 = (int)(r.y + r.h) - 1;
    const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, reach = (int)std::max(r.w, r.h);
    float v[512], le[512], ri[512], to[512], bo[512];
    int nr = 0, nc = 0;
    for (int y = y0 + 10; y <= y1 - 10 && nr < 512; ++y, ++nr) {
        v[nr]  = (float)y + 0.5f;
        le[nr] = edge_crossing(a, cx, y, -1, 0, reach);
        ri[nr] = edge_crossing(a, cx, y, +1, 0, reach);
        if (std::isnan(le[nr]) || std::isnan(ri[nr])) return INFINITY;
    }
    const Line L = fit_line(v, le, nr), R = fit_line(v, ri, nr);
    for (int x = x0 + 10; x <= x1 - 10 && nc < 512; ++x, ++nc) {
        v[nc]  = (float)x + 0.5f;
        to[nc] = edge_crossing(a, x, cy, 0, -1, reach);
        bo[nc] = edge_crossing(a, x, cy, 0, +1, reach);
        if (std::isnan(to[nc]) || std::isnan(bo[nc])) return INFINITY;
    }
    const Line T = fit_line(v, to, nc), B = fit_line(v, bo, nc);
    auto corner = [](Line side, Line cap, float& x, float& y) {
        y = (cap.k * side.c + cap.c) / (1.0f - cap.k * side.k);
        x = side.k * y + side.c;
    };
    const float rx[4] = { r.x, r.x + r.w, r.x + r.w, r.x };
    const float ry[4] = { r.y, r.y,       r.y + r.h, r.y + r.h };
    const Line  sides[4] = { L, R, R, L }, caps[4] = { T, T, B, B };
    float worst = 0.0f;
    for (int i = 0; i < 4; ++i) {
        float x, y;
        corner(sides[i], caps[i], x, y);
        worst = std::max(worst, std::hypot(x - rx[i], y - ry[i]));
    }
    return worst;
}

} // namespace

int main() {
    const CockpitArt* art = find_art("centurion");
    check(art != nullptr, "centurion has cockpit art");
    check(find_art("tarsus") == nullptr, "hull without art keeps classic HUD");
    check(find_art(nullptr) == nullptr, "null class is safe");
    if (!art) return 1;

    // Native size: 1:1, slid down so boresight row 260 sits on centre 360.
    const Fit native = fit_to_viewport(*art, 0, 0, 1280, 720);
    check(near(native.scale, 1.0f) && near(native.ox, 0) && near(native.oy, 100.0f),
          "1280x720: 1:1, boresight slid onto screen centre");

    // 16:10 is height-bound: scale 1.25, 80px cropped off each side.
    const Fit f1610 = fit_to_viewport(*art, 0, 0, 1440, 900);
    check(near(f1610.scale, 1.25f) && near(f1610.ox, -80.0f) && near(f1610.oy, 125.0f),
          "16:10 covers by height, crops sides evenly");
    const Rect left = to_screen(f1610, art->mfd[(int)Mfd::Left]);
    check(near(left.x, 298.75f) && near(left.w, 157.5f),
          "MFD rect follows the art transform");

    // Every hole stays fully on screen for common window shapes.
    const float shapes[][2] = { {1024, 768}, {1440, 900}, {1512, 982},
                                {1920, 1080}, {2560, 1080}, {800, 600} };
    bool all_visible = true, bottom_covered = true, sides_covered = true;
    for (const auto& s : shapes) {
        const Fit f = fit_to_viewport(*art, 0, 0, s[0], s[1]);
        for (const Rect& hole : art->mfd) {
            const Rect r = to_screen(f, hole);
            all_visible &= r.x >= 0 && r.y >= 0 &&
                           r.x + r.w <= s[0] && r.y + r.h <= s[1];
        }
        bottom_covered &= f.oy + art->art_h * f.scale >= s[1] - 0.01f;
        sides_covered  &= f.ox <= 0.01f && f.ox + art->art_w * f.scale >= s[0] - 0.01f;
    }
    check(all_visible,    "all MFD holes on screen from 4:3 to 21:9");
    check(bottom_covered, "dash always reaches the bottom edge");
    check(sides_covered,  "art always spans the full width");

    // Radar split: square disc in the middle, equal flanks, no gaps.
    const Rect centre = to_screen(native, art->mfd[(int)Mfd::Center]);
    const RadarSplit sp = split_radar(centre);
    check(near(sp.disc.w, centre.h) && near(sp.left.w, sp.right.w) &&
          near(sp.left.w + sp.disc.w + sp.right.w, centre.w),
          "radar split = square disc + equal flanks");

    // The table must match the real alpha channel.
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load(art->path, &w, &h, &n, 4);
    check(px != nullptr, "centurion.png loads (run from repo root)");
    if (px) {
        check(w == (int)art->art_w && h == (int)art->art_h,
              "PNG size matches authoring resolution");
        const char* names[] = { "left", "centre", "right" };
        for (int i = 0; i < kMfdCount; ++i) {
            char label[96];
            const float glass = alpha_fraction(px, w, h, inset(art->mfd[i], 3.0f), true);
            std::snprintf(label, sizeof(label), "%s MFD interior is real alpha glass (%.3f)",
                          names[i], glass);
            check(glass > 0.99f, label);
            const float bezel = ring_opaque(px, w, h, art->mfd[i], 3);
            std::snprintf(label, sizeof(label), "%s MFD ringed by opaque bezel (%.3f)",
                          names[i], bezel);
            check(bezel > 0.95f, label);
        }
        // Screen centre -> art space must be canopy glass for every shape,
        // with a margin box so the crosshair/reticle isn't clipped either.
        bool boresight_clear = true;
        for (const auto& s : shapes) {
            const Fit f = fit_to_viewport(*art, 0, 0, s[0], s[1]);
            const float ax = (s[0] * 0.5f - f.ox) / f.scale;
            const float ay = (s[1] * 0.5f - f.oy) / f.scale;
            boresight_clear &=
                alpha_fraction(px, w, h, { ax - 30, ay - 30, 60, 60 }, true) > 0.999f;
        }
        check(boresight_clear, "boresight looks through canopy glass at every aspect");
        stbi_image_free(px);
    }

    // Perspective guard, for EVERY art row (not just the Centurion).
    for (const CockpitArt& a : kCockpitArts) {
        int iw = 0, ih = 0, ic = 0;
        unsigned char* ipx = stbi_load(a.path, &iw, &ih, &ic, 4);
        char label[112];
        if (!ipx) {
            std::snprintf(label, sizeof(label), "%s art loads for flatness check", a.ship_class);
            check(false, label);
            continue;
        }
        const AlphaImage img{ ipx, iw, ih };
        const char* names[] = { "left", "centre", "right" };
        for (int i = 0; i < kMfdCount; ++i) {
            const float off = max_corner_offset(img, a.mfd[i]);
            std::snprintf(label, sizeof(label), "%s %s MFD is a frontal rect (worst corner %.2f px)",
                          a.ship_class, names[i], off);
            check(off <= kFlatTolerancePx, label);
        }
        stbi_image_free(ipx);
    }

    std::printf("\n%s\n", g_failures ? "FAIL" : "ALL PASS");
    return g_failures ? 1 : 0;
}

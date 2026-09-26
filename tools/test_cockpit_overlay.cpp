// Pure cockpit-art geometry harness (#426). Run from the repo root:
//   cmake --build build --target test_cockpit_overlay && ./build/test_cockpit_overlay
//
// Checks, for EVERY art row, against its real PNG alpha:
//   * each display quad matches the glass edges re-measured from the alpha
//     (lines fitted to the alpha=128 crossings, corners intersected) — so a
//     re-export of the art can't silently leave instruments off the bezels;
//   * the quad interior is transparent glass and a ring just outside it is
//     opaque bezel (the art on top masks warped-panel overhang);
//   * the screen centre (gun boresight) looks through canopy glass — and,
//     for art that paints its own gunsight, lands exactly on it.
// Plus the fit and homography maths.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "cockpit_overlay_layout.h"

#include <cmath>
#include <cstdio>

using namespace cockpit_overlay;

namespace {

int g_failures = 0;
void check(bool ok, const char* label) {
    std::printf("%-66s %s\n", label, ok ? "PASS" : "FAIL");
    if (!ok) ++g_failures;
}
bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

const char* kNames[kDisplayCount] = { "left", "centre", "right", "banner", "speed" };
const float kShapes[][2] = { {1024, 768}, {1280, 720}, {1280, 813}, {1440, 900},
                             {1512, 982}, {1920, 1080}, {2560, 1080}, {800, 600} };

struct AlphaImage {
    unsigned char* px = nullptr; int w = 0, h = 0;
    int  at(int x, int y) const { return px[(y * w + x) * 4 + 3]; }
    bool clear(float x, float y) const {
        const int ix = (int)std::floor(x), iy = (int)std::floor(y);
        return ix >= 0 && iy >= 0 && ix < w && iy < h && at(ix, iy) < 128;
    }
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
            const int from = dx ? nx - dx : ny - dy;
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

// Re-measure a display's glass from the alpha, seeded by the table quad's
// bbox; edges sampled over their middle 60% (clear of the rounded corners).
// Returns the worst corner distance to the table quad (INFINITY if lost).
float worst_corner_offset(const AlphaImage& a, const Quad& q) {
    float x0 = q.p[0].x, x1 = x0, y0 = q.p[0].y, y1 = y0;
    for (const Vec2& p : q.p) {
        x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
    }
    const int cx = (int)((x0 + x1) * 0.5f), cy = (int)((y0 + y1) * 0.5f);
    const int reach = (int)std::max(x1 - x0, y1 - y0);
    const int my = std::max(2, (int)((y1 - y0) * 0.2f)), mx = std::max(2, (int)((x1 - x0) * 0.2f));
    float v[1024], lo[1024], hi[1024];
    int n = 0;
    for (int y = (int)y0 + my; y <= (int)y1 - my && n < 1024; ++y, ++n) {
        v[n]  = (float)y + 0.5f;
        lo[n] = edge_crossing(a, cx, y, -1, 0, reach);
        hi[n] = edge_crossing(a, cx, y, +1, 0, reach);
        if (std::isnan(lo[n]) || std::isnan(hi[n])) return INFINITY;
    }
    const Line L = fit_line(v, lo, n), R = fit_line(v, hi, n);
    n = 0;
    for (int x = (int)x0 + mx; x <= (int)x1 - mx && n < 1024; ++x, ++n) {
        v[n]  = (float)x + 0.5f;
        lo[n] = edge_crossing(a, x, cy, 0, -1, reach);
        hi[n] = edge_crossing(a, x, cy, 0, +1, reach);
        if (std::isnan(lo[n]) || std::isnan(hi[n])) return INFINITY;
    }
    const Line T = fit_line(v, lo, n), B = fit_line(v, hi, n);
    const Line sides[4] = { L, R, R, L }, caps[4] = { T, T, B, B };
    float worst = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float y = (caps[i].k * sides[i].c + caps[i].c) / (1.0f - caps[i].k * sides[i].k);
        const float x = sides[i].k * y + sides[i].c;
        worst = std::max(worst, std::hypot(x - q.p[i].x, y - q.p[i].y));
    }
    return worst;
}

// Fraction of samples on the unit-square grid [lo,hi]^2, mapped through the
// quad's projective map, that are transparent (want_clear) / opaque.
float quad_fraction(const AlphaImage& a, const Quad& q, float lo, float hi, bool want_clear) {
    const Homography m = unit_square_to_quad(q);
    int hit = 0, total = 0;
    for (int j = 0; j <= 40; ++j)
        for (int i = 0; i <= 40; ++i) {
            const Vec2 p = apply(m, { lo + (hi - lo) * i / 40.0f, lo + (hi - lo) * j / 40.0f });
            hit += a.clear(p.x, p.y) == want_clear;
            ++total;
        }
    return (float)hit / (float)total;
}

// Fraction of opaque samples on the quad's edges pushed `by` art px outward.
float bezel_ring(const AlphaImage& a, const Quad& q, float by) {
    int hit = 0, total = 0;
    Vec2 c{ 0, 0 };
    for (const Vec2& p : q.p) { c.x += p.x * 0.25f; c.y += p.y * 0.25f; }
    for (int e = 0; e < 4; ++e) {
        const Vec2 p0 = q.p[e], p1 = q.p[(e + 1) % 4];
        for (int i = 1; i < 40; ++i) {
            const float t = i / 40.0f;
            Vec2 p{ p0.x + (p1.x - p0.x) * t, p0.y + (p1.y - p0.y) * t };
            const float dx = p.x - c.x, dy = p.y - c.y, len = std::hypot(dx, dy);
            // Push outward along the edge normal (sign chosen away from centre).
            float nx = -(p1.y - p0.y), ny = p1.x - p0.x;
            const float nl = std::hypot(nx, ny);
            nx /= nl; ny /= nl;
            if (nx * dx + ny * dy < 0) { nx = -nx; ny = -ny; }
            (void)len;
            p.x += nx * by; p.y += ny * by;
            hit += !a.clear(p.x, p.y);
            ++total;
        }
    }
    return (float)hit / (float)total;
}

void check_maths() {
    // Homography: corners exact, inverse round-trips, rect->itself = identity.
    const Quad q{ { { 206.9f, 427.0f }, { 380.3f, 427.0f }, { 369.4f, 576.0f }, { 189.6f, 576.0f } } };
    const Rect r = panel_rect(q);
    const Homography h = rect_to_quad(r, q), hi = inverse(h);
    const Vec2 rc[4] = { { r.x, r.y }, { r.x + r.w, r.y }, { r.x + r.w, r.y + r.h }, { r.x, r.y + r.h } };
    bool corners = true, round_trip = true;
    for (int i = 0; i < 4; ++i) {
        const Vec2 s = apply(h, rc[i]);
        corners &= near(s.x, q.p[i].x, 0.01f) && near(s.y, q.p[i].y, 0.01f);
        const Vec2 back = apply(hi, s);
        round_trip &= near(back.x, rc[i].x, 0.01f) && near(back.y, rc[i].y, 0.01f);
    }
    check(corners,    "homography maps panel corners onto the skewed quad");
    check(round_trip, "inverse homography round-trips (mouse remap)");
    const Vec2 mid = apply(h, { r.x + r.w * 0.5f, r.y + r.h * 0.5f });
    check(mid.x > r.x && mid.x < r.x + r.w && mid.y > r.y && mid.y < r.y + r.h,
          "warp keeps the panel centre inside its glass");
    const Rect flat{ 10, 20, 30, 40 };
    const Homography id = rect_to_quad(flat, quad_from_rect(10, 20, 30, 40));
    const Vec2 p = apply(id, { 17, 33 });
    check(near(p.x, 17) && near(p.y, 33), "rect onto itself is the identity");

    // Radar split: square disc, equal flanks, no gaps, flanks >= 19% each.
    const RadarSplit sp = split_radar({ 0, 0, 300, 243 });
    check(near(sp.disc.w, 186.0f) && near(sp.left.w, sp.right.w) &&
          near(sp.left.w + sp.disc.w + sp.right.w, 300.0f),
          "radar split = capped square disc + equal flanks");
}

void check_fit(const CockpitArt& art) {
    char label[128];
    bool visible = true, bottom = true, sides = true, pinned = true;
    for (const auto& s : kShapes) {
        const Fit f = fit_to_viewport(art, 0, 0, s[0], s[1]);
        for (const Quad& q : art.display) {
            if (!present(q)) continue;
            for (const Vec2& c : q.p) {
                const Vec2 p = to_screen(f, c);
                visible &= p.x >= -0.01f && p.y >= -0.01f &&
                           p.x <= s[0] + 0.01f && p.y <= s[1] + 0.01f;
            }
        }
        bottom &= f.oy + art.art_h * f.scale >= s[1] - 0.01f;
        sides  &= f.ox <= 0.01f && f.ox + art.art_w * f.scale >= s[0] - 0.01f;
        const Vec2 b = to_screen(f, art.boresight);
        pinned &= near(b.x, s[0] * 0.5f, 0.05f) && near(b.y, s[1] * 0.5f, 0.05f);
    }
    std::snprintf(label, sizeof(label), "%s: every display on screen, 4:3 to 21:9", art.ship_class);
    check(visible, label);
    std::snprintf(label, sizeof(label), "%s: dash always reaches the bottom edge", art.ship_class);
    check(bottom, label);
    if (art.painted_gunsight) {
        std::snprintf(label, sizeof(label), "%s: painted gunsight pinned to screen centre", art.ship_class);
        check(pinned, label);
    } else {
        std::snprintf(label, sizeof(label), "%s: art always spans the full width", art.ship_class);
        check(sides, label);
    }
}

void check_art_alpha(const CockpitArt& art) {
    char label[128];
    AlphaImage img;
    int n = 0;
    img.px = stbi_load(art.path, &img.w, &img.h, &n, 4);
    std::snprintf(label, sizeof(label), "%s: %s loads (run from repo root)", art.ship_class, art.path);
    check(img.px != nullptr, label);
    if (!img.px) return;
    std::snprintf(label, sizeof(label), "%s: PNG size matches authoring resolution", art.ship_class);
    check(img.w == (int)art.art_w && img.h == (int)art.art_h, label);

    for (int i = 0; i < kDisplayCount; ++i) {
        const Quad& q = art.display[i];
        if (!present(q)) continue;
        const float off = worst_corner_offset(img, q);
        std::snprintf(label, sizeof(label), "%s %s: quad matches measured glass (%.2f px)",
                      art.ship_class, kNames[i], off);
        check(off <= 1.5f, label);
        const float glass = quad_fraction(img, q, 0.06f, 0.94f, true);
        std::snprintf(label, sizeof(label), "%s %s: interior is real alpha glass (%.3f)",
                      art.ship_class, kNames[i], glass);
        check(glass > 0.99f, label);
        const float bezel = bezel_ring(img, q, 4.0f);
        std::snprintf(label, sizeof(label), "%s %s: ringed by opaque bezel (%.3f)",
                      art.ship_class, kNames[i], bezel);
        check(bezel > 0.95f, label);
    }

    // Boresight: canopy glass around it, except where the art paints its own
    // gunsight — then the gunsight's opaque strokes must straddle it.
    bool clear = true;
    for (const auto& s : kShapes) {
        const Fit f = fit_to_viewport(art, 0, 0, s[0], s[1]);
        const float ax = (s[0] * 0.5f - f.ox) / f.scale, ay = (s[1] * 0.5f - f.oy) / f.scale;
        clear &= img.clear(ax, ay);
        if (!art.painted_gunsight)
            clear &= quad_fraction(img, quad_from_rect(ax - 30, ay - 30, 60, 60), 0, 1, true) > 0.999f;
    }
    std::snprintf(label, sizeof(label), "%s: boresight looks through canopy glass", art.ship_class);
    check(clear, label);
    if (art.painted_gunsight) {
        const float bx = art.boresight.x, by = art.boresight.y;
        int x0 = 0, x1 = 0;   // nearest opaque strokes left/right of centre
        for (int d = 1; d < 80 && !x0; ++d) if (!img.clear(bx - d, by)) x0 = d;
        for (int d = 1; d < 80 && !x1; ++d) if (!img.clear(bx + d, by)) x1 = d;
        std::snprintf(label, sizeof(label), "%s: boresight centred in the painted gunsight (%d/%d px)",
                      art.ship_class, x0, x1);
        check(x0 > 0 && x1 > 0 && std::abs(x0 - x1) <= 2, label);
    }
    stbi_image_free(img.px);
}

} // namespace

int main() {
    check(find_art("centurion") != nullptr, "centurion has cockpit art");
    check(find_art("talon") != nullptr,     "talon has cockpit art");
    check(find_art("tarsus") == nullptr,    "hull without art keeps classic HUD");
    check(find_art(nullptr) == nullptr,     "null class is safe");

    check_maths();

    // Talon (cover-and-slide) exact fits.
    const CockpitArt& talon = *find_art("talon");
    const Fit t720 = fit_to_viewport(talon, 0, 0, 1280, 720);
    check(near(t720.scale, 1.0f) && near(t720.ox, 0) && near(t720.oy, 100.0f),
          "talon 1280x720: 1:1, boresight slid onto screen centre");
    const Fit t1610 = fit_to_viewport(talon, 0, 0, 1440, 900);
    check(near(t1610.scale, 1.25f) && near(t1610.ox, -80.0f) && near(t1610.oy, 125.0f),
          "talon 16:10 covers by height, crops sides evenly");

    for (const CockpitArt& a : kCockpitArts) {
        check_fit(a);
        check_art_alpha(a);
    }

    std::printf("\n%s\n", g_failures ? "FAIL" : "ALL PASS");
    return g_failures ? 1 : 0;
}

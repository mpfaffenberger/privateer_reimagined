// Pure cockpit-art geometry harness (#426). Run from the repo root:
//   cmake --build build --target test_cockpit_overlay && ./build/test_cockpit_overlay
//
// Checks, for EVERY art row, against its real PNG alpha:
//   * each display quad matches the glass edges re-measured from the alpha
//     (lines fitted to the alpha=128 crossings, corners intersected) — so a
//     re-export of the art can't silently leave instruments off the bezels;
//   * the quad interior is transparent glass and a ring just outside it is
//     opaque bezel (the art on top masks warped-panel overhang);
//   * the screen centre (gun boresight) looks through clear canopy glass —
//     which also catches a baked-in crosshair (the HUD draws the live one).
// Plus the fit and homography maths.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "canopy_mask.h"
#include "cockpit_overlay_layout.h"
#include "pilot_head_motion.h"
#include "cockpit_lights.h"

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

const char* kNames[kDisplayCount] = { "left", "centre", "right", "banner", "set", "kps" };
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
    bool visible = true, bottom = true, sides = true;
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
        bottom &= f.oy + art.art_h * f.scale_y >= s[1] - 0.01f;
        sides &= f.ox <= 0.01f && f.ox + art.art_w * f.scale_x >= s[0] - 0.01f;
        sides &= near(f.ox * 2.0f + art.art_w * f.scale_x, s[0], 0.05f);
    }
    std::snprintf(label, sizeof(label), "%s: every display on screen, 4:3 to 21:9", art.ship_class);
    check(visible, label);
    std::snprintf(label, sizeof(label), "%s: dash always reaches the bottom edge", art.ship_class);
    check(bottom, label);
    std::snprintf(label, sizeof(label), "%s: art centred; covers both sides at every aspect", art.ship_class);
    check(sides, label);
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
        // 2 px out: the Centurion centre MFD's bottom bezel is only 3 px.
        const float bezel = bezel_ring(img, q, 2.0f);
        std::snprintf(label, sizeof(label), "%s %s: ringed by opaque bezel (%.3f)",
                      art.ship_class, kNames[i], bezel);
        check(bezel > 0.95f, label);
    }

    // Boresight: a 60 art-px box of clear canopy around it at every aspect —
    // no dash, no strut, no baked reticle.
    bool clear = true;
    for (const auto& s : kShapes) {
        const Fit f = fit_to_viewport(art, 0, 0, s[0], s[1]);
        const float ax = (s[0] * 0.5f - f.ox) / f.scale_x, ay = (s[1] * 0.5f - f.oy) / f.scale_y;
        clear &= quad_fraction(img, quad_from_rect(ax - 30, ay - 30, 60, 60), 0, 1, true) > 0.999f;
    }
    std::snprintf(label, sizeof(label), "%s: boresight looks through clear canopy", art.ship_class);
    check(clear, label);
    stbi_image_free(img.px);
}

// Off-screen pointers (#732). World glyphs draw behind the art (#429), so an
// arrow on the classic screen-edge box is buried under the frame; the canopy
// clamp must leave the whole arrow on real glass in every direction.
void check_pointers(const CockpitArt& art) {
    char label[128];
    AlphaImage img;
    int n = 0;
    img.px = stbi_load(art.path, &img.w, &img.h, &n, 4);
    if (!img.px) return;   // check_art_alpha already reported it
    const CanopyMask mask = make_canopy_mask(img.px, img.w, img.h);

    bool conservative = true;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x)
            if (img.at(x, y) >= 128) conservative &= mask.solid({ x + 0.5f, y + 0.5f });
    std::snprintf(label, sizeof(label), "%s: canopy mask covers every opaque art px", art.ship_class);
    check(conservative, label);

    constexpr float kMargin = 80.0f, kArrow = 14.0f, kHalf = kArrow * 0.6f;   // main.cpp's arrow
    int rays = 0, buried = 0, floored = 0;
    bool on_glass = true;
    for (const auto& s : kShapes) {
        const Fit f = fit_to_viewport(art, 0, 0, s[0], s[1]);
        const Homography to_art = screen_to_art(f, Homography{});
        const Vec2 half{ s[0] * 0.5f, s[1] * 0.5f };
        for (int deg = 0; deg < 360; deg += 5) {
            const float a = (float)deg * 3.14159265f / 180.0f;
            const Vec2 dir{ std::cos(a), std::sin(a) };
            const float t = pointer_reach(mask, to_art, half, half, dir, kMargin, kArrow, kHalf);
            const float edge = pointer_reach(CanopyMask{}, to_art, half, half, dir, kMargin, kArrow, kHalf);
            const Vec2 old_tip = apply(to_art, { half.x + dir.x * (edge + kArrow), half.y + dir.y * (edge + kArrow) });
            ++rays;
            buried += !img.clear(old_tip.x, old_tip.y);
            if (t <= kMinPointerReach + 0.01f && t < edge) { ++floored; continue; }
            // Centre line to the tip, and both flanks past the base corners.
            for (const float side : { -kHalf, 0.0f, kHalf })
                for (float r = 0.0f; r <= t + kArrow; r += 1.0f) {
                    const Vec2 p = apply(to_art, { half.x + dir.x * r - dir.y * side,
                                                   half.y + dir.y * r + dir.x * side });
                    on_glass &= img.clear(p.x, p.y) || p.x < 0 || p.y < 0 || p.x >= img.w || p.y >= img.h;
                }
        }
    }
    std::snprintf(label, sizeof(label), "%s: canopy-clamped arrows (tip + base) sit on glass", art.ship_class);
    check(on_glass, label);
    // The floor keeps a pointer off the gun crosshair where the dash rides
    // high; only the Galaxy needs it today, for rays aimed below the sight.
    std::snprintf(label, sizeof(label), "%s: most rays clear the crosshair floor (%d of %d floored)",
                  art.ship_class, floored, rays);
    check(floored * 2 < rays, label);
    std::printf("  (%s: classic screen-edge arrow was buried on %d of %d rays)\n",
                art.ship_class, buried, rays);
    stbi_image_free(img.px);
}

} // namespace

int main() {
    check(find_art("centurion") != nullptr, "centurion has cockpit art");
    check(find_art("talon") != nullptr,     "talon has cockpit art");
    check(find_art("galaxy") != nullptr,    "Galaxy has its own reference-based cockpit");
    check(find_art("tarsus") != nullptr,    "Tarsus has its own live cockpit");
    const CockpitArt& tarsus = *find_art("tarsus");
    check(tarsus.display[0].p[2].y < tarsus.display[1].p[0].y &&
          tarsus.display[0].p[2].x < tarsus.display[1].p[0].x &&
          tarsus.display[1].p[2].x < tarsus.display[2].p[0].x,
          "Tarsus silhouette: hanging upper-left CRT plus two separate lower monitors");
    check(!present(tarsus.display[3]) && !present(tarsus.display[4]) && !present(tarsus.display[5]),
          "Tarsus has three CRTs, not the fighter six-hole dashboard");
    check(find_art("unmapped-hull") == nullptr, "hull without art keeps classic HUD");
    check(find_art(nullptr) == nullptr,     "null class is safe");

    check_maths();
    const LightState healthy{true, false, true, false};
    const LightState alert{true, true, false, true};
    check(lamp_on(Lamp::Power, healthy, 0.1) && !lamp_on(Lamp::Power, healthy, 0.9) &&
          lamp_on(Lamp::Power, healthy, 1.7),
          "healthy power indicator visibly cycles with no damage or comms");
    check(lamp_on(Lamp::Auto, healthy, 0.1) && lamp_on(Lamp::Auto, healthy, 0.9),
          "AUTO-ready stays steady and independent of power heartbeat");
    check(!lamp_on(Lamp::Comms, healthy, 0) && !lamp_on(Lamp::Damage, healthy, 0) &&
          !lamp_on(Lamp::Auto, alert, 0), "inactive comms, damage and blocked AUTO stay dark");
    check(lamp_on(Lamp::Comms, alert, 0.1) && !lamp_on(Lamp::Comms, alert, 0.3),
          "comms activity blinks at 2Hz only when active");
    check(lamp_on(Lamp::Damage, alert, 0.1) && !lamp_on(Lamp::Damage, alert, 0.6),
          "actual low armor warning blinks at 1Hz");
    check(!lamp_on(Lamp::Power, {}, 0) && !lamp_on(Lamp::Activity, {}, 0.9),
          "unpowered lamps stay dark");
    check(!lamp_on(Lamp::Activity, healthy, 0.1) && lamp_on(Lamp::Activity, healthy, 0.9),
          "upper-rim amber activity alternates with cyan heartbeat in normal flight");
    const float capacity[4] = {10, 10, 10, 10}, safe[4] = {10, 10, 10, 10};
    const float hurt[4] = {10, 2.5f, 10, 10}, missing[4] = {0, 0, 0, 0};
    check(!low_armor(safe, capacity) && low_armor(hurt, capacity) && !low_armor(hurt, missing),
          "armor warning checks any facing and ignores missing capacity");
    AlphaImage lamp_art;
    int channels = 0;
    lamp_art.px = stbi_load(find_art("centurion")->path, &lamp_art.w, &lamp_art.h, &channels, 4);
    bool on_metal = lamp_art.px != nullptr;
    if (lamp_art.px) {
        for (const LampPlacement& lamp : kCenturionLamps) {
            const Rect r = inset(lamp.bounds, -6);
            for (int y = (int)r.y; y <= (int)(r.y+r.h); ++y)
                for (int x = (int)r.x; x <= (int)(r.x+r.w); ++x)
                    on_metal &= lamp_art.at(x,y) == 255;
        }
        stbi_image_free(lamp_art.px);
    }
    check(on_metal, "Centurion lamp hardware and glow sit entirely on opaque metal");
    bool rim_visible = true;
    int rim_count = 0;
    const CockpitArt& lamp_cockpit = *find_art("centurion");
    for (const LampPlacement& lamp : kCenturionLamps) {
        if (lamp.bounds.y >= 100) continue;
        ++rim_count;
        for (const auto& size : kShapes) {
            const Fit fit = fit_to_viewport(lamp_cockpit, 0, 0, size[0], size[1]);
            for (float side : {-1.0f, 1.0f}) {
                PilotHeadMotion motion;
                motion.lateral.rate = motion.vertical.rate = side;
                const auto transform = motion.transform({0, 0, size[0], size[1]});
                const Rect r = inset(lamp.bounds, -6);
                for (const Vec2& corner : to_screen(fit, quad_from_rect(r.x, r.y, r.w, r.h)).p) {
                    const Vec2 p = apply(transform, corner);
                    rim_visible &= p.x >= 0 && p.x <= size[0] && p.y >= 0 && p.y < size[1] * 0.5f;
                }
            }
        }
    }
    check(rim_count == 2 && rim_visible,
          "two top-rim lamps remain in upper view throughout slide and supported aspects");

    AlphaImage auxiliary_art;
    auxiliary_art.px = stbi_load(find_art("tarsus")->path, &auxiliary_art.w, &auxiliary_art.h, &channels, 4);
    bool auxiliary_opaque = auxiliary_art.px != nullptr;
    if (auxiliary_art.px) {
        for (const LampPlacement& lamp : kTarsusLamps) {
            const Rect r = inset(lamp.bounds, -1);
            for (int y = (int)r.y; y <= (int)(r.y+r.h); ++y)
                for (int x = (int)r.x; x <= (int)(r.x+r.w); ++x)
                    auxiliary_opaque &= auxiliary_art.at(x,y) == 255;
        }
        const Rect r = kTarsusActivityBackplate;
        for (int y = (int)r.y; y <= (int)(r.y+r.h); ++y)
            for (int x = (int)r.x; x <= (int)(r.x+r.w); ++x)
                auxiliary_opaque &= auxiliary_art.at(x,y) == 255;
        stbi_image_free(auxiliary_art.px);
    }
    check(auxiliary_opaque, "Tarsus auxiliary lamps replace opaque panel graphics, not window glass");
    bool ceiling_covered = true;
    for (const auto& size : kShapes) {
        const Fit f = fit_to_viewport(tarsus, 25, 35, size[0], size[1]);
        ceiling_covered &= near(f.oy, 35) && near(f.oy + tarsus.art_h*f.scale_y, 35+size[1]);
        for (float sign : {-1.0f, 1.0f}) {
            PilotHeadMotion motion;
            motion.lateral.rate = motion.vertical.rate = sign;
            const auto h = motion.transform({25,35,size[0],size[1]});
            const Vec2 top = apply(h, to_screen(f, Vec2{0,0}));
            const Vec2 bottom = apply(h, to_screen(f, Vec2{tarsus.art_w,tarsus.art_h}));
            ceiling_covered &= top.y <= 35 && bottom.y >= 35+size[1];
        }
    }
    check(ceiling_covered, "Tarsus ceiling and floor cover viewport edges even during full slide");

    // Head lag responds to acceleration, not steady angular velocity.
    PilotHeadMotion head;
    const Rect view{75, 40, 1920, 1080};
    const Vec2 sample{400, 200};
    const Homography resting = head.transform(view);
    const Vec2 center{view.x + view.w * 0.5f, view.y + view.h * 0.5f};
    check(near(apply(resting, center).x, center.x) && near(apply(resting, center).y, center.y),
          "resting slide centers fixed overscan without shifting cockpit");
    for (int i = 0; i < 60; ++i) head.update(3, -3, 1.0f / 60);
    check(head.lateral.rate < -0.99f && head.vertical.rate > 0.99f,
          "cockpit slides against angular acceleration");
    const Vec2 disabled = apply(head.transform(view, 0), sample);
    check(near(disabled.x, sample.x) && near(disabled.y, sample.y), "zero strength disables head effect");
    const float before = head.lateral.rate;
    head.update(-3, 3, 1.0f / 60);
    check(head.lateral.rate < 0 && std::abs(head.lateral.rate - before) < 0.1f,
          "head reversal is damped, not an instantaneous flip");
    bool covered = true, bounded = true, aligned_head = true, rigid = true;
    for (float lateral : {-1.0f, 0.0f, 1.0f}) {
        for (float vertical : {-1.0f, 0.0f, 1.0f}) {
            head.lateral.rate = lateral;
            head.vertical.rate = vertical;
            const Homography h = head.transform(view);
            rigid &= near(h.m[0], resting.m[0]) && near(h.m[4], resting.m[4]) &&
                     h.m[1] == 0 && h.m[3] == 0 && h.m[6] == 0 && h.m[7] == 0;
            for (float y : {view.y, view.y + view.h}) {
                // Intersect translated sides at actual viewport rows.
                const float source_y = (y - h.m[5]) / h.m[4];
                covered &= apply(h, {view.x, source_y}).x <= view.x + 0.01f;
                covered &= apply(h, {view.x + view.w, source_y}).x >= view.x + view.w - 0.01f;
            }
            const Vec2 p = apply(h, sample), neutral = apply(resting, sample);
            bounded &= near(p.x - neutral.x, lateral * 20.0f, 0.01f) &&
                       std::abs(p.y - neutral.y) <= 4.33f;
            covered &= apply(h, {view.x, view.y}).y <= view.y + 0.01f &&
                       apply(h, {view.x, view.y + view.h}).y >= view.y + view.h - 0.01f;
            const auto* art = find_art("centurion");
            const Fit fit = fit_to_viewport(*art, view.x, view.y, view.w, view.h);
            for (const Quad& q : art->display) {
                const Quad screen = to_screen(fit, q);
                const Rect panel = panel_rect(screen);
                const Homography combined = multiply(h, rect_to_quad(panel, screen));
                const Vec2 actual = apply(combined, {panel.x, panel.y});
                const Vec2 bezel = apply(h, screen.p[0]);
                const Vec2 back = apply(inverse(combined), actual);
                aligned_head &= near(actual.x, bezel.x, 0.01f) && near(actual.y, bezel.y, 0.01f)
                    && near(back.x, panel.x, 0.01f) && near(back.y, panel.y, 0.01f);
            }
        }
    }
    check(rigid, "motion changes translation only: no shear, rotation or dynamic zoom");
    check(covered, "maximum rigid slide cannot reopen side or bottom gaps");
    check(bounded, "maximum slide is exactly 20px left/right with subtle vertical travel");
    check(aligned_head, "rigid slide keeps glass, instruments and mouse coordinates aligned");
    bool slide_fit = true;
    for (float width : {1024.0f, 1280.0f, 1920.0f, 2560.0f}) {
        const Rect viewport{20, 30, width, 720};
        for (const CockpitArt& art : kCockpitArts) {
            const Fit f = fit_to_viewport(art, viewport.x, viewport.y, width, 720);
            for (float sign : {-1.0f, 1.0f}) {
                head.lateral.rate = head.vertical.rate = sign;
                const Homography transform = head.transform(viewport);
                const Rect art_rect = to_screen(f, Rect{0, 0, art.art_w, art.art_h});
                const Vec2 lo = apply(transform, {art_rect.x, art_rect.y});
                const Vec2 hi = apply(transform, {art_rect.x + art_rect.w, art_rect.y + art_rect.h});
                slide_fit &= lo.x <= 20.01f && hi.x >= 20 + width - 0.01f && hi.y >= 749.99f;
                for (const Quad& glass : art.display) {
                    if (!present(glass)) continue;
                    for (const Vec2& corner : to_screen(f, glass).p) {
                        const Vec2 p = apply(transform, corner);
                        slide_fit &= p.x >= 20 && p.x <= 20 + width && p.y >= 30 && p.y <= 750;
                    }
                }
            }
        }
    }
    check(slide_fit, "all hulls keep glass visible and frame covering edges during slide, 4:3 to 32:9");
    for (int i = 0; i < 180; ++i) head.update(0, 0, 1.0f / 60);
    check(std::abs(head.lateral.rate) < 0.00001f && std::abs(head.vertical.rate) < 0.00001f,
          "head returns to neutral in a steady turn");
    head.reset();
    check(head.lateral.rate == 0 && head.vertical.drive == 0, "cockpit re-entry resets head lag");

    // Talon (cover-and-slide) exact fits.
    const CockpitArt& talon = *find_art("talon");
    const Fit t720 = fit_to_viewport(talon, 0, 0, 1280, 720);
    check(near(t720.scale_y, 1.0f) && near(t720.ox, 0) && near(t720.oy, 100.0f),
          "talon 1280x720: 1:1, boresight slid onto screen centre");
    const Fit t1610 = fit_to_viewport(talon, 0, 0, 1440, 900);
    check(near(t1610.scale_y, 1.25f) && near(t1610.ox, -80.0f) && near(t1610.oy, 125.0f),
          "talon 16:10 covers by height, crops sides evenly");

    // Reproduce exposed side margins: vertical fit caps the uniform scale,
    // but horizontal coverage must still reach both viewport edges (#436).
    const CockpitArt& centurion = *find_art("centurion");
    const Fit wide = fit_to_viewport(centurion, 75, 40, 3840, 1080);
    const Rect bounds = to_screen(wide, Rect{0, 0, centurion.art_w, centurion.art_h});
    check(wide.scale_x > wide.scale_y && near(bounds.x, 75, 0.01f) &&
          near(bounds.x + bounds.w, 3915, 0.01f),
          "32:9 offset viewport: stretch removes hard side edges");
    bool aligned = true;
    for (const Quad& art_quad : centurion.display) {
        const Quad screen_quad = to_screen(wide, art_quad);
        const Rect panel = panel_rect(screen_quad);
        const Homography warp = rect_to_quad(panel, screen_quad);
        const Homography back = inverse(warp);
        const Vec2 flat{panel.x + panel.w * 0.3f, panel.y + panel.h * 0.6f};
        const Vec2 restored = apply(back, apply(warp, flat));
        aligned &= near(restored.x, flat.x, 0.01f) && near(restored.y, flat.y, 0.01f);
        for (const Vec2& point : screen_quad.p)
            aligned &= point.x >= 75 && point.x <= 3915 && point.y >= 40 && point.y <= 1120;
    }
    check(aligned, "stretched instruments stay visible and inverse mouse mapping round-trips");

    for (const CockpitArt& a : kCockpitArts) {
        check_fit(a);
        check_art_alpha(a);
        check_pointers(a);
    }
    {
        // Head sway: screen_to_art undoes fit + sway exactly (#732).
        const CockpitArt& a = *find_art("centurion");
        const Fit f = fit_to_viewport(a, 0, 0, 1920, 1080);
        Homography head;
        head.m[0] = 1.01f; head.m[1] = 0.02f; head.m[2] = 12.0f; head.m[5] = -7.0f;
        const Vec2 art_pt{ 700.0f, 300.0f };
        const Vec2 back = apply(screen_to_art(f, head), apply(head, to_screen(f, art_pt)));
        check(near(back.x, art_pt.x, 0.05f) && near(back.y, art_pt.y, 0.05f),
              "screen_to_art inverts fit + head sway");
        check(pointer_reach(CanopyMask{}, head, { 960, 540 }, { 960, 540 }, { 1, 0 }, 80, 14, 8) == 880.0f,
              "no art: pointer rides the classic screen-edge box");
    }

    std::printf("\n%s\n", g_failures ? "FAIL" : "ALL PASS");
    return g_failures ? 1 : 0;
}

// Pure cockpit-art geometry harness (#426). Run from the repo root:
//   cmake --build build --target test_cockpit_overlay && ./build/test_cockpit_overlay
//
// Checks the viewport fit, that every MFD hole stays on screen at common
// aspects, and — against the real PNG — that each table rect really is
// transparent glass ringed by opaque bezel (so a re-export of the art can't
// silently leave the instruments floating over the dashboard) and that the
// screen centre (gun boresight) always looks through canopy glass.
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

    std::printf("\n%s\n", g_failures ? "FAIL" : "ALL PASS");
    return g_failures ? 1 : 0;
}

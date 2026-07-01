// -----------------------------------------------------------------------------
// sprite_light_rec.cpp — dependency-free, REGION-based light recommender.
// See sprite_light_rec.h for the why. The pipeline:
//
//   1. Decode hull PNG (stb_image) -> RGBA + a valid (alpha) mask.
//   2. Box-blur the colour by `patch_px` (averages away tiny specks so the
//      broad feature colour dominates; integral-image, ignores transparent).
//   3. Segment: union-find merging adjacent valid pixels whose blurred colour
//      differs by < `color_tol`. Smoothly-shaded materials become one region;
//      sharp edges split them. -> per-region area, centroid, mean colour.
//   4. Query region = the segment under the clicked light.
//   5. Score every other region by colour-similarity x size-similarity.
//   6. NMS on centroid distance -> top-K region CENTROIDS (best-first).
// -----------------------------------------------------------------------------

#include "sprite_light_rec.h"

// stb_image is header-only; its implementation is compiled once in
// skybox.cpp. Here we only need the declarations to call stbi_load().
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace sprite_light_rec {
namespace {

struct RegionStat {
    int    area = 0;
    double sx = 0, sy = 0;       // centroid accumulators (pixel coords)
    double sr = 0, sg = 0, sb = 0; // mean-colour accumulators (0..255)
    float  cx() const { return (float)(sx / std::max(1, area)); }
    float  cy() const { return (float)(sy / std::max(1, area)); }
    float  r()  const { return (float)(sr / std::max(1, area)); }
    float  g()  const { return (float)(sg / std::max(1, area)); }
    float  b()  const { return (float)(sb / std::max(1, area)); }
};

struct Analysis {
    int w = 0, h = 0;
    std::vector<int>        label;    // region index per pixel, -1 if invalid
    std::vector<RegionStat> regions;
};

std::unordered_map<std::string, Analysis> g_cache;

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- union-find -----------------------------------------------------------
int uf_find(std::vector<int>& p, int x) {
    while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
    return x;
}
void uf_union(std::vector<int>& p, std::vector<int>& rank, int a, int b) {
    a = uf_find(p, a); b = uf_find(p, b);
    if (a == b) return;
    if (rank[a] < rank[b]) std::swap(a, b);
    p[b] = a;
    if (rank[a] == rank[b]) ++rank[a];
}

// ---- build the segmentation -----------------------------------------------
bool build(const std::string& path, int color_tol, int patch_px, Analysis& a) {
    int w = 0, h = 0, comp = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px || w < 4 || h < 4) { if (px) stbi_image_free(px); return false; }
    a.w = w; a.h = h;
    const size_t n = (size_t)w * h;

    // Valid mask + raw channels as doubles for the integral image.
    std::vector<uint8_t> valid(n, 0);
    std::vector<double> ir(n + w, 0), ig(n + w, 0), ib(n + w, 0), ic(n + w, 0);
    auto idx = [&](int x, int y) { return (size_t)y * w + x; };
    for (size_t i = 0; i < n; ++i) valid[i] = px[i * 4 + 3] > 32 ? 1 : 0;

    // Row-wise then column-wise prefix sums (integral images) of colour and
    // of the valid count, so a box average is O(1) per pixel.
    std::vector<double> R(n), G(n), B(n), CN(n);
    for (size_t i = 0; i < n; ++i) {
        const double m = valid[i] ? 1.0 : 0.0;
        R[i]  = m * px[i * 4 + 0];
        G[i]  = m * px[i * 4 + 1];
        B[i]  = m * px[i * 4 + 2];
        CN[i] = m;
    }
    auto integral = [&](std::vector<double>& v) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double s = v[idx(x, y)];
                if (x > 0) s += v[idx(x - 1, y)];
                if (y > 0) s += v[idx(x, y - 1)];
                if (x > 0 && y > 0) s -= v[idx(x - 1, y - 1)];
                v[idx(x, y)] = s;
            }
    };
    integral(R); integral(G); integral(B); integral(CN);

    const int rad = clampi(patch_px / 2, 0, 64);
    auto box = [&](const std::vector<double>& v, int x, int y) {
        const int x0 = clampi(x - rad - 1, -1, w - 1);
        const int y0 = clampi(y - rad - 1, -1, h - 1);
        const int x1 = clampi(x + rad, 0, w - 1);
        const int y1 = clampi(y + rad, 0, h - 1);
        double s = v[idx(x1, y1)];
        if (x0 >= 0) s -= v[idx(x0, y1)];
        if (y0 >= 0) s -= v[idx(x1, y0)];
        if (x0 >= 0 && y0 >= 0) s += v[idx(x0, y0)];
        return s;
    };
    // Blurred colour per valid pixel (average over valid neighbours only).
    std::vector<float> br(n, 0), bg(n, 0), bb(n, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = idx(x, y);
            if (!valid[i]) continue;
            const double cnt = std::max(1.0, box(CN, x, y));
            br[i] = (float)(box(R, x, y) / cnt);
            bg[i] = (float)(box(G, x, y) / cnt);
            bb[i] = (float)(box(B, x, y) / cnt);
        }

    // Quantize the blurred colour into bins of `color_tol`. Segmentation
    // then merges adjacent valid pixels in the SAME bin. Hard bins stop the
    // smooth-gradient leak that flooded the whole hull when we keyed on
    // adjacent-pixel difference: a shading ramp crosses bin edges and
    // splits, while a uniformly-shaded feature (a dark intake, a pod) stays
    // one bin. The pre-blur keeps a feature's interior inside a single bin.
    const int step = std::max(2, color_tol);
    std::vector<uint8_t> qr(n), qg(n), qb(n);
    for (size_t i = 0; i < n; ++i) {
        if (!valid[i]) continue;
        qr[i] = (uint8_t)((int)br[i] / step);
        qg[i] = (uint8_t)((int)bg[i] / step);
        qb[i] = (uint8_t)((int)bb[i] / step);
    }
    std::vector<int> parent(n), rank(n, 0);
    std::iota(parent.begin(), parent.end(), 0);
    auto same_bin = [&](size_t i, size_t j) {
        return qr[i] == qr[j] && qg[i] == qg[j] && qb[i] == qb[j];
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = idx(x, y);
            if (!valid[i]) continue;
            if (x + 1 < w) { const size_t j = idx(x + 1, y);
                if (valid[j] && same_bin(i, j)) uf_union(parent, rank, (int)i, (int)j); }
            if (y + 1 < h) { const size_t j = idx(x, y + 1);
                if (valid[j] && same_bin(i, j)) uf_union(parent, rank, (int)i, (int)j); }
        }

    // Compact roots -> region indices; accumulate stats on TRUE colour.
    a.label.assign(n, -1);
    std::unordered_map<int, int> root_to_idx;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = idx(x, y);
            if (!valid[i]) continue;
            const int root = uf_find(parent, (int)i);
            auto it = root_to_idx.find(root);
            int ri;
            if (it == root_to_idx.end()) {
                ri = (int)a.regions.size();
                root_to_idx.emplace(root, ri);
                a.regions.emplace_back();
            } else ri = it->second;
            RegionStat& rs = a.regions[ri];
            rs.area += 1;
            rs.sx += x; rs.sy += y;
            rs.sr += px[i * 4 + 0]; rs.sg += px[i * 4 + 1]; rs.sb += px[i * 4 + 2];
            a.label[i] = ri;
        }

    stbi_image_free(px);
    return true;
}

Analysis* get_analysis(const std::string& path, int color_tol, int patch_px) {
    const std::string key =
        path + "|" + std::to_string(color_tol) + "|" + std::to_string(patch_px);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return &it->second;
    Analysis a;
    if (!build(path, color_tol, patch_px, a)) return nullptr;
    return &g_cache.emplace(key, std::move(a)).first->second;
}

} // namespace

std::vector<Candidate> find_candidates(const std::string& hull_png_path,
                                       float query_u, float query_v,
                                       int k, int color_tol, int patch_px) {
    std::vector<Candidate> out;
    if (k <= 0) return out;
    color_tol = clampi(color_tol, 2, 200);
    patch_px  = clampi(patch_px, 1, 256);

    Analysis* a = get_analysis(hull_png_path, color_tol, patch_px);
    if (!a || a->regions.empty()) return out;

    const int qx = clampi((int)(query_u * a->w), 0, a->w - 1);
    const int qy = clampi((int)(query_v * a->h), 0, a->h - 1);
    const int qlabel = a->label[(size_t)qy * a->w + qx];
    if (qlabel < 0) return out;                  // query on transparent pixel
    const RegionStat& q = a->regions[qlabel];
    if (q.area <= 0) return out;

    const bool dbg = std::getenv("SLR_DEBUG") != nullptr;
    if (dbg) {
        std::fprintf(stderr,
            "[slr] %dx%d regions=%zu  query label=%d area=%d centroid=(%.0f,%.0f) "
            "rgb=(%.0f,%.0f,%.0f)\n", a->w, a->h, a->regions.size(), qlabel,
            q.area, q.cx(), q.cy(), q.r(), q.g(), q.b());
        std::vector<int> order(a->regions.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int i, int j){
            return a->regions[i].area > a->regions[j].area; });
        std::fprintf(stderr, "[slr] top regions (area  uv  rgb):\n");
        for (int i = 0; i < (int)order.size() && i < 14; ++i) {
            const RegionStat& r = a->regions[order[i]];
            std::fprintf(stderr, "   area=%6d uv=(%.3f,%.3f) rgb=(%.0f,%.0f,%.0f)\n",
                r.area, (r.cx()+0.5f)/a->w, (r.cy()+0.5f)/a->h, r.r(), r.g(), r.b());
        }
    }

    // Colour-match strictness scales with the user's tolerance so the two
    // knobs feel coherent: a loose tol also accepts looser colour matches.
    const float sc = std::max(12.0f, (float)color_tol);
    const float min_area = std::max(6.0f, q.area * 0.10f);

    struct Scored { float u, v, score, cx, cy; };
    std::vector<Scored> scored;
    scored.reserve(a->regions.size());
    for (int i = 0; i < (int)a->regions.size(); ++i) {
        if (i == qlabel) continue;
        const RegionStat& r = a->regions[i];
        if (r.area < min_area) continue;
        const float dr = r.r() - q.r(), dg = r.g() - q.g(), db = r.b() - q.b();
        const float cd2 = dr * dr + dg * dg + db * db;
        const float color_sim = std::exp(-cd2 / (2.0f * sc * sc));
        const float lo = (float)std::min(r.area, q.area);
        const float hi = (float)std::max(r.area, q.area);
        const float size_sim = lo / hi;
        const float score = color_sim * size_sim;
        if (score < 0.05f) continue;
        scored.push_back({ (r.cx() + 0.5f) / a->w, (r.cy() + 0.5f) / a->h,
                           score, r.cx(), r.cy() });
    }
    std::sort(scored.begin(), scored.end(),
              [](const Scored& p, const Scored& q2) { return p.score > q2.score; });

    // NMS in pixel space so two segments can't pile a marker on the same
    // spot; also keep clear of the query's own centroid.
    const float nms = std::max(6.0f, 0.5f * std::sqrt((float)q.area));
    const float qcx = q.cx(), qcy = q.cy();
    std::vector<Scored> kept;
    for (const Scored& s : scored) {
        const float dqx = s.cx - qcx, dqy = s.cy - qcy;
        if (dqx * dqx + dqy * dqy < nms * nms) continue;
        bool ok = true;
        for (const Scored& a2 : kept) {
            const float dx = s.cx - a2.cx, dy = s.cy - a2.cy;
            if (dx * dx + dy * dy < nms * nms) { ok = false; break; }
        }
        if (ok) kept.push_back(s);
        if ((int)kept.size() >= k) break;
    }
    for (const Scored& s : kept) out.push_back({ s.u, s.v, s.score });
    return out;
}

void clear_cache_for(const std::string& hull_png_path) {
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (it->first.rfind(hull_png_path + "|", 0) == 0) it = g_cache.erase(it);
        else ++it;
    }
}
void clear_cache() { g_cache.clear(); }

} // namespace sprite_light_rec

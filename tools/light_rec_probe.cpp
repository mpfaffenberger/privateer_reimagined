// Offline CLI probe for sprite_light_rec: runs the region matcher on one
// pixel of a hull PNG (no GPU/window). Backend for tools/annotate_light_rec.py.
// Not a test harness: it needs arguments and asserts nothing (#493). Build:
//   g++ -std=c++17 -I src -I third_party tools/light_rec_probe.cpp -o /tmp/tlr
// Run: /tmp/tlr <hull.png> <u> <v> [color_tol] [patch_px]
// stb_image.h has no include guard; let sprite_light_rec.cpp's single
// include pull it in, with the implementation macro set here.
#define STB_IMAGE_IMPLEMENTATION
#include "../src/sprite_light_rec.cpp"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s png u v [patch]\n", argv[0]); return 2; }
    const char* path = argv[1];
    const float u = (float)atof(argv[2]);
    const float v = (float)atof(argv[3]);
    const int tol   = argc > 4 ? atoi(argv[4]) : 30;
    const int patch = argc > 5 ? atoi(argv[5]) : 12;
    auto c = sprite_light_rec::find_candidates(path, u, v, 8, tol, patch);
    std::printf("query (%.3f,%.3f) color_tol=%d patch=%d -> %zu candidates\n",
                u, v, tol, patch, c.size());
    for (size_t i = 0; i < c.size(); ++i)
        std::printf("  #%zu (%.3f, %.3f) score=%.3f\n", i + 1, c[i].u, c[i].v, c[i].score);
    return 0;
}

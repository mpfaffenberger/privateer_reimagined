// test_mesh_indices.cpp — #484 regression: OBJ meshes beyond 65,535 unique
// corners must keep every triangle, in memory and through the .npmesh cache.
//
//   cmake --build build --target test_mesh_indices && ./build/test_mesh_indices

#include "mesh_cache.h"
#include "obj_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

// Link-only stubs: load_obj_file's texture/AABB steps need the GPU module.
// This test drives load_obj_text and the cache, which never call them.
bool load_texture_png(const std::string&, TextureSlot&) { return false; }
void Mesh::recompute_aabb() {}

namespace {

int failures = 0;

void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}

// Disjoint triangles: every corner is a unique OBJ vertex, like a reskin's
// atlas unwrap splitting shared positions at UV seams.
std::string disjoint_triangles_obj(int triangles) {
    std::ostringstream obj;
    for (int t = 0; t < triangles; ++t) {
        const float x = (float)t;
        obj << "v " << x << " 0 0\nv " << x << " 1 0\nv " << x + 0.5f << " 0 1\n";
    }
    for (int t = 0; t < triangles; ++t) {
        const int base = 3 * t + 1;
        obj << "f " << base << ' ' << base + 1 << ' ' << base + 2 << '\n';
    }
    return obj.str();
}

bool sequential_indices(const Mesh& mesh) {
    for (size_t i = 0; i < mesh.indices.size(); ++i)
        if (mesh.indices[i] != (MeshIndex)i) return false;
    return true;
}

} // namespace

int main() {
    constexpr int kTriangles = 23'000;   // 69,000 unique corners > 65,535
    constexpr size_t kCorners = 3 * kTriangles;
    const std::string text = disjoint_triangles_obj(kTriangles);

    Mesh loaded;
    check(load_obj_text(text, loaded), "large OBJ parses");
    check(loaded.vertices.size() == kCorners, "every unique corner becomes a vertex");
    check(loaded.indices.size() == kCorners, "every triangle is kept");
    check(sequential_indices(loaded), "indices past 65,535 are not collapsed to vertex 0");
    // Final corner is the last triangle's third vertex: x = (N-1) + 0.5.
    check(loaded.vertices.back().pos[0] == (float)kTriangles - 0.5f &&
          loaded.vertices.back().pos[2] == 1.0f,
          "last vertex keeps its own position");

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "np_test_mesh_indices";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path obj_path = dir / "wide.obj";
    std::ofstream(obj_path) << text;

    check(write_mesh_cache(obj_path.string(), loaded), "u32 cache writes");
    Mesh cached;
    check(try_load_mesh_cache(obj_path.string(), cached), "u32 cache reloads");
    check(cached.indices.size() == kCorners && sequential_indices(cached),
          "cache round-trip preserves 32-bit indices");

    // A pre-#484 NPMESH01 cache stored u16 indices; it must miss, not misread.
    {
        std::fstream f(dir / "wide.npmesh", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(7);
        f.put('1');
    }
    Mesh stale;
    check(!try_load_mesh_cache(obj_path.string(), stale), "old u16 cache version is rejected");

    fs::remove_all(dir);
    std::printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}

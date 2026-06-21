// -----------------------------------------------------------------------------
// skybox_gen.cpp — see skybox_gen.h. Owns the generated shader header in its
// own TU (sokol-shdc headers can't share a TU; see CMake note).
// -----------------------------------------------------------------------------
#include "skybox_gen.h"

#include "HandmadeMath.h"
#include "generated/skybox_gen.glsl.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {

// ---- deterministic RNG (seed string -> reproducible sky) --------------------
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    float next() {                          // xorshift64* -> [0,1)
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        uint64_t r = s * 0x2545F4914F6CDD1Dull;
        return (float)((r >> 40) & 0xFFFFFF) / (float)0x1000000;
    }
};
uint64_t hash_seed(const std::string& str) {
    uint64_t h = 1469598103934665603ull;    // FNV-1a
    for (unsigned char c : str) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// Uniformly-random unit vector (Marsaglia).
HMM_Vec3 rand_dir(Rng& r) {
    for (;;) {
        float x = r.next() * 2.0f - 1.0f, y = r.next() * 2.0f - 1.0f;
        float q = x * x + y * y;
        if (q >= 1.0f || q == 0.0f) continue;
        float k = 2.0f * std::sqrt(1.0f - q);
        return HMM_V3(x * k, y * k, 1.0f - 2.0f * q);
    }
}

// Inside-out unit cube (36 verts) — view direction == vertex position.
const float kBox[108] = {
    -1,-1,-1, 1,-1,-1, 1,1,-1,  -1,-1,-1, 1,1,-1, -1,1,-1,
     1,-1,1, -1,-1,1, -1,1,1,    1,-1,1, -1,1,1,  1,1,1,
     1,-1,-1, 1,-1,1, 1,1,1,     1,-1,-1, 1,1,1,  1,1,-1,
    -1,-1,1, -1,-1,-1, -1,1,-1, -1,-1,1, -1,1,-1, -1,1,1,
    -1,1,-1, 1,1,-1, 1,1,1,     -1,1,-1, 1,1,1,  -1,1,1,
    -1,-1,1, 1,-1,1, 1,-1,-1,   -1,-1,1, 1,-1,-1, -1,-1,-1,
};

// Per-cube-face camera, ordered to match sokol cube slices (and our
// kFaceSuffixes): 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z. Standard D3D/Metal-style
// cubemap face orientation so the rendered texels line up with sampling.
struct Face { HMM_Vec3 dir, up; };
const Face kFaces[6] = {
    {{ 1, 0, 0}, {0,-1, 0}},   // +X
    {{-1, 0, 0}, {0,-1, 0}},   // -X
    {{ 0, 1, 0}, {0, 0, 1}},   // +Y
    {{ 0,-1, 0}, {0, 0,-1}},   // -Y
    {{ 0, 0, 1}, {0,-1, 0}},   // +Z
    {{ 0, 0,-1}, {0,-1, 0}},   // -Z
};

// One bright billboard "point star": 2 tris, oriented toward `pos`, pushed out
// to `dist`. Brightness pow'd for a dim-biased distribution. Appends 6 verts.
void build_point_star(float size, HMM_Vec3 dir, float dist, Rng& r,
                      std::vector<float>& pos, std::vector<float>& col) {
    const float c = std::pow(r.next(), 4.0f);
    // Orient a +Z quad toward `dir`.
    const HMM_Vec3 fwd = HMM_V3(0, 0, 1);
    HMM_Quat q;
    const float d = HMM_DotV3(fwd, dir);
    if (d > 0.999999f)       q = HMM_Q(0, 0, 0, 1);
    else if (d < -0.999999f) q = HMM_Q(1, 0, 0, 0);
    else {
        HMM_Vec3 ax = HMM_NormV3(HMM_Cross(fwd, dir));
        q = HMM_QFromAxisAngle_RH(ax, std::acos(d));
    }
    const HMM_Vec3 quad[6] = {
        {-size,-size,0}, {size,-size,0}, {size,size,0},
        {-size,-size,0}, {size,size,0}, {-size,size,0},
    };
    for (const HMM_Vec3& v0 : quad) {
        HMM_Vec3 v = HMM_RotateV3Q(v0, q);
        v = HMM_AddV3(v, HMM_MulV3F(dir, dist));
        pos.push_back(v.X); pos.push_back(v.Y); pos.push_back(v.Z);
        col.push_back(c);   col.push_back(c);   col.push_back(c);
    }
}

struct Nebula { HMM_Vec3 color, offset; float scale, intensity, falloff; };
struct Star   { HMM_Vec3 dir; float size, falloff; };

// Per-cube-face basis (forward, s-axis, t-axis) in sokol slice order
// (0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z), exactly the OpenGL cube-map face
// coordinate convention. The fullscreen nebula shader reconstructs each
// texel's sample direction as fwd + (2u-1)*sax + (2v-1)*tax, which is the
// inverse of the hardware's dir->face-uv mapping -> seamless across faces.
struct FaceBasis { HMM_Vec3 fwd, sax, tax; };
const FaceBasis kFaceBasis[6] = {
    {{ 1, 0, 0}, { 0, 0,-1}, { 0,-1, 0}},   // +X
    {{-1, 0, 0}, { 0, 0, 1}, { 0,-1, 0}},   // -X
    {{ 0, 1, 0}, { 1, 0, 0}, { 0, 0, 1}},   // +Y
    {{ 0,-1, 0}, { 1, 0, 0}, { 0, 0,-1}},   // -Y
    {{ 0, 0, 1}, { 1, 0, 0}, { 0,-1, 0}},   // +Z
    {{ 0, 0,-1}, {-1, 0, 0}, { 0,-1, 0}},   // -Z
};

// Fullscreen quad: clip-space xy + face texel uv (top-left origin).
const float kFsQuad[24] = {
    -1.0f,  1.0f, 0.0f, 0.0f,   1.0f,  1.0f, 1.0f, 0.0f,   1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,   1.0f, -1.0f, 1.0f, 1.0f,  -1.0f, -1.0f, 0.0f, 1.0f,
};

// kind: 0 = box (FLOAT3 pos), 1 = pstar (FLOAT3 pos + FLOAT3 col),
//       2 = fullscreen nebula (FLOAT2 pos + FLOAT2 uv).
sg_pipeline make_pipe(sg_shader sh, int kind) {
    sg_pipeline_desc pd{};
    pd.shader = sh;
    if (kind == 2) {
        pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;   // a_pos (clip xy)
        pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;   // a_uv
    } else {
        pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT3;   // a_pos
        if (kind == 1) pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT3; // a_col
    }
    pd.cull_mode = SG_CULLMODE_NONE;
    pd.depth.write_enabled = false;
    pd.depth.pixel_format  = SG_PIXELFORMAT_NONE;   // color-only offscreen pass
    pd.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
    pd.colors[0].blend.enabled        = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.sample_count = 1;
    return sg_make_pipeline(&pd);
}

}  // namespace

namespace skybox_gen {

sg_image generate(const std::string& seed, int face_res, float sun_warmth,
                   HMM_Vec3 target_a, HMM_Vec3 target_b) {
    const uint64_t h = hash_seed(seed);
    // Clamp to a sane band so a wildly-tuned sun can't flatten the skybox
    // to a single hue. A warmth of 0.4 (mild warm) is enough to nudge
    // most random colours into a warm palette; we cap the visible bias
    // at ~0.85 so even an extreme sun keeps some variety.
    const float w     = std::fmax(-1.0f, std::fmin(1.0f, sun_warmth));
    const float w_abs = std::fabs(w);

    // ---- seeded parameter lists (counts/ranges from the skyboxgen "rich"
    //      preset that was the established sweet spot) ----------------------
    std::vector<HMM_Quat> pstar_layers;
    {
        Rng r(h + 1000);
        do {
            const HMM_Vec3 ax = rand_dir(r);
            pstar_layers.push_back(HMM_QFromAxisAngle_RH(ax, r.next() * 6.2831853f));
        } while (r.next() >= 0.2f && pstar_layers.size() < 6);
    }
    std::vector<Star> stars;
    {
        Rng r(h + 3000);
        do {
            stars.push_back({rand_dir(r), 0.0f,
                             r.next() * 1048576.0f + 1048576.0f});
        } while (r.next() >= 0.01f && stars.size() < 256);
    }
    std::vector<Nebula> nebulae;
    {
        Rng r(h + 2000);
        const int target = 2;                 // "rich" preset count
        // Pre-pick the per-nebula target row so we don't go fully mono.
        bool use_b = (h & 1) != 0;
        for (int i = 0; i < target; ++i) {
            Nebula n;
            n.scale     = (r.next() * 0.5f + 0.25f) * 1.1f;
            // Random base color, then bias toward the family palette by
            // w_abs. mix=0 keeps it pure random; mix=0.7 locks it in for
            // strong suns while preserving some variety. target_a vs
            // target_b is chosen per nebula (alternating via use_b) so
            // both anchors contribute across a system.
            HMM_Vec3 base = HMM_V3(r.next(), r.next(), r.next());
            HMM_Vec3 tgt  = use_b ? target_b : target_a;
            const float mix = w_abs * 0.70f;     // up to 70% blend
            n.color = HMM_V3(
                base.X * (1.0f - mix) + tgt.X * mix,
                base.Y * (1.0f - mix) + tgt.Y * mix,
                base.Z * (1.0f - mix) + tgt.Z * mix);
            // Dim the "unused" channel a touch for stronger suns so the
            // family dominates (extra cheap; no extra branch).
            if      (w >  0.05f && n.color.Z > 0.05f) n.color.Z *= 1.0f - 0.30f * w_abs;
            else if (w < -0.05f && n.color.X > 0.05f) n.color.X *= 1.0f - 0.30f * w_abs;
            n.intensity = (r.next() * 0.2f + 0.9f) * 1.15f;
            n.falloff   = (r.next() * 3.0f + 3.0f) * 0.95f;
            n.offset    = HMM_V3(r.next() * 2000.0f - 1000.0f,
                                 r.next() * 2000.0f - 1000.0f,
                                 r.next() * 2000.0f - 1000.0f);
            nebulae.push_back(n);
        }
    }

    // ---- point-star mesh (shared across faces+layers) --------------------
    std::vector<float> ps_pos, ps_col;
    constexpr int kNumPointStars = 60000;
    ps_pos.reserve(kNumPointStars * 18);
    ps_col.reserve(kNumPointStars * 18);
    {
        Rng r(h + 5000);
        for (int i = 0; i < kNumPointStars; ++i)
            build_point_star(0.05f, rand_dir(r), 128.0f, r, ps_pos, ps_col);
    }

    // ---- GPU resources ---------------------------------------------------
    sg_image_desc cd{};
    cd.type        = SG_IMAGETYPE_CUBE;
    cd.width       = face_res;
    cd.height      = face_res;
    cd.num_slices  = 6;
    cd.pixel_format = SG_PIXELFORMAT_RGBA8;
    cd.usage.color_attachment = true;
    sg_image cube = sg_make_image(&cd);
    if (sg_query_image_state(cube) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[skybox_gen] cube image creation failed\n");
        return cube;
    }

    sg_view face_att[6];
    for (int f = 0; f < 6; ++f) {
        sg_view_desc vd{};
        vd.color_attachment.image = cube;
        vd.color_attachment.slice = f;
        face_att[f] = sg_make_view(&vd);
    }

    sg_shader sh_neb   = sg_make_shader(neb_shader_desc(sg_query_backend()));
    sg_shader sh_star  = sg_make_shader(star_shader_desc(sg_query_backend()));
    sg_shader sh_pstar = sg_make_shader(pstar_shader_desc(sg_query_backend()));
    sg_pipeline pipe_neb   = make_pipe(sh_neb,   2);   // fullscreen quad
    sg_pipeline pipe_star  = make_pipe(sh_star,  0);   // box geometry
    sg_pipeline pipe_pstar = make_pipe(sh_pstar, 1);   // pos+col geometry

    sg_buffer_desc bvd{};
    bvd.data = SG_RANGE(kBox);
    sg_buffer box_vbuf = sg_make_buffer(&bvd);

    sg_buffer_desc qvd{};
    qvd.data = SG_RANGE(kFsQuad);
    sg_buffer fsquad_vbuf = sg_make_buffer(&qvd);

    // Interleave pstar pos+col into one buffer (pos xyz, col rgb per vert).
    std::vector<float> ps_interleaved;
    ps_interleaved.reserve(ps_pos.size() * 2);
    for (size_t i = 0; i < ps_pos.size(); i += 3) {
        ps_interleaved.insert(ps_interleaved.end(), {ps_pos[i], ps_pos[i+1], ps_pos[i+2],
                                                     ps_col[i], ps_col[i+1], ps_col[i+2]});
    }
    sg_buffer_desc pvd{};
    pvd.data = sg_range{ ps_interleaved.data(), ps_interleaved.size() * sizeof(float) };
    sg_buffer pstar_vbuf = sg_make_buffer(&pvd);
    const int pstar_verts = (int)(ps_pos.size() / 3);

    const HMM_Mat4 proj = HMM_Perspective_RH_NO(HMM_AngleDeg(90.0f), 1.0f, 0.05f, 512.0f);

    // ---- render each face ------------------------------------------------
    for (int f = 0; f < 6; ++f) {
        const HMM_Mat4 view = HMM_LookAt_RH(HMM_V3(0, 0, 0), kFaces[f].dir, kFaces[f].up);
        const HMM_Mat4 vp   = HMM_MulM4(proj, view);

        sg_pass p{};
        p.attachments.colors[0] = face_att[f];
        p.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        p.action.colors[0].clear_value = {0.0f, 0.0f, 0.0f, 1.0f};
        sg_begin_pass(&p);

        // -- point stars (per accumulated-rotation layer) --
        sg_apply_pipeline(pipe_pstar);
        { sg_bindings b{}; b.vertex_buffers[0] = pstar_vbuf; sg_apply_bindings(&b); }
        {
            HMM_Mat4 model = HMM_M4D(1.0f);
            for (const HMM_Quat& q : pstar_layers) {
                model = HMM_MulM4(HMM_QToM4(q), model);
                const HMM_Mat4 mvp = HMM_MulM4(vp, model);
                pstar_vs_params_t u{};
                std::memcpy(u.mvp, &mvp, 64);
                sg_apply_uniforms(UB_pstar_vs_params, SG_RANGE(u));
                sg_draw(0, pstar_verts, 1);
            }
        }

        // -- bright stars (box geometry, radial glow) --
        sg_apply_pipeline(pipe_star);
        { sg_bindings b{}; b.vertex_buffers[0] = box_vbuf; sg_apply_bindings(&b); }
        {
            box_vs_params_t vu{}; std::memcpy(vu.mvp, &vp, 64);
            sg_apply_uniforms(UB_box_vs_params, SG_RANGE(vu));
            for (const Star& s : stars) {
                star_params_t fu{};
                fu.u_pos_size[0] = s.dir.X; fu.u_pos_size[1] = s.dir.Y;
                fu.u_pos_size[2] = s.dir.Z; fu.u_pos_size[3] = s.size;
                fu.u_color_falloff[0] = 1.0f; fu.u_color_falloff[1] = 1.0f;
                fu.u_color_falloff[2] = 1.0f; fu.u_color_falloff[3] = s.falloff;
                sg_apply_uniforms(UB_star_params, SG_RANGE(fu));
                sg_draw(0, 36, 1);
            }
        }

        // -- nebulae (fullscreen per-direction pass, seamless) --
        sg_apply_pipeline(pipe_neb);
        { sg_bindings b{}; b.vertex_buffers[0] = fsquad_vbuf; sg_apply_bindings(&b); }
        {
            const FaceBasis& fb = kFaceBasis[f];
            for (const Nebula& n : nebulae) {
                nebula_params_t fu{};
                fu.u_fwd[0] = fb.fwd.X; fu.u_fwd[1] = fb.fwd.Y; fu.u_fwd[2] = fb.fwd.Z;
                fu.u_sax[0] = fb.sax.X; fu.u_sax[1] = fb.sax.Y; fu.u_sax[2] = fb.sax.Z;
                fu.u_tax[0] = fb.tax.X; fu.u_tax[1] = fb.tax.Y; fu.u_tax[2] = fb.tax.Z;
                fu.u_color_scale[0] = n.color.X; fu.u_color_scale[1] = n.color.Y;
                fu.u_color_scale[2] = n.color.Z; fu.u_color_scale[3] = n.scale;
                fu.u_offset_intensity[0] = n.offset.X; fu.u_offset_intensity[1] = n.offset.Y;
                fu.u_offset_intensity[2] = n.offset.Z; fu.u_offset_intensity[3] = n.intensity;
                fu.u_misc[0] = n.falloff;
                sg_apply_uniforms(UB_nebula_params, SG_RANGE(fu));
                sg_draw(0, 6, 1);
            }
        }

        sg_end_pass();
    }

    // ---- tear down transient resources (keep the cube image) -------------
    sg_destroy_buffer(box_vbuf);
    sg_destroy_buffer(fsquad_vbuf);
    sg_destroy_buffer(pstar_vbuf);
    sg_destroy_pipeline(pipe_neb);
    sg_destroy_pipeline(pipe_star);
    sg_destroy_pipeline(pipe_pstar);
    sg_destroy_shader(sh_neb);
    sg_destroy_shader(sh_star);
    sg_destroy_shader(sh_pstar);
    for (int f = 0; f < 6; ++f) sg_destroy_view(face_att[f]);

    std::printf("[skybox_gen] generated '%s' %dx%d cube: %zu nebulae, %zu stars, "
                "%d point-stars x %zu layers\n",
                seed.c_str(), face_res, face_res, nebulae.size(), stars.size(),
                kNumPointStars, pstar_layers.size());
    return cube;
}

}  // namespace skybox_gen

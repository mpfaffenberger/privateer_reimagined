// -----------------------------------------------------------------------------
// dust.glsl — near-field parallax specks.
//
// Dust lives in a cube of half-extent `wrap_extent` around a reference point
// (the camera). On the CPU we keep a static buffer of random positions in a
// centered box of size [-1, +1] and offset them by the camera's floor position
// in the vertex shader, wrapping each axis back into range. That makes the
// field feel infinite without re-uploading vertex data.
//
// No lighting, no texture — just soft dots. Purely a "motion is happening"
// cue. The point size shrinks with distance so faraway dust doesn't dominate.
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform vs_params {
    mat4 view_proj;
    vec4 cam_pos;       // .xyz = camera world pos, .w unused
    vec4 field_params;  // .x = wrap_extent, .y = point_size_px, .zw unused
};

in vec3 a_pos;   // random position in [-1, +1]^3

out float v_alpha;

void main() {
    float E = field_params.x;

    // Treat the VBO positions as a repeating WORLD-space cell, then wrap
    // that cell around the camera. Do not add the camera before deriving
    // `rel`: doing so cancels the subtraction algebraically and glues every
    // speck to the camera, eliminating all fly-by motion.
    vec3 world_seed = a_pos * E;
    vec3 rel = mod(world_seed - cam_pos.xyz + E, 2.0 * E) - E;
    vec3 p   = cam_pos.xyz + rel;

    // Fade dust near the cubic boundary to avoid a hard pop when it wraps.
    float edge = max(max(abs(rel.x), abs(rel.y)), abs(rel.z)) / E;
    v_alpha = clamp(1.0 - smoothstep(0.75, 1.0, edge), 0.0, 1.0);

    gl_Position  = view_proj * vec4(p, 1.0);
    // Point size: GL + Metal support a per-vertex point size (Metal via
    // [[point_size]], which SPIRV-Cross emits from gl_PointSize). HLSL has
    // NO point-size builtin — SPIRV-Cross throws 'Unsupported builtin in
    // HLSL' — so the D3D11 build defines NP_NO_POINT_SIZE (see CMakeLists)
    // to compile it out. We use our OWN define rather than sokol-shdc's
    // SOKOL_D3D11/SOKOL_METAL because this shdc build doesn't reliably set
    // those during the glslang preprocessor pass.
    // Leaving point size UNSET on Metal was the cause of the giant
    // flickering white squares: an unwritten point size is undefined.
#if !defined(NP_NO_POINT_SIZE)
    gl_PointSize = field_params.y;
#endif
}
@end

@fs fs
in  float v_alpha;
out vec4  frag_color;

void main() {
#if !defined(NP_NO_POINT_SIZE)
    // Soft round dot via distance to point-center in [-0.5, 0.5].
    // gl_PointCoord works on GL and Metal (SPIRV-Cross -> [[point_coord]]);
    // HLSL has no point-coord builtin so the D3D11 build (NP_NO_POINT_SIZE)
    // excludes it.
    vec2  d = gl_PointCoord - vec2(0.5);
    float r = length(d);
    if (r > 0.5) discard;
    float a = smoothstep(0.5, 0.1, r) * v_alpha;
#else
    // D3D11 POINT primitives are 1 pixel and have no gl_PointCoord;
    // just emit a flat alpha at v_alpha for every speck.
    float a = v_alpha;
#endif
    // Slightly warm white — reads as 'icy dust' against the nebula.
    frag_color = vec4(vec3(1.0, 1.0, 1.0), a);
}
@end

@program dust vs fs

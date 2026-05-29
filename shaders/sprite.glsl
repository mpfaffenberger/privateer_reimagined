// -----------------------------------------------------------------------------
// sprite.glsl — camera-facing billboard quad with alpha/additive texturing.
//
// One shader, TWO pipelines (CPU-side): the alpha-blend pipeline draws the
// sprite HULL, the additive pipeline draws the LIGHTS overlay on top. Same
// geometry, same UVs, same sampling — the only difference is the blend
// state. Keeps shader permutations at zero.
//
// Billboarding is done by expanding a unit quad along the CAMERA'S right/up
// vectors (extracted CPU-side from the inverse view matrix and passed as
// uniforms). This keeps the sprite perpetually facing the camera regardless
// of camera orientation — classic Privateer / Homeworld look, no banking,
// no pitch.
//
// Aspect ratio: the sprite's world size is defined by `world_size` (its
// *longest* side in world units). The short axis is scaled down via
// `inst_scale.xy`, which the CPU sets from the texture's aspect ratio so
// non-square sprites don't get stretched into squares.
// -----------------------------------------------------------------------------

@vs vs

layout(binding=0) uniform vs_params {
    mat4 view_proj;
    vec4 cam_right;     // world-space camera right vector, .w unused
    vec4 cam_up;        // world-space camera up vector,    .w unused
    vec4 inst_pos;      // .xyz = sprite world position, .w = world_size (half)
    vec4 inst_scale;    // .xy  = aspect multipliers (1.0 for square sprites)
};

in vec2 a_corner;   // unit quad corners in [-1,+1]^2
in vec2 a_uv;       // 0..1, matches corner ordering

out vec2 v_uv;

void main() {
    // Half-extents along camera right/up. `inst_pos.w` is the half-length
    // of the sprite's LONGEST edge; `inst_scale.xy` shrinks the shorter
    // edge for non-square sprites. `inst_scale.zw` carries cos/sin for an
    // optional in-plane roll, used by the atlas inspector to fix individual
    // frames that came back a little crooked. Default cos=1, sin=0.
    vec2 corner = a_corner;
    float c = inst_scale.z;
    float s = inst_scale.w;
    corner = vec2(corner.x * c - corner.y * s,
                  corner.x * s + corner.y * c);
    vec3 offs = cam_right.xyz * (corner.x * inst_pos.w * inst_scale.x)
              + cam_up.xyz    * (corner.y * inst_pos.w * inst_scale.y);
    gl_Position = view_proj * vec4(inst_pos.xyz + offs, 1.0);

    // Sokol/Metal texture origin fun: PNGs loaded through stb_image arrive
    // visually upside-down in our billboard path. Flip V once here so every
    // sprite authoring tool can keep the normal "top of PNG is top of ship"
    // mental model. Do NOT flip the camera unless you enjoy debugging the
    // universe through a haunted funhouse mirror.
    v_uv = vec2(a_uv.x, 1.0 - a_uv.y);
}
@end

@fs fs

layout(binding=0) uniform texture2D u_tex;
layout(binding=0) uniform sampler   u_smp;

layout(binding=1) uniform fs_params {
    vec4 tint;          // .rgb tints the sample, .a is global alpha multiplier
};

in  vec2 v_uv;
out vec4 frag;

// ── Pixel-art post knobs ──────────────────────────────────────────────
// Tuned for the ~512px 3D-rendered atlas cells. PIXEL_RES is the number
// of "virtual pixels" across the sprite's longest edge — lower = chunkier.
// COLOR_LEVELS is the per-channel palette depth (6 → 6³ = 216 colours,
// close to the 256-colour VGA palette the original Privateer ran in).
// ALPHA_CUTOFF is bumped well above zero so the chunked edge reads as a
// hard 1-bit mask (crisp blocky silhouette) instead of soft AA fringe.
const float PIXEL_RES    = 200.0;   // subtle pixel grid (96→160→200)
const float COLOR_LEVELS = 16.0;    // mild posterize (6→12→16)
const float ALPHA_CUTOFF = 0.35;

void main() {
    // Snap the UV to the centre of its virtual-pixel cell. Adjacent
    // screen fragments inside the same cell now sample the same point,
    // which is what produces the blocky "big pixel" look regardless of
    // how close the camera gets to the billboard.
    vec2 px_uv = (floor(v_uv * PIXEL_RES) + 0.5) / PIXEL_RES;

    vec4 c = texture(sampler2D(u_tex, u_smp), px_uv);
    // Hard alpha mask — see ALPHA_CUTOFF note. Also keeps the sprite's
    // bounding quad from writing depth on (now-chunked) empty texels so
    // additive atmospherics don't get punched out around the silhouette.
    if (c.a < ALPHA_CUTOFF) discard;

    // Palette quant(posterize) each channel to COLOR_LEVELS steps. This
    // is what sells the retro look — smooth hull gradients collapse into
    // a handful of flat shades like a hand-indexed 256-colour sprite.
    vec3 q = floor(c.rgb * COLOR_LEVELS + 0.5) / COLOR_LEVELS;

    // Multiply RGB by tint.rgb (damage flash, distance-dim, etc). Alpha
    // is forced to 1.0 post-cutoff so the kept pixels are fully opaque
    // (no soft edge), then scaled by tint.a for global fade control.
    frag = vec4(q * tint.rgb, tint.a);
}
@end

@program sprite vs fs

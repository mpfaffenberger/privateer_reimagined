// -----------------------------------------------------------------------------
// post_bloom.glsl — progressive bloom chain (#724).
//
// Two programs sharing one fullscreen-triangle vertex shader:
//
//   bloom_down — 13-tap downsample (Jimenez, "Next Generation Post
//                Processing in Call of Duty: Advanced Warfare"). Four
//                overlapping 2x2 box filters weighted toward the centre,
//                so a 1-pixel bolt lands in the next mip without the
//                shimmer the old 2.5 px-strided 9-tap blur had. The first
//                level (scene -> half res) also runs the bright-pass.
//
//   bloom_up   — 9-tap tent upsample, blended ADDITIVELY onto the next
//                larger mip (the pipeline's blend state does the add).
//                Walking the chain back up sums every mip, so the low mips
//                give the wide soft glow and the high mips the tight core.
//
// ORIENTATION (#736): both passes take their UV from gl_FragCoord / target
// size, NOT from the fullscreen triangle's v_uv. On D3D11 / Metal a v_uv
// pass flips the image vertically (clip space is y-up, texture rows start
// at the top), and with 11 chained passes the mips ended up with mixed
// parity: every light bloomed twice, once mirrored. Pixel and texel
// origins always agree, so FragCoord-based UV is an identity on every
// backend.
// -----------------------------------------------------------------------------

@vs vs
// Fullscreen triangle from gl_VertexIndex: no vertex buffer, 3 vertices.
out vec2 v_uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs fs_down
layout(binding=0) uniform texture2D u_src;
layout(binding=0) uniform sampler   u_smp;

layout(binding=0) uniform bloom_down_params {
    vec4 texel_and_cfg;   // .xy = 1/src size, .z = threshold (0 = no bright-pass), .w = peak clamp (0 = none)
    vec4 dst_texel;       // .xy = 1/dst (render target) size
};

in  vec2 v_uv;
out vec4 frag_color;

vec3 tap(vec2 uv) { return texture(sampler2D(u_src, u_smp), uv).rgb; }

// Soft-knee bright-pass: energy ABOVE the threshold, eased in by a
// quadratic knee so pixels hovering at the threshold don't flicker in and
// out. The peak clamp keeps the HDR sun core from ballooning the glow.
vec3 bright_pass(vec3 c, float threshold, float clamp_peak) {
    float peak = max(max(c.r, c.g), c.b);
    if (clamp_peak > 0.0 && peak > clamp_peak) {
        c   *= clamp_peak / peak;
        peak = clamp_peak;
    }
    float knee = threshold * 0.5;
    float soft = clamp(peak - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-5);
    float contrib = max(soft, peak - threshold) / max(peak, 1e-5);
    return c * contrib;
}

void main() {
    vec2 t = texel_and_cfg.xy;
    vec2 uv = gl_FragCoord.xy * dst_texel.xy;

    vec3 a = tap(uv + t * vec2(-2.0,  2.0));
    vec3 b = tap(uv + t * vec2( 0.0,  2.0));
    vec3 c = tap(uv + t * vec2( 2.0,  2.0));
    vec3 d = tap(uv + t * vec2(-2.0,  0.0));
    vec3 e = tap(uv);
    vec3 f = tap(uv + t * vec2( 2.0,  0.0));
    vec3 g = tap(uv + t * vec2(-2.0, -2.0));
    vec3 h = tap(uv + t * vec2( 0.0, -2.0));
    vec3 i = tap(uv + t * vec2( 2.0, -2.0));
    vec3 j = tap(uv + t * vec2(-1.0,  1.0));
    vec3 k = tap(uv + t * vec2( 1.0,  1.0));
    vec3 l = tap(uv + t * vec2(-1.0, -1.0));
    vec3 m = tap(uv + t * vec2( 1.0, -1.0));

    vec3 sum = e * 0.125
             + (a + c + g + i) * 0.03125
             + (b + d + f + h) * 0.0625
             + (j + k + l + m) * 0.125;

    if (texel_and_cfg.z > 0.0) {
        sum = bright_pass(sum, texel_and_cfg.z, texel_and_cfg.w);
    }
    frag_color = vec4(sum, 1.0);
}
@end

@fs fs_up
layout(binding=0) uniform texture2D u_src;
layout(binding=0) uniform sampler   u_smp;

layout(binding=0) uniform bloom_up_params {
    vec4 texel_and_radius;   // .xy = 1/src size, .z = tent radius in src texels, .w = level weight
    vec4 dst_texel;          // .xy = 1/dst (render target) size
};

in  vec2 v_uv;
out vec4 frag_color;

vec3 tap(vec2 uv) { return texture(sampler2D(u_src, u_smp), uv).rgb; }

void main() {
    vec2 d  = texel_and_radius.xy * texel_and_radius.z;
    vec2 uv = gl_FragCoord.xy * dst_texel.xy;

    // 3x3 tent: 1-2-1 / 2-4-2 / 1-2-1, normalised by 16.
    vec3 sum = tap(uv) * 4.0;
    sum += (tap(uv + vec2(-d.x, 0.0)) + tap(uv + vec2(d.x, 0.0))
          + tap(uv + vec2(0.0, -d.y)) + tap(uv + vec2(0.0, d.y))) * 2.0;
    sum += tap(uv + vec2(-d.x, -d.y)) + tap(uv + vec2(d.x, -d.y))
         + tap(uv + vec2(-d.x,  d.y)) + tap(uv + vec2(d.x,  d.y));

    frag_color = vec4(sum * (texel_and_radius.w / 16.0), 1.0);
}
@end

@program bloom_down vs fs_down
@program bloom_up   vs fs_up

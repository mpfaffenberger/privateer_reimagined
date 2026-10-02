// -----------------------------------------------------------------------------
// post_composite.glsl — final blit from offscreen scene to swapchain.
//
// Reads the scene color and the (already blurred) bloom texture, mixes them
// together, and additionally draws a procedural lens flare based on the
// sun's screen-space position. Output goes straight to the swapchain.
//
// The lens flare has three components, all additive:
//
//   * HALO    — a soft radial gaussian at the sun's projected position.
//               Mostly redundant with bloom, but keeps the flare intense
//               even when the sun disk is small/off-center.
//
//   * STREAKS — an anamorphic horizontal streak + a thinner vertical one
//               through the sun. This is the "J.J. Abrams" effect that
//               every sci-fi lens adds. Width is aspect-corrected so the
//               streak doesn't get fat when the window is wide.
//
//   * GHOSTS  — 4 small disks along the line from the sun through the
//               screen center, at fractional positions. Classic "junk
//               reflecting off lens elements" look.
//
// Visibility is scaled by `flare_intensity`, which the CPU sets to 0 when
// the sun is behind the camera or far off-screen (so no flare artifacts
// ghost in from outside the view).
//
// TONEMAP (#715): the scene arrives HDR (RGBA16F). Scene + bloom + flare
// are summed in linear HDR, scaled by exposure, then squeezed into [0,1]
// by a highlight shoulder. Everything below the knee passes through
// untouched, so the painted skybox / sprites look exactly as authored;
// only energy above it rolls off smoothly instead of clipping.
//
// GRADE (#726), in display space after the tonemap: a subtle vignette,
// light animated film grain, and +-0.5 LSB dither so the 8-bit write
// doesn't band the big dark nebula gradients.
// -----------------------------------------------------------------------------

@vs vs
out vec2 v_uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs fs
layout(binding=0) uniform texture2D u_scene;
layout(binding=1) uniform texture2D u_bloom;
layout(binding=0) uniform sampler   u_smp;

layout(binding=0) uniform post_composite_params {
    vec4 sun_ndc_and_flare;   // .xy = sun NDC [-1,1] (y up), .z = flare intensity, .w = aspect (w/h)
    vec4 tint_and_bloom;      // .rgb = flare tint, .a = bloom blend amount
    vec4 tone;                // .x = exposure, .y = shoulder knee, .z = 1 tonemap on / 0 off
    vec4 grade;               // .x = vignette, .y = grain, .z = 1 dither on, .w = grain frame seed
};

in  vec2 v_uv;
out vec4 frag_color;

vec3 procedural_flare(vec2 pixel_ndc, vec2 sun_ndc, float aspect, vec3 tint) {
    // Aspect-correct the ndc space so circles are actually circular.
    vec2 p = vec2(pixel_ndc.x * aspect, pixel_ndc.y);
    vec2 s = vec2(sun_ndc.x   * aspect, sun_ndc.y);
    vec2 d = p - s;

    // --- HALO ---------------------------------------------------------
    float halo_r = length(d);
    vec3 halo = tint * exp(-halo_r * 5.0) * 0.35;

    // --- STREAKS (anamorphic) -----------------------------------------
    // Horizontal streak: very thin in y, long in x. Vertical: opposite.
    float streak_h = exp(-abs(d.y) * 110.0) * exp(-abs(d.x) * 0.9);
    float streak_v = exp(-abs(d.x) * 200.0) * exp(-abs(d.y) * 1.3);
    vec3 streaks   = tint * (streak_h * 0.9 + streak_v * 0.45);

    // --- GHOSTS -------------------------------------------------------
    // Positions sampled along the line from sun through screen center,
    // in aspect-corrected ndc. Each ghost gets a different tint bias to
    // sell "chromatic glass elements."
    vec3 ghosts = vec3(0.0);
    float spacing[4];  spacing[0] = -0.35; spacing[1] = -0.7;
                       spacing[2] =  0.25; spacing[3] =  0.55;
    vec3  tints[4];
    tints[0] = vec3(1.0, 0.85, 0.55);
    tints[1] = vec3(1.0, 0.55, 0.35);
    tints[2] = vec3(0.85, 0.95, 1.0);
    tints[3] = vec3(0.6,  0.8,  1.0);
    for (int i = 0; i < 4; ++i) {
        vec2 gp = s * spacing[i];
        float gd = length(p - gp);
        ghosts += tints[i] * exp(-gd * 28.0) * 0.18;
    }

    return halo + streaks + ghosts;
}

// Film-style "white-hot": once a pixel's peak channel climbs well past 1.0
// its other channels start catching up, so a 4x-bright shield flash or the
// sun's heart reads as a white core inside its coloured glow. Colours only
// a little over 1.0 keep their hue.
vec3 white_hot(vec3 c) {
    float peak = max(max(c.r, c.g), c.b);
    float t    = clamp((peak - 1.5) / 3.0, 0.0, 1.0);
    return mix(c, vec3(peak), t * 0.4);
}

// Per-channel highlight shoulder: identity below `knee`, then an
// exponential approach to 1.0 with matching slope at the knee (C1, so no
// visible band where it kicks in). Per-channel on purpose: very hot
// colours drift toward white like film, which keeps the bleached-hot read
// the old RGBA8 clip gave the sun and explosions, minus the flat plateau.
vec3 shoulder(vec3 x, float knee) {
    float range = 1.0 - knee;
    vec3  over  = max(x - knee, 0.0);
    return min(x, vec3(knee)) + range * (1.0 - exp(-over / range));
}

// Interleaved gradient noise (Jimenez 2014): cheap, well-distributed
// per-pixel noise in [0,1), no texture needed.
float ign(vec2 px) {
    return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

vec3 apply_grade(vec3 c, vec2 uv, vec2 px, float aspect) {
    // Vignette: 0 at the centre, 1 in the corners, aspect-corrected so it
    // stays round on wide windows.
    vec2  q = vec2((uv.x - 0.5) * aspect, uv.y - 0.5);
    float d = length(q) / length(vec2(0.5 * aspect, 0.5));
    c *= 1.0 - grade.x * pow(d, 2.2);

    // Grain: multiplicative, so it lives in the image rather than floating
    // over black space as grey static.
    float n = ign(px + grade.w * vec2(5.588238, 3.191201)) - 0.5;
    c *= 1.0 + 2.0 * grade.y * n;

    // Dither: a different noise phase from the grain, +-0.5 of one 8-bit step.
    if (grade.z > 0.5) {
        c += (ign(px.yx + vec2(17.0, 31.0)) - 0.5) / 255.0;
    }
    return c;
}

void main() {
    vec3 scene = texture(sampler2D(u_scene, u_smp), v_uv).rgb;
    vec3 bloom = texture(sampler2D(u_bloom, u_smp), v_uv).rgb;

    vec2 pixel_ndc = v_uv * 2.0 - 1.0;
    vec2 sun_ndc   = sun_ndc_and_flare.xy;
    float flare_i  = sun_ndc_and_flare.z;
    float aspect   = sun_ndc_and_flare.w;

    vec3 flare = vec3(0.0);
    if (flare_i > 0.0) {
        flare = procedural_flare(pixel_ndc, sun_ndc, aspect, tint_and_bloom.rgb) * flare_i;
    }

    vec3 color = scene + bloom * tint_and_bloom.a + flare;
    if (tone.z > 0.5) {
        color = shoulder(white_hot(color * tone.x), tone.y);
    }
    color = apply_grade(color, v_uv, gl_FragCoord.xy, aspect);
    frag_color = vec4(color, 1.0);
}
@end

@program post_composite vs fs

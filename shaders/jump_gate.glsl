// -----------------------------------------------------------------------------
// jump_gate.glsl — black-hole jump portal (clean circle + subtle horizon warp).
//
// A camera-facing billboard quad renders a fake black hole at each jump nav
// point:
//   * a pure-black, opaque event-horizon disk in the centre (premultiplied
//     alpha = 1 -> the background is fully replaced, so it reads as a hole
//     punched in space). Its edge gets a TINY animated noise wobble so it
//     shimmers slightly instead of being a perfect circle;
//   * a bright HDR-blue photon ring hugging the horizon that bloom catches;
//   * a soft outer blue glow halo.
//   (No swirling ring pattern.)
//
// Blend is premultiplied "over" (SRC + DST*(1-SRC.a), set in jump_gate.cpp):
//   - horizon: emission 0, alpha 1            -> solid black
//   - ring/glow: emission = HDR colour, alpha = coverage -> blue light over bg
//   - outside: emission 0, alpha 0            -> background untouched
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform jg_vs_params {
    mat4 view_proj;
    vec4 world_pos;     // .xyz = gate centre, .w = radius (half-extent)
    vec4 cam_right;     // .xyz = camera right  (world)
    vec4 cam_up;        // .xyz = camera up     (world)
};

in vec2 a_quad;         // billboard corner in [-1,1]^2

out vec2 v_uv;

void main() {
    v_uv = a_quad;
    vec3 world = world_pos.xyz
               + cam_right.xyz * (a_quad.x * world_pos.w)
               + cam_up.xyz    * (a_quad.y * world_pos.w);
    gl_Position = view_proj * vec4(world, 1.0);
}
@end

@fs fs
layout(binding=1) uniform jg_fs_params {
    vec4 camera_pos;    // unused (kept for struct stability)
    vec4 tint;          // .rgb = glow colour, .a = intensity envelope
    vec4 anim;          // .x = time_sec, .y = pulse_slow rad/s,
                        // .z = pulse_fast rad/s, .w = unused
};

in  vec2 v_uv;
out vec4 frag_color;

// --- cheap value noise (only used for the subtle horizon-edge wobble) -------
float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i),                hash21(i + vec2(1.0, 0.0)), u.x),
               mix(hash21(i + vec2(0.0, 1.0)), hash21(i + vec2(1.0, 1.0)), u.x), u.y);
}

void main() {
    float t   = anim.x;
    vec2  p   = v_uv;
    float r   = length(p);
    if (r > 1.0) discard;                 // circular cutout
    float ang = atan(p.y, p.x);

    // ---- event horizon: clean circle + a TINY animated edge wobble -------
    float wobble  = vnoise(vec2(ang * 3.0 + t * 0.6, t * 0.4)) - 0.5;
    float horizon = 0.0136 + 0.0006 * wobble;   // tiny black core (~1/5 prior again)

    // ---- bright photon ring just outside the horizon ---------------------
    float photon = exp(-pow((r - (horizon + 0.05)) / 0.055, 2.0));

    // ---- outer glow halo --------------------------------------------------
    // ANNULAR glow: brightest in a ring at mid-radius and DIMMER toward the
    // centre, so the middle reads as deep blue instead of a blown-out white
    // core. Falls off to the rim too.
    float glow = exp(-pow((r - 0.42) / 0.30, 2.0)) * 0.55;
    glow *= smoothstep(horizon - 0.02, horizon + 0.10, r);   // outside horizon only
    // Extra centre knock-down so the very middle stays blue, not white.
    float center_dim = mix(0.30, 1.0, smoothstep(0.0, 0.40, r));
    glow *= center_dim;

    // ---- pulse ------------------------------------------------------------
    float pulse = mix(0.82, 1.18, 0.5 + 0.5 * sin(t * anim.y))
                * mix(0.92, 1.10, 0.5 + 0.5 * sin(t * anim.z + 1.7));

    // ---- compose (HDR blue so bloom blazes, but not enough to clip white) -
    // Saturated blue (low R/G, high B) so even when bright it reads blue
    // instead of white. Photon ring kept modest so the centre doesn't blow.
    vec3 ring_col = vec3(0.20, 0.45, 1.00) * 1.25;  // deep electric blue
    vec3 glow_col = tint.rgb * 1.4;

    vec3 emission = (ring_col * photon + glow_col * glow) * pulse * tint.a;

    // Inside the horizon: pure black + fully opaque so the disk reads as a
    // hole. smoothstep gives a thin feather on the rim (scaled to the now-
    // tiny core).
    float in_horizon = 1.0 - smoothstep(horizon - 0.0015, horizon + 0.0015, r);
    emission *= (1.0 - in_horizon);                 // no emission inside -> black

    float light = clamp((photon * 1.4 + glow) * pulse * tint.a, 0.0, 1.0);
    float alpha = max(in_horizon, light);

    frag_color = vec4(emission, alpha);
}
@end

@program jump_gate vs fs

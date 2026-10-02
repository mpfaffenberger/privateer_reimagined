//------------------------------------------------------------------------------
// sky_card.glsl — flat cards on the celestial sphere for the space backdrop
// (#693 galaxies/anomalies, #701 meteors + comets).
//
// Every program shares one vertex stage. Cards are drawn like the skybox:
// rotation-only view, z = w, so they sit on the far plane and every gameplay
// object paints over them. The CPU supplies the centre and the two (already
// rolled + scaled) half-axes; see sky_card.h. All programs are blended
// additively, so black adds nothing.
//------------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform vs_params {
    mat4 view_proj;   // projection * rotation-only view
    vec4 center;      // .xyz = card centre (on or near the unit sphere)
    vec4 axis_u;      // .xyz = half-extent along the card's local +U
    vec4 axis_v;      // .xyz = half-extent along the card's local +V
};

in vec2 a_corner;     // [-1, +1]^2
out vec2 v_uv;        // u: 0 at -U, 1 at +U.  v: 0 at +V, 1 at -V

void main() {
    vec3 p = center.xyz + a_corner.x * axis_u.xyz + a_corner.y * axis_v.xyz;
    v_uv = vec2(a_corner.x, -a_corner.y) * 0.5 + 0.5;
    gl_Position = (view_proj * vec4(p, 1.0)).xyww;
}
@end

// ---- textured prop: painted on black, radial fade hides the card edge ------
@fs fs_prop
layout(binding=0) uniform texture2D u_tex;
layout(binding=0) uniform sampler   u_smp;
layout(binding=1) uniform fs_params {
    vec4 tint;        // .rgb = brightness (intensity * pulse), .a unused
};

in  vec2 v_uv;
out vec4 frag_color;

void main() {
    float r    = length(v_uv - 0.5);
    float fade = 1.0 - smoothstep(0.40, 0.5, r);
    vec3  rgb  = texture(sampler2D(u_tex, u_smp), v_uv).rgb * tint.rgb * fade;
    frag_color = vec4(rgb, 0.0);
}
@end

// ---- meteor: thin streak, tail at u=0, head at u=1 --------------------------
@fs fs_meteor
layout(binding=1) uniform fs_params {
    vec4 tint;        // .rgb = colour * brightness
};

in  vec2 v_uv;
out vec4 frag_color;

void main() {
    float across = abs(v_uv.y - 0.5) * 2.0;
    float core   = exp(-across * across * 7.0);
    float trail  = v_uv.x * v_uv.x;
    float head   = smoothstep(0.88, 0.97, v_uv.x) * (1.0 - smoothstep(0.97, 1.0, v_uv.x));
    float k      = core * (trail + 1.5 * head) * (1.0 - smoothstep(0.985, 1.0, v_uv.x));
    frag_color   = vec4(tint.rgb * k, 0.0);
}
@end

// ---- comet: coma at u=0.12, dust + ion tails toward +U ----------------------
@fs fs_comet
layout(binding=1) uniform fs_params {
    vec4 tint;        // .rgb = brightness, .a = time (s) for the shimmer
};

in  vec2 v_uv;
out vec4 frag_color;

void main() {
    float t = tint.a;
    float x = v_uv.x - 0.12;          // distance down the tail
    float y = v_uv.y - 0.5;
    float xp = max(x, 0.0);

    // Coma: tight core plus a soft halo. Kept small and below white so
    // bloom doesn't turn it into a second sun.
    float r2   = x * x + y * y;
    float coma = 0.85 * exp(-r2 / 0.0004) + 0.22 * exp(-r2 / 0.004);

    // Dust tail: fans out and curves gently; faint radial striations drift.
    float yc     = 0.22 * xp * xp;
    float w      = 0.015 + 0.16 * xp;
    float dd     = (y - yc) / w;
    float stri   = 0.8 + 0.2 * sin(atan(y - yc, xp + 0.02) * 55.0 + t * 0.4);
    float dust   = exp(-dd * dd * 1.6) * exp(-xp * 2.4) * smoothstep(-0.02, 0.06, x) * stri;

    // Ion tail: straight, narrow, blue, with knots that stream outward.
    float wi   = 0.005 + 0.025 * xp;
    float ion  = exp(-(y / wi) * (y / wi)) * exp(-xp * 1.6) * smoothstep(-0.01, 0.04, x);
    ion       *= 0.75 + 0.25 * sin(xp * 38.0 - t * 1.7);

    // Fade at the card edges so nothing clips square.
    float edge = (1.0 - smoothstep(0.82, 1.0, v_uv.x)) * smoothstep(0.0, 0.06, v_uv.x)
               * (1.0 - smoothstep(0.38, 0.5, abs(y)));

    vec3 rgb = coma * vec3(1.0, 0.98, 0.92)
             + dust * vec3(1.0, 0.88, 0.68) * 0.75
             + ion  * vec3(0.45, 0.72, 1.0) * 0.6;
    frag_color = vec4(rgb * tint.rgb * edge, 0.0);
}
@end

@program sky_prop   vs fs_prop
@program sky_meteor vs fs_meteor
@program sky_comet  vs fs_comet

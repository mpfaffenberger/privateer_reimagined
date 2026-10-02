//------------------------------------------------------------------------------
// sky_prop.glsl — far-field galaxy / anomaly card on the celestial sphere
// (#693).
//
// Drawn like the skybox: rotation-only view, z = w so the card sits on the
// far plane and every gameplay object paints over it. The card is a quad in
// the plane tangent to the unit sphere at the prop's direction; the CPU
// supplies the centre and the two (already rolled + scaled) half-axes.
//
// The art is painted on pure black and blended additively, so black adds
// nothing. A radial fade guarantees the square card edge never shows.
//------------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform vs_params {
    mat4 view_proj;   // projection * rotation-only view
    vec4 center;      // .xyz = unit direction to the prop
    vec4 axis_u;      // .xyz = half-extent along the card's local +U
    vec4 axis_v;      // .xyz = half-extent along the card's local +V
};

in vec2 a_corner;     // [-1, +1]^2
out vec2 v_uv;

void main() {
    vec3 p = center.xyz + a_corner.x * axis_u.xyz + a_corner.y * axis_v.xyz;
    v_uv = vec2(a_corner.x, -a_corner.y) * 0.5 + 0.5;
    gl_Position = (view_proj * vec4(p, 1.0)).xyww;
}
@end

@fs fs
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

@program sky_prop vs fs

// -----------------------------------------------------------------------------
// jump_gate.glsl — pulsing translucent sphere wrapping a jump nav point.
//
// Drawn as an additive shell with CULL_NONE so both hemispheres contribute,
// giving a "you can see through to the back side" depth read without any
// real volumetric work. The fragment shader compresses three cheap cues
// into the silhouette:
//
//   1. Fresnel rim       — pow(1 - N·V, k); brighter at the silhouette,
//                          softer at the center, which makes a flat blue
//                          ball read as a 3D translucent shell.
//   2. Slow brightness   — sin(time * f_slow); the breathing pulse, 0.4..1.0
//      pulse                with period ~4 s. The "alive" cue.
//   3. Fast shimmer      — sin(time * f_fast); a small secondary modulation
//                          so the shell doesn't read as a single mechanical
//                          sine wave.
//
// Colour stays in the cool electric-blue family — matches the cyan nav-
// reticle so the player groks "the cyan dot in the HUD is THIS sphere" at
// a glance.
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform jg_vs_params {
    mat4 view_proj;
    vec4 world_pos;     // .xyz = gate centre, .w = radius
};

in vec3 a_pos;          // unit-sphere position (also serves as world normal
                        // after scaling, since the mesh is unit-radius)

out vec3 v_world_pos;
out vec3 v_normal;

void main() {
    vec3 world = world_pos.xyz + a_pos * world_pos.w;
    v_world_pos = world;
    v_normal    = normalize(a_pos);
    gl_Position = view_proj * vec4(world, 1.0);
}
@end

@fs fs
layout(binding=1) uniform jg_fs_params {
    vec4 camera_pos;    // .xyz = camera world pos
    vec4 tint;          // .rgb = base shell colour, .a = intensity envelope
    vec4 anim;          // .x = time_sec, .y = pulse_freq_slow,
                        // .z = pulse_freq_fast, .w = rim_exponent
};

in  vec3 v_world_pos;
in  vec3 v_normal;
out vec4 frag_color;

void main() {
    // CULL_NONE means we see both sides. The front side has N facing the
    // camera (N·V > 0); the back side has N facing away (N·V < 0). For the
    // shell to look symmetric (additive contributions from both halves)
    // we take abs(N·V) so the rim term reads the same on either face.
    vec3 N = normalize(v_normal);
    vec3 V = normalize(camera_pos.xyz - v_world_pos);
    float NdotV  = abs(dot(N, V));
    float rim    = pow(1.0 - NdotV, anim.w);   // bright at silhouette
    // Soft core glow so the centre isn't fully transparent; reads as "the
    // shell is filled with energy" rather than "hollow soap bubble".
    float core   = 0.18 * (1.0 - rim);

    // Two-frequency pulse: slow breath + fast shimmer.
    float t      = anim.x;
    float slow   = 0.5 + 0.5 * sin(t * anim.y);            // 0..1
    float fast   = 0.5 + 0.5 * sin(t * anim.z + 1.7);      // 0..1
    float pulse  = mix(0.55, 1.00, slow) * mix(0.85, 1.15, fast);

    float shell  = (rim + core) * pulse * tint.a;

    // Premultiplied-alpha additive composition: emission = colour * shell,
    // alpha follows shell so SRC + DST*(1-SRC.a) layers cleanly with the
    // scene behind. Slight bias on rim hue so the silhouette pulls toward
    // a hotter white-blue (like an arc-welder rim), centre stays deep
    // electric blue.
    vec3 rim_col  = vec3(0.65, 0.85, 1.00);    // hot blue-white
    vec3 core_col = tint.rgb;                  // base deep blue
    vec3 col      = mix(core_col, rim_col, rim);

    vec3 emission = col * shell;
    float alpha   = clamp(shell * 0.85, 0.0, 1.0);
    frag_color    = vec4(emission, alpha);
}
@end

@program jump_gate vs fs

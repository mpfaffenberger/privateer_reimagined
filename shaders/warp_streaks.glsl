// -----------------------------------------------------------------------------
// warp_streaks.glsl — autopilot "cruise streaks" overlay.
//
// Camera-tunnels-through-a-fixed-world approach (the classic "warp drive"
// look). Each particle's world position is snapped to the camera's nearest
// 2E-spaced lattice cell, so the streak is FIXED in world space while the
// camera is inside that cell — the cockpit POV sees it grow as the camera
// approaches and sweep past as the camera flies through. When the camera
// crosses a cell boundary the particle teleports to a fresh cell ahead (the
// 2E jump is invisible because per-particle alpha fades at the cube edge).
//
// Each streak has two vertices: head at the lattice cell, tail offset
// backward along -vel_dir by `streak_len_m * per_particle_jitter`. The
// jitter scrambles length AND brightness per-particle from a hash of a_pos,
// so the field reads as variety-of-streaks rather than identical lines.
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform vs_params {
    mat4 view_proj;
    vec4 cam_pos;        // .xyz = camera world pos, .w unused
    // .x = wrap_extent     (cube half-extent E around the camera)
    // .y = streak_len_m    (base world-space tail offset along -vel_dir)
    // .z = intensity       (0..1 master fade)
    // .w = unused
    vec4 field_params;
    vec4 vel_dir;        // .xyz = unit camera velocity (world); .w unused
};

in vec3  a_pos;   // random position in [-1, +1]^3 (shared by head and tail)
in float a_tip;   // 0.0 = head, 1.0 = tail

out float v_tip;       // pass-through for fs colour blend
out float v_alpha;     // edge-of-cube fade combined with intensity
out float v_bright;    // per-particle brightness jitter [0.4, 1.4]

// Cheap deterministic hash: maps a 3-vector to [0,1]. Used so each particle
// gets a different streak length / brightness without any per-vertex input.
float hash13(vec3 p) {
    p = fract(p * vec3(443.8975, 397.2973, 491.1871));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

void main() {
    float E = field_params.x;
    float L_base = field_params.y;
    float I = field_params.z;

    // ---- camera-following wrap (true parallax) ---------------------------
    // Each particle has a NOMINAL world position = a_pos * E (a fixed point
    // in world space, near the world origin). We then translate that
    // nominal point by the nearest 2E lattice offset that brings it within
    // E of the camera, using mod() so the offset shifts continuously as the
    // camera moves. Net effect: the particle is FIXED in world frame for as
    // long as it stays within E of the camera, then teleports by 2E to a
    // new cell when the camera flies past. The teleport happens exactly
    // when the per-particle distance hits E — same E we fade alpha to 0 at,
    // so the jump is invisible. This is what gives the cockpit POV true
    // "streaks slide past me as I fly forward" parallax.
    float cell = 2.0 * E;
    vec3 nominal     = a_pos * E;
    vec3 displaced   = cam_pos.xyz - nominal;
    vec3 wrapped     = mod(displaced + E, cell) - E;   // in [-E, +E]
    vec3 head_world  = cam_pos.xyz - wrapped;

    // ---- per-particle jitter ---------------------------------------------
    // hash on a_pos so each particle has a stable seed across frames.
    float h_len = hash13(a_pos);                 // [0,1]
    float h_brt = hash13(a_pos + vec3(7.3, 0.0, 0.0));
    float len_mul = mix(0.55, 1.55, h_len);      // [0.55, 1.55] streak length
    v_bright      = mix(0.40, 1.40, h_brt);      // [0.4, 1.4] brightness

    // Tail offset: backward along velocity (so streaks read as "light
    // trailing behind us"). Multiply by per-particle length jitter so
    // streaks come in short + long varieties instead of all identical.
    vec3 tail_offset = -vel_dir.xyz * (L_base * len_mul);
    vec3 world_pos   = head_world + tail_offset * a_tip;

    // ---- edge fade --------------------------------------------------------
    // Distance of head from the camera, normalised by E. Past ~75% of the
    // cube radius we fade smoothly to zero so the 2E teleport at cell change
    // is invisible. Both head + tail share this alpha (computed from head
    // offset only) so the line doesn't flicker as one end crosses the edge.
    vec3 rel = head_world - cam_pos.xyz;
    float edge = max(max(abs(rel.x), abs(rel.y)), abs(rel.z)) / E;
    v_alpha = clamp(1.0 - smoothstep(0.75, 1.0, edge), 0.0, 1.0) * I;

    v_tip = a_tip;
    gl_Position = view_proj * vec4(world_pos, 1.0);
}
@end

@fs fs
in  float v_tip;
in  float v_alpha;
in  float v_bright;
out vec4  frag_color;

void main() {
    // Head ~white-hot, tail ~electric cyan. lerp on v_tip.
    vec3 head_col = vec3(0.85, 0.95, 1.00);   // near-white with a blue cast
    vec3 tail_col = vec3(0.20, 0.55, 1.00);   // deep electric cyan
    vec3 col      = mix(head_col, tail_col, v_tip);

    // Tail end dims a little for direction-of-travel read; per-particle
    // brightness scales the whole streak so the field reads as varied.
    float fade = (1.0 - 0.5 * v_tip) * v_bright;
    frag_color = vec4(col * fade, v_alpha);
}
@end

@program warp_streaks vs fs
